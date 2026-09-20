// Collective Traffic Fabric - ctfctl: the operator command line tool.
// Copyright 2026 Summon Software Labs.
//
// ctfctl talks to a real ctf::CoordinatorServer over loopback TCP with the real
// wire protocol, and it can serve one itself, so every subcommand exercises the
// framing, handshake and envelope discipline a production peer must use.
// stdout carries the machine readable result, stderr carries diagnostics, and
// the exit code reports the outcome: 0 accepted, 1 refused by the coordinator,
// 2 the command line was wrong (in which case the usage goes to stderr).
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "ctf/catalog.hpp"
#include "ctf/collective.hpp"
#include "ctf/coordinator.hpp"
#include "ctf/decision.hpp"
#include "ctf/error.hpp"
#include "ctf/evidence.hpp"
#include "ctf/flow_group.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"
#include "ctf/participant.hpp"
#include "ctf/protocol.hpp"
#include "ctf/service.hpp"
#include "ctf/traffic.hpp"
#include "ctf/transport.hpp"

namespace {

using ctf::AlgorithmHint;
using ctf::BootIncarnation;
using ctf::CollectiveAttemptId;
using ctf::CollectiveClass;
using ctf::CollectiveDefinition;
using ctf::CollectiveInstance;
using ctf::CollectiveState;
using ctf::DecisionRecord;
using ctf::DecisionRequest;
using ctf::ErrorCode;
using ctf::FlowEdge;
using ctf::FlowGroupPlan;
using ctf::FlowPattern;
using ctf::Identity;
using ctf::IdentityMinter;
using ctf::NodeId;
using ctf::ParticipantId;
using ctf::ParticipantState;
using ctf::PolicyGeneration;
using ctf::RegistrationResult;
using ctf::Sequence;
using ctf::Status;
using ctf::to_string;
using ctf::transport::monotonic_now_ms;

// Exit codes are part of the tool's contract, not an implementation detail.
inline constexpr int kExitAccepted = 0;
inline constexpr int kExitRefused = 1;
inline constexpr int kExitUsage = 2;

// The loopback endpoint a coordinator is served on.  ctfctl never resolves a
// name: a tool that inspects a cluster must connect to exactly the address the
// operator stated, deterministically and without DNS.
inline constexpr const char* kHost = "127.0.0.1";

// The supervisor polling interval for shutdown requests.  The transport exposes
// the request as an atomic flag rather than as an event, so polling is the only
// available wait; the interval bounds how long a shutdown request waits.
inline constexpr std::uint64_t kShutdownPollMs = 20;

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
void emit(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// Writes text that is already line terminated exactly as it arrived; inspection
// and explanation bodies are contractually verbatim.
void emit_raw(const std::string& text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

void diagnose(const std::string& line) {
  std::fputs(line.c_str(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

[[nodiscard]] std::string usage_text() {
  return std::string(
      "usage: ctfctl <command> [options]\n"
      "\n"
      "commands:\n"
      "  serve     --port N [--snapshot PATH] [--label L]\n"
      "            serve a real coordinator until a peer requests shutdown\n"
      "  register  --port N --class NAME [--participant HEX]... [--bytes N] [--label L] [--barrier]\n"
      "            register a durable collective definition\n"
      "  begin-attempt --port N --collective HEX --generation HEX [--attempt HEX] [--bytes N]\n"
      "            open the attempt a plan must be bound to\n"
      "  plan      --port N --collective HEX --generation HEX --attempt HEX --pattern NAME\n"
      "            [--step N] [--participant HEX]...\n"
      "            request a flow group decision for a ring, chain, full_mesh or tree_up plan\n"
      "  inspect   --port N [--subject summary|collective|decisions|peers|policy|topology]\n"
      "            [--collective HEX] [--offset N] [--limit N]\n"
      "            send an inspection request and print the body verbatim\n"
      "  explain   --port N [--collective HEX --attempt HEX | --correlation N]\n"
      "            print the explanation of a retained decision\n"
      "  heartbeat --port N --participant HEX --incarnation HEX [--node HEX]\n"
      "            [--state live|suspect|expired] [--count N] [--interval-ms N]\n"
      "            publish participant liveness and print the acknowledged expiry\n"
      "  install-policy --port N [--standard]\n"
      "            install the standard traffic policy under a fresh generation\n"
      "  evidence  --port N --kind congestion|capacity|topology [--link HEX]... [--node HEX]...\n"
      "            [--utilization-bps N] [--available-bps N] [--valid-for-ms N]\n"
      "            ingest one observation; values typed on a command line are SYNTHETIC\n"
      "  selftest  run the in-process SYNTHETIC end to end path\n"
      "  shutdown  --port N\n"
      "            ask a running coordinator to stop accepting new work\n"
      "\n"
      "exit codes: 0 accepted, 1 refused by the coordinator, 2 usage error\n");
}

void print_usage(std::FILE* stream) {
  const std::string text = usage_text();
  std::fputs(text.c_str(), stream);
  std::fflush(stream);
}

[[nodiscard]] int usage_error(const std::string& message) {
  // A stable prefix, so an operator or a test can parse the diagnostic whatever
  // path the binary happened to be invoked through.
  diagnose(std::string("ctfctl: ") + message);
  print_usage(stderr);
  return kExitUsage;
}

// Every Status is checked before a decoded value is used; this is the one place
// that turns a failed Status into an operator visible line.
[[nodiscard]] int report_failure(const char* what, const Status& status) {
  diagnose(std::string(what) + ": " + std::string(to_string(status.code())) + " " + status.message());
  return kExitRefused;
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------
// Strict option parsing.  An unknown option, a missing value or an option that
// takes no value is a usage error rather than something silently ignored, so a
// typo can never change what the tool asks a coordinator to do.
class Options {
 public:
  [[nodiscard]] static bool parse(const std::vector<std::string>& arguments, Options& out,
                                  std::string& error) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const std::string& token = arguments[index];
      if (token.size() < 3 || token[0] != '-' || token[1] != '-') {
        error = "unexpected argument '" + token + "'";
        return false;
      }
      std::string name = token;
      std::string value;
      bool has_value = false;
      const std::size_t equals = token.find('=');
      if (equals != std::string::npos) {
        name = token.substr(0, equals);
        value = token.substr(equals + 1);
        has_value = true;
      }
      if (!is_known(name)) {
        error = "unknown option " + name;
        return false;
      }
      const bool is_flag = takes_no_value(name);
      if (is_flag) {
        if (has_value) {
          error = name + " does not take a value";
          return false;
        }
      } else if (!has_value) {
        if (index + 1 >= arguments.size() || starts_with_option(arguments[index + 1])) {
          error = name + " requires a value";
          return false;
        }
        value = arguments[++index];
      }
      out.entries_.emplace_back(std::move(name), std::move(value));
    }
    return true;
  }

  [[nodiscard]] bool has(std::string_view name) const {
    return find(name) != nullptr;
  }

  [[nodiscard]] const std::string* find(std::string_view name) const {
    for (const auto& entry : entries_) {
      if (entry.first == name) return &entry.second;
    }
    return nullptr;
  }

  // Every occurrence, in command line order.  Repeated options carry a list
  // (participants, links, nodes) rather than overwriting each other.
  [[nodiscard]] std::vector<std::string> all(std::string_view name) const {
    std::vector<std::string> values;
    for (const auto& entry : entries_) {
      if (entry.first == name) values.push_back(entry.second);
    }
    return values;
  }

  [[nodiscard]] std::string value_or(std::string_view name, const std::string& fallback) const {
    const std::string* found = find(name);
    return found != nullptr ? *found : fallback;
  }

 private:
  [[nodiscard]] static bool starts_with_option(const std::string& token) {
    return token.size() >= 2 && token[0] == '-' && token[1] == '-';
  }

  [[nodiscard]] static bool takes_no_value(std::string_view name) {
    return name == "--barrier" || name == "--standard" || name == "--help";
  }

  [[nodiscard]] static bool is_known(std::string_view name) {
    static const std::string_view kKnown[] = {
        "--port",       "--snapshot",     "--label",         "--class",      "--participant",
        "--bytes",      "--barrier",      "--collective",    "--generation", "--attempt",
        "--pattern",    "--step",         "--subject",       "--offset",     "--limit",
        "--correlation", "--incarnation", "--node",          "--state",      "--count",
        "--interval-ms", "--kind",        "--link",          "--utilization-bps",
        "--available-bps", "--valid-for-ms", "--standard", "--help"};
    for (const std::string_view known : kKnown) {
      if (known == name) return true;
    }
    return false;
  }

  std::vector<std::pair<std::string, std::string>> entries_;
};

// Strict unsigned decimal parse: an empty value, a sign, or trailing junk is an
// error rather than a silent prefix parse.
[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out) {
  if (text.empty()) return false;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  std::uint64_t value = 0;
  const std::from_chars_result result = std::from_chars(first, last, value);
  if (result.ec != std::errc() || result.ptr != last) return false;
  out = value;
  return true;
}

[[nodiscard]] bool resolve_u64(const Options& options, std::string_view name, std::uint64_t fallback,
                               std::uint64_t& out, std::string& error) {
  const std::string* text = options.find(name);
  if (text == nullptr) {
    out = fallback;
    return true;
  }
  if (!parse_u64(*text, out)) {
    error = std::string(name) + " must be an unsigned decimal number";
    return false;
  }
  return true;
}

[[nodiscard]] bool resolve_port(const Options& options, bool allow_ephemeral, std::uint16_t& out,
                                std::string& error) {
  const std::string* text = options.find("--port");
  if (text == nullptr) {
    error = "--port is required";
    return false;
  }
  std::uint64_t value = 0;
  if (!parse_u64(*text, value) || value > 65535 || (!allow_ephemeral && value == 0)) {
    error = allow_ephemeral ? "--port must be a port number in 0..65535"
                            : "--port must be a port number in 1..65535";
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

// Identities are only ever accepted in the canonical spelling the runtime
// prints, so a copied identity and a typed one cannot disagree.
[[nodiscard]] bool resolve_identity(const Options& options, std::string_view name, Identity& out,
                                    std::string& error) {
  const std::string* text = options.find(name);
  if (text == nullptr) {
    error = std::string(name) + " is required";
    return false;
  }
  if (!Identity::parse(*text, out)) {
    error = std::string(name) + " '" + *text + "' is not canonical 32 character hexadecimal";
    return false;
  }
  return true;
}

[[nodiscard]] bool resolve_optional_identity(const Options& options, std::string_view name, Identity& out,
                                             bool& present, std::string& error) {
  const std::string* text = options.find(name);
  if (text == nullptr) {
    present = false;
    return true;
  }
  if (!Identity::parse(*text, out)) {
    error = std::string(name) + " '" + *text + "' is not canonical 32 character hexadecimal";
    return false;
  }
  present = true;
  return true;
}

[[nodiscard]] bool resolve_participants(const Options& options, std::vector<Identity>& out,
                                        std::string& error) {
  out.clear();
  for (const std::string& text : options.all("--participant")) {
    Identity id;
    if (!Identity::parse(text, id)) {
      error = "--participant '" + text + "' is not canonical 32 character hexadecimal";
      return false;
    }
    out.push_back(id);
  }
  if (out.empty()) {
    error = "at least one --participant is required";
    return false;
  }
  return true;
}

// Maps the three liveness states an operator may publish.  Every other state is
// a usage error: liveness is authority, so it is never guessed.
[[nodiscard]] bool parse_participant_state(const std::string& text, ParticipantState& out) {
  if (text == "live") {
    out = ParticipantState::kLive;
    return true;
  }
  if (text == "suspect") {
    out = ParticipantState::kSuspect;
    return true;
  }
  if (text == "expired") {
    out = ParticipantState::kExpired;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Protocol client
// ---------------------------------------------------------------------------
// Turns a wire error response into a Status that carries the coordinator's own
// code plus its whole reason chain, so no caller has to interpret a bare code.
[[nodiscard]] Status refusal_from(const std::vector<std::uint8_t>& payload) {
  ctf::protocol::ErrorPayload error;
  const Status decoded = ctf::protocol::decode_error(payload, error);
  if (!decoded.is_ok()) return decoded;
  std::string message = std::string(to_string(error.code));
  message += ": ";
  message += error.message;
  if (!error.reasons.empty()) {
    message += " reasons=[";
    message += error.reasons.to_string();
    message += "]";
  }
  return Status(error.code, message);
}

// One strict protocol session: the handshake, the per session frame sequence,
// the session identity on every frame and the error response contract live here
// exactly once, and every subcommand reuses them.
class Client {
 public:
  Client() = default;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client() { connection_.shutdown_socket(); }

  [[nodiscard]] Status connect(std::uint16_t port) {
    ctf::transport::Endpoint endpoint;
    endpoint.host = kHost;
    endpoint.port = port;
    Status status = ctf::transport::Connection::connect_to(endpoint, connection_);
    if (!status.is_ok()) return status;

    ctf::protocol::HelloPayload hello;
    hello.client_label = "ctfctl";
    hello.client_version = "ctfctl/1.0.0";
    hello.client_boot_monotonic_ms = monotonic_now_ms();
    std::vector<std::uint8_t> payload;
    status = ctf::protocol::encode_hello(hello, payload);
    if (!status.is_ok()) return status;

    // The hello carries no session identity: the coordinator mints it and the
    // acknowledgement is the only place it ever comes from.
    ctf::protocol::Frame frame;
    frame.kind = static_cast<std::uint16_t>(ctf::protocol::MessageKind::kHello);
    frame.flags = ctf::protocol::kFlagRequest;
    frame.payload = std::move(payload);
    status = connection_.send_frame(frame);
    if (!status.is_ok()) return status;

    ctf::protocol::Frame response;
    status = connection_.receive_frame(response);
    if (!status.is_ok()) return status;
    if (static_cast<ctf::protocol::MessageKind>(response.kind) != ctf::protocol::MessageKind::kHelloAck) {
      return Status(ErrorCode::kEnvelopeSessionUnknown,
                    "the coordinator did not acknowledge the hello");
    }
    status = ctf::protocol::decode_hello_ack(response.payload, hello_);
    if (!status.is_ok()) return status;
    connected_ = true;
    return Status::ok();
  }

  [[nodiscard]] const ctf::protocol::HelloAckPayload& hello() const noexcept { return hello_; }

  // The correlation value of the last request, so a later process can ask the
  // coordinator to explain exactly that request.
  [[nodiscard]] std::uint64_t correlation() const noexcept { return correlation_; }

  [[nodiscard]] Status request(ctf::protocol::MessageKind kind, const std::vector<std::uint8_t>& payload,
                               ctf::protocol::MessageKind& response_kind,
                               std::vector<std::uint8_t>& response_payload) {
    Status status = send_request(kind, payload);
    if (!status.is_ok()) return status;
    ctf::protocol::Frame response;
    status = connection_.receive_frame(response);
    if (!status.is_ok()) return status;
    response_kind = static_cast<ctf::protocol::MessageKind>(response.kind);
    response_payload = std::move(response.payload);
    if (response_kind != ctf::protocol::MessageKind::kErrorResponse) return Status::ok();
    return refusal_from(response_payload);
  }

  // The acknowledgement carries the coordinator's own view of the participant,
  // including the expiry instant it recorded, so a peer learns what authority it
  // actually holds rather than assuming that sending a message established it.
  [[nodiscard]] Status heartbeat(const ctf::protocol::HeartbeatPayload& payload,
                                 ctf::protocol::HeartbeatAckPayload& out) {
    std::vector<std::uint8_t> body;
    Status status = ctf::protocol::encode_heartbeat(payload, body);
    if (!status.is_ok()) return status;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    status = request(ctf::protocol::MessageKind::kHeartbeat, body, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kHeartbeatAck) {
      return Status(ErrorCode::kInternalInvariant, "unexpected heartbeat response");
    }
    return ctf::protocol::decode_heartbeat_ack(response, out);
  }

  [[nodiscard]] Status install_policy(const ctf::TrafficPolicy& policy,
                                      ctf::protocol::InstallPolicyAckPayload& out) {
    ctf::protocol::InstallPolicyPayload payload;
    payload.policy = policy;
    std::vector<std::uint8_t> body;
    Status status = ctf::protocol::encode_install_policy(payload, body);
    if (!status.is_ok()) return status;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    status = request(ctf::protocol::MessageKind::kInstallPolicy, body, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kInstallPolicyAck) {
      return Status(ErrorCode::kInternalInvariant, "unexpected policy response");
    }
    return ctf::protocol::decode_install_policy_ack(response, out);
  }

  [[nodiscard]] Status register_collective(const CollectiveDefinition& definition,
                                           ctf::protocol::RegisterAckPayload& out) {
    ctf::protocol::RegisterCollectivePayload payload;
    payload.definition = definition;
    std::vector<std::uint8_t> body;
    Status status = ctf::protocol::encode_register_collective(payload, body);
    if (!status.is_ok()) return status;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    status = request(ctf::protocol::MessageKind::kRegisterCollective, body, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kRegisterAck) {
      return Status(ErrorCode::kInternalInvariant, "unexpected registration response");
    }
    return ctf::protocol::decode_register_ack(response, out);
  }

  [[nodiscard]] Status begin_attempt(Identity id, Identity generation, Identity attempt,
                                     std::uint64_t transfer_bytes,
                                     ctf::protocol::AttemptAckPayload& out) {
    ctf::protocol::BeginAttemptPayload payload;
    payload.id = id;
    payload.generation = generation;
    payload.attempt = attempt;
    payload.transfer_bytes = transfer_bytes;
    std::vector<std::uint8_t> body;
    Status status = ctf::protocol::encode_begin_attempt(payload, body);
    if (!status.is_ok()) return status;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    status = request(ctf::protocol::MessageKind::kBeginAttempt, body, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kAttemptAck) {
      return Status(ErrorCode::kInternalInvariant, "unexpected attempt response");
    }
    return ctf::protocol::decode_attempt_ack(response, out);
  }

  [[nodiscard]] Status plan_flow_group(DecisionRequest request_value, DecisionRecord& out) {
    ctf::protocol::PlanFlowGroupPayload payload;
    // The request names the session the coordinator minted, never a value the
    // tool chose.
    request_value.session = hello_.session;
    payload.request = request_value;
    std::vector<std::uint8_t> body;
    Status status = ctf::protocol::encode_plan_flow_group(payload, body);
    if (!status.is_ok()) return status;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    status = request(ctf::protocol::MessageKind::kPlanFlowGroup, body, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kFlowGroupDecision) {
      return Status(ErrorCode::kInternalInvariant, "unexpected decision response");
    }
    ctf::protocol::DecisionPayload decision;
    status = ctf::protocol::decode_decision(response, decision);
    if (!status.is_ok()) return status;
    out = std::move(decision.decision);
    return Status::ok();
  }

  [[nodiscard]] Status inspect(std::uint8_t subject, Identity id, std::uint32_t offset,
                               std::uint32_t limit, std::string& body) {
    ctf::protocol::InspectRequestPayload payload;
    payload.subject = subject;
    payload.id = id;
    payload.offset = offset;
    payload.limit = limit;
    std::vector<std::uint8_t> encoded;
    Status status = ctf::protocol::encode_inspect_request(payload, encoded);
    if (!status.is_ok()) return status;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    status = request(ctf::protocol::MessageKind::kInspectRequest, encoded, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kInspectResponse) {
      return Status(ErrorCode::kInternalInvariant, "unexpected inspection response");
    }
    ctf::protocol::InspectResponsePayload decoded;
    status = ctf::protocol::decode_inspect_response(response, decoded);
    if (!status.is_ok()) return status;
    if (decoded.truncated) {
      diagnose("ctfctl: the coordinator truncated the inspection body");
    }
    body = std::move(decoded.body);
    return Status::ok();
  }

  [[nodiscard]] Status explain(Identity id, CollectiveAttemptId attempt, std::uint64_t correlation,
                               bool by_correlation, ctf::protocol::ExplainResponsePayload& out) {
    ctf::protocol::ExplainRequestPayload payload;
    payload.id = id;
    payload.attempt = attempt;
    payload.correlation = correlation;
    payload.has_correlation = by_correlation;
    std::vector<std::uint8_t> encoded;
    Status status = ctf::protocol::encode_explain_request(payload, encoded);
    if (!status.is_ok()) return status;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    status = request(ctf::protocol::MessageKind::kExplainRequest, encoded, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kExplainResponse) {
      return Status(ErrorCode::kInternalInvariant, "unexpected explanation response");
    }
    return ctf::protocol::decode_explain_response(response, out);
  }

  [[nodiscard]] Status ingest_evidence(const ctf::protocol::EvidencePayload& payload,
                                       ctf::protocol::EvidenceAckPayload& out) {
    std::vector<std::uint8_t> body;
    Status status = ctf::protocol::encode_evidence(payload, body);
    if (!status.is_ok()) return status;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    status = request(ctf::protocol::MessageKind::kIngestEvidence, body, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kEvidenceAck) {
      return Status(ErrorCode::kInternalInvariant, "unexpected evidence response");
    }
    return ctf::protocol::decode_evidence_ack(response, out);
  }

  [[nodiscard]] Status request_shutdown() {
    const std::vector<std::uint8_t> empty;
    ctf::protocol::MessageKind response_kind = ctf::protocol::MessageKind::kInvalid;
    std::vector<std::uint8_t> response;
    const Status status = request(ctf::protocol::MessageKind::kShutdown, empty, response_kind, response);
    if (!status.is_ok()) return status;
    if (response_kind != ctf::protocol::MessageKind::kShutdownAck) {
      return Status(ErrorCode::kInternalInvariant, "unexpected shutdown response");
    }
    return Status::ok();
  }

 private:
  [[nodiscard]] Status send_request(ctf::protocol::MessageKind kind,
                                    const std::vector<std::uint8_t>& payload) {
    if (!connected_) {
      return Status(ErrorCode::kTransportNotStarted, "the session is not connected");
    }
    ++sequence_;
    ++correlation_;
    ctf::protocol::Frame frame;
    frame.kind = static_cast<std::uint16_t>(kind);
    frame.flags = ctf::protocol::kFlagRequest;
    frame.envelope.session = hello_.session;
    frame.envelope.incarnation = hello_.incarnation;
    frame.envelope.epoch = hello_.epoch;
    frame.envelope.frame_sequence = Sequence(sequence_);
    frame.envelope.correlation = correlation_;
    frame.payload = payload;
    return connection_.send_frame(frame);
  }

  ctf::transport::Connection connection_;
  ctf::protocol::HelloAckPayload hello_{};
  std::uint64_t sequence_ = 0;
  std::uint64_t correlation_ = 0;
  bool connected_ = false;
};

// Connects and reports the failure in the caller's own terms.
[[nodiscard]] int connect_or_report(Client& client, std::uint16_t port) {
  const Status status = client.connect(port);
  if (status.is_ok()) return kExitAccepted;
  return report_failure("connect", status);
}

// ---------------------------------------------------------------------------
// Synthetic fabric for the in-process self test
// ---------------------------------------------------------------------------
// A two rack cluster with oversubscribed inter-rack uplinks.  Everything here
// is SYNTHETIC: it models a fabric, it does not measure one, and the synthetic
// flag travels with the evidence into the coordinator.
inline constexpr std::uint64_t kSyntheticFabricSeed = 0x5EEDFAB1C0000001ull;
inline constexpr std::uint64_t kSyntheticParticipantSeed = 0x5EEDFAB1C0000002ull;
inline constexpr std::uint64_t kSyntheticLinkBps = 400ull * 1000 * 1000 * 1000ull;
inline constexpr std::uint32_t kSyntheticRackCount = 2;
inline constexpr std::uint32_t kSyntheticNodesPerRack = 2;
inline constexpr std::uint32_t kSyntheticUtilizationBps = 2000;
inline constexpr std::uint32_t kSyntheticAvailablePerMille = 750;
inline constexpr std::uint64_t kSyntheticCongestionWindowMs = 2000;
inline constexpr std::uint64_t kSyntheticLogicalBytes = 1ull << 30;
inline constexpr std::uint64_t kSyntheticEdgeBytes = 1ull << 28;

struct SyntheticFabric {
  ctf::TopologyEvidence topology;
  ctf::CapacityEvidence capacity;
  ctf::CongestionEvidence congestion;
  std::vector<NodeId> nodes;
};

[[nodiscard]] SyntheticFabric make_synthetic_fabric(std::uint64_t seed, std::uint64_t now_ms) {
  IdentityMinter ids(seed);
  SyntheticFabric fabric;
  std::vector<Identity> racks;
  for (std::uint32_t index = 0; index < kSyntheticRackCount; ++index) {
    racks.push_back(ids.next());
  }
  std::vector<Identity> switches;
  for (std::uint32_t index = 0; index < kSyntheticRackCount + 1; ++index) {
    switches.push_back(ids.next());
  }
  for (std::uint32_t rack = 0; rack < kSyntheticRackCount; ++rack) {
    for (std::uint32_t node = 0; node < kSyntheticNodesPerRack; ++node) {
      ctf::TopologyNode entry;
      entry.id = ids.next();
      entry.rack = racks[rack];
      entry.accelerator_attached = true;
      entry.accelerator_count = 8;
      fabric.topology.nodes.push_back(entry);
      fabric.nodes.push_back(entry.id);
    }
  }
  for (std::uint32_t rack = 0; rack < kSyntheticRackCount; ++rack) {
    ctf::TopologyLink link;
    link.id = ids.next();
    link.a = switches[rack];
    link.b = switches.back();
    link.link_class = ctf::LinkClass::kInterRack;
    link.capacity_bps = kSyntheticLinkBps;
    link.lanes = 4;
    link.shared_oversubscribed = true;
    fabric.topology.links.push_back(link);
  }
  fabric.topology.generation = ids.next();
  fabric.topology.captured_monotonic_ms = now_ms;
  fabric.topology.synthetic = true;
  fabric.topology.canonicalize();

  fabric.capacity.generation = ids.next();
  fabric.capacity.topology_generation = fabric.topology.generation;
  fabric.capacity.captured_monotonic_ms = now_ms;
  fabric.capacity.synthetic = true;
  for (const ctf::TopologyLink& link : fabric.topology.links) {
    ctf::LinkCapacity entry;
    entry.link = link.id;
    entry.provisioned_bps = link.capacity_bps;
    entry.available_bps = (link.capacity_bps / 1000ull) * kSyntheticAvailablePerMille;
    entry.utilization_bps = kSyntheticUtilizationBps;
    entry.oversubscribed = true;
    entry.oversubscription_ratio_per_mille = 4000;
    fabric.capacity.links.push_back(entry);
  }
  fabric.capacity.canonicalize();

  fabric.congestion.generation = ids.next();
  fabric.congestion.topology_generation = fabric.topology.generation;
  fabric.congestion.captured_monotonic_ms = now_ms;
  fabric.congestion.valid_for_ms = kSyntheticCongestionWindowMs;
  fabric.congestion.synthetic = true;
  for (const ctf::TopologyLink& link : fabric.topology.links) {
    ctf::LinkCongestion entry;
    entry.link = link.id;
    entry.utilization_bps = kSyntheticUtilizationBps;
    entry.queue_depth_bytes = 0;
    entry.ecn_marks_per_mille = 0;
    entry.pfc_pause_per_mille = 0;
    entry.level = ctf::congestion_level_from_utilization_bps(kSyntheticUtilizationBps);
    fabric.congestion.links.push_back(entry);
  }
  fabric.congestion.canonicalize();
  return fabric;
}

// ---------------------------------------------------------------------------
// Flow group plans
// ---------------------------------------------------------------------------
// Builds the requested pattern over the participants exactly as they were
// listed: a ring, a chain and a tree are defined by that order, so a different
// argument order is a different plan.  Edges carry no declared volume, because
// ctfctl does not invent a transfer size the operator never stated, and the
// runtime reads an undeclared volume as zero rather than as "no traffic".
[[nodiscard]] FlowGroupPlan build_plan(FlowPattern pattern, CollectiveClass collective_class,
                                       const CollectiveInstance& instance,
                                       const std::vector<Identity>& participants, Sequence step) {
  FlowGroupPlan plan;
  plan.instance = instance;
  // The class is stated for a caller that knows it; the coordinator stamps it
  // from the durable definition regardless, so kUnknown here is never a claim
  // about semantics.
  plan.collective_class = collective_class;
  plan.pattern = pattern;
  plan.direction = ctf::FlowDirection::kUnidirectional;
  plan.plan_sequence = Sequence(1);
  const std::size_t count = participants.size();
  switch (pattern) {
    case FlowPattern::kRing: {
      plan.requires_simultaneous_start = true;
      for (std::size_t index = 0; index < count; ++index) {
        FlowEdge edge;
        edge.source = participants[index];
        edge.destination = participants[(index + 1) % count];
        edge.step_index = step;
        edge.hint = AlgorithmHint::kRing;
        plan.edges.push_back(edge);
      }
      break;
    }
    case FlowPattern::kChain: {
      for (std::size_t index = 0; index + 1 < count; ++index) {
        FlowEdge edge;
        edge.source = participants[index];
        edge.destination = participants[index + 1];
        edge.step_index = step;
        edge.hint = AlgorithmHint::kPipeline;
        plan.edges.push_back(edge);
      }
      break;
    }
    case FlowPattern::kFullMesh: {
      for (std::size_t source = 0; source < count; ++source) {
        for (std::size_t destination = 0; destination < count; ++destination) {
          if (source == destination) continue;
          FlowEdge edge;
          edge.source = participants[source];
          edge.destination = participants[destination];
          edge.step_index = step;
          edge.hint = AlgorithmHint::kDirect;
          plan.edges.push_back(edge);
        }
      }
      break;
    }
    case FlowPattern::kTreeUp: {
      for (std::size_t index = 1; index < count; ++index) {
        FlowEdge edge;
        edge.source = participants[index];
        edge.destination = participants[(index - 1) / 2];
        edge.step_index = step;
        edge.hint = AlgorithmHint::kTree;
        plan.edges.push_back(edge);
      }
      break;
    }
    default:
      break;
  }
  return plan;
}

// ---------------------------------------------------------------------------
// serve
// ---------------------------------------------------------------------------
int run_serve(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, true, port, error)) return usage_error(error);

  ctf::CoordinatorConfig config;
  config.transport.bind_host = kHost;
  config.transport.port = port;
  const std::string* snapshot = options.find("--snapshot");
  if (snapshot != nullptr) config.snapshot_path = *snapshot;
  config.service.coordinator_label = options.value_or("--label", "ctfctl");

  ctf::CoordinatorServer server(config);
  Status status = server.start(monotonic_now_ms());
  if (!status.is_ok()) return report_failure("serve", status);
  emit(std::string("listening ") + config.transport.bind_host + ":" + std::to_string(server.port()));
  emit("epoch " + server.service().epoch().to_string());

  // Polling is the only available wait: the transport publishes the request as
  // an atomic flag, not as an event, and the interval only bounds how long a
  // shutdown request waits behind this loop.
  while (!server.shutdown_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownPollMs));
  }
  diagnose("ctfctl: a peer requested shutdown");
  status = server.stop(monotonic_now_ms());
  if (!status.is_ok()) return report_failure("stop", status);
  return kExitAccepted;
}

// ---------------------------------------------------------------------------
// register
// ---------------------------------------------------------------------------
[[nodiscard]] bool registration_accepted(std::uint8_t result) {
  return result == static_cast<std::uint8_t>(RegistrationResult::kRegistered) ||
         result == static_cast<std::uint8_t>(RegistrationResult::kGenerationAdvanced) ||
         result == static_cast<std::uint8_t>(RegistrationResult::kUnchanged);
}

int run_register(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);
  const std::string* class_text = options.find("--class");
  if (class_text == nullptr) return usage_error("--class is required");
  CollectiveClass collective_class = CollectiveClass::kUnknown;
  if (!ctf::collective_class_from_string(*class_text, collective_class)) {
    return usage_error("--class '" + *class_text + "' is not a collective class name");
  }
  std::vector<Identity> participants;
  if (!resolve_participants(options, participants, error)) return usage_error(error);
  ctf::canonicalize_participants(participants);
  std::uint64_t bytes = 0;
  if (!resolve_u64(options, "--bytes", 0, bytes, error)) return usage_error(error);

  CollectiveDefinition definition;
  definition.id = ctf::mint_identity();
  definition.collective_class = collective_class;
  definition.label = options.value_or("--label", *class_text);
  definition.participants = participants;
  definition.logical_bytes = bytes;
  definition.declares_barrier_semantics = options.has("--barrier");

  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;
  ctf::protocol::RegisterAckPayload ack;
  const Status status = client.register_collective(definition, ack);
  if (!status.is_ok()) return report_failure("register", status);

  emit("id " + ack.id.to_string());
  emit("generation " + ack.generation.to_string());
  emit("result " + std::string(to_string(static_cast<RegistrationResult>(ack.result))));
  if (!ack.reasons.empty()) emit("reasons " + ack.reasons.to_string());
  return registration_accepted(ack.result) ? kExitAccepted : kExitRefused;
}

// ---------------------------------------------------------------------------
// begin-attempt
// ---------------------------------------------------------------------------
// Opens the attempt a decision must be bound to.  The coordinator never
// authorizes a plan against an attempt it does not consider current, so without
// this step no remote plan can be admitted however good the plan is.
int run_begin_attempt(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);
  Identity collective;
  if (!resolve_identity(options, "--collective", collective, error)) return usage_error(error);
  Identity generation;
  if (!resolve_identity(options, "--generation", generation, error)) return usage_error(error);
  Identity attempt;
  if (options.has("--attempt")) {
    if (!resolve_identity(options, "--attempt", attempt, error)) return usage_error(error);
  } else {
    // The attempt identity is the tool's to mint; it is printed so a later plan
    // can be bound to exactly this attempt.
    attempt = ctf::mint_identity();
  }
  std::uint64_t bytes = 0;
  if (!resolve_u64(options, "--bytes", 0, bytes, error)) return usage_error(error);

  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;
  ctf::protocol::AttemptAckPayload ack;
  const Status status = client.begin_attempt(collective, generation, attempt, bytes, ack);
  if (!status.is_ok()) return report_failure("begin-attempt", status);
  emit("attempt " + ack.attempt.to_string());
  emit("previous_attempt " +
       (ack.previous_attempt.is_some() ? ack.previous_attempt.to_string() : std::string("none")));
  emit("state " + std::string(to_string(ack.state)));
  if (!ack.reasons.empty()) emit("reasons " + ack.reasons.to_string());
  // The catalog moves the collective to kPlanning exactly when the attempt
  // became current; every other state means the attempt was refused.
  return ack.state == CollectiveState::kPlanning ? kExitAccepted : kExitRefused;
}

// ---------------------------------------------------------------------------
// plan
// ---------------------------------------------------------------------------
int run_plan(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);
  Identity collective;
  if (!resolve_identity(options, "--collective", collective, error)) return usage_error(error);
  Identity generation;
  if (!resolve_identity(options, "--generation", generation, error)) return usage_error(error);
  Identity attempt;
  if (!resolve_identity(options, "--attempt", attempt, error)) return usage_error(error);

  const std::string* pattern_text = options.find("--pattern");
  if (pattern_text == nullptr) return usage_error("--pattern is required");
  FlowPattern pattern = FlowPattern::kUnspecified;
  if (!ctf::flow_pattern_from_string(*pattern_text, pattern)) {
    return usage_error("--pattern '" + *pattern_text + "' is not a flow pattern name");
  }
  if (pattern != FlowPattern::kRing && pattern != FlowPattern::kChain &&
      pattern != FlowPattern::kFullMesh && pattern != FlowPattern::kTreeUp) {
    return usage_error("--pattern '" + *pattern_text + "' is not buildable; use ring, chain, full_mesh or tree_up");
  }
  std::vector<Identity> participants;
  if (!resolve_participants(options, participants, error)) return usage_error(error);
  std::uint64_t step = 1;
  if (!resolve_u64(options, "--step", 1, step, error)) return usage_error(error);
  if (step == 0 || step > ctf::limits::kStepsMaxPerPhase) {
    return usage_error("--step must be in 1.." + std::to_string(ctf::limits::kStepsMaxPerPhase));
  }

  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;

  DecisionRequest request;
  request.instance.id = collective;
  request.instance.generation = generation;
  request.instance.attempt = attempt;
  // The requester states the generation set the handshake reported, which is
  // exactly what this session believes is current; anything else would be a
  // stale request by construction.
  request.observed_epoch = client.hello().epoch;
  request.observed_policy_generation = client.hello().policy_generation;
  request.observed_topology_generation = client.hello().topology_generation;
  request.observed_capacity_generation = client.hello().capacity_generation;
  request.observed_congestion_generation = client.hello().congestion_generation;
  request.has_flow_group_plan = true;
  request.plan = build_plan(pattern, CollectiveClass::kUnknown, request.instance, participants,
                            Sequence(step));

  DecisionRecord decision;
  const Status status = client.plan_flow_group(request, decision);
  if (!status.is_ok()) return report_failure("plan", status);

  emit("correlation " + std::to_string(client.correlation()));
  emit("summary " + decision.summary());
  emit("explanation:");
  emit_raw(decision.explain());
  return ctf::outcome_grants_authority(decision.outcome) ? kExitAccepted : kExitRefused;
}

// ---------------------------------------------------------------------------
// inspect
// ---------------------------------------------------------------------------
[[nodiscard]] bool inspect_subject(const std::string& name, std::uint8_t& out) {
  struct Entry {
    const char* name;
    std::uint8_t value;
  };
  static const Entry kSubjects[] = {
      {"summary", 0}, {"collective", 1}, {"decisions", 2}, {"peers", 3}, {"policy", 4}, {"topology", 5}};
  for (const Entry& entry : kSubjects) {
    if (name == entry.name) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

int run_inspect(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);

  std::uint8_t subject = 0;
  const std::string* subject_text = options.find("--subject");
  if (subject_text != nullptr && !inspect_subject(*subject_text, subject)) {
    return usage_error("--subject '" + *subject_text +
                       "' is not one of summary, collective, decisions, peers, policy, topology");
  }
  Identity collective;
  bool has_collective = false;
  if (!resolve_optional_identity(options, "--collective", collective, has_collective, error)) {
    return usage_error(error);
  }
  if (subject == 1 && !has_collective) {
    return usage_error("--subject collective requires --collective");
  }
  std::uint64_t offset = 0;
  if (!resolve_u64(options, "--offset", 0, offset, error)) return usage_error(error);
  std::uint64_t limit = 64;
  if (!resolve_u64(options, "--limit", 64, limit, error)) return usage_error(error);
  if (offset > 0xFFFFFFFFull || limit > 0xFFFFFFFFull) {
    return usage_error("--offset and --limit must fit in 32 bits");
  }

  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;
  std::string body;
  const Status status = client.inspect(subject, collective, static_cast<std::uint32_t>(offset),
                                       static_cast<std::uint32_t>(limit), body);
  if (!status.is_ok()) return report_failure("inspect", status);
  emit_raw(body);
  return kExitAccepted;
}

// ---------------------------------------------------------------------------
// explain
// ---------------------------------------------------------------------------
int run_explain(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);

  Identity collective;
  bool has_collective = false;
  if (!resolve_optional_identity(options, "--collective", collective, has_collective, error)) {
    return usage_error(error);
  }
  Identity attempt;
  bool has_attempt = false;
  if (!resolve_optional_identity(options, "--attempt", attempt, has_attempt, error)) {
    return usage_error(error);
  }
  std::uint64_t correlation = 0;
  bool has_correlation = false;
  if (!resolve_u64(options, "--correlation", 0, correlation, error)) return usage_error(error);
  has_correlation = options.has("--correlation");
  if (has_correlation && (has_collective || has_attempt)) {
    return usage_error("--correlation cannot be combined with --collective or --attempt");
  }
  if (!has_correlation && !(has_collective && has_attempt)) {
    return usage_error("either --correlation, or both --collective and --attempt, are required");
  }

  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;
  ctf::protocol::ExplainResponsePayload response;
  const Status status = client.explain(collective, attempt, correlation, has_correlation, response);
  if (!status.is_ok()) return report_failure("explain", status);
  emit_raw(response.explanation);
  return response.found ? kExitAccepted : kExitRefused;
}

// ---------------------------------------------------------------------------
// heartbeat
// ---------------------------------------------------------------------------
int run_heartbeat(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);
  Identity participant;
  if (!resolve_identity(options, "--participant", participant, error)) return usage_error(error);
  Identity incarnation;
  if (!resolve_identity(options, "--incarnation", incarnation, error)) return usage_error(error);
  Identity node;
  bool has_node = false;
  if (!resolve_optional_identity(options, "--node", node, has_node, error)) return usage_error(error);

  ParticipantState state = ParticipantState::kLive;
  const std::string* state_text = options.find("--state");
  if (state_text != nullptr && !parse_participant_state(*state_text, state)) {
    return usage_error("--state '" + *state_text + "' is not live, suspect or expired");
  }
  std::uint64_t count = 1;
  if (!resolve_u64(options, "--count", 1, count, error)) return usage_error(error);
  if (count == 0) return usage_error("--count must be at least 1");
  std::uint64_t interval_ms = 0;
  if (!resolve_u64(options, "--interval-ms", 0, interval_ms, error)) return usage_error(error);
  if (interval_ms > 3600000ull) return usage_error("--interval-ms must not exceed one hour");

  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;

  for (std::uint64_t index = 0; index < count; ++index) {
    ctf::protocol::HeartbeatPayload payload;
    payload.participant = participant;
    payload.incarnation = incarnation;
    payload.node = node;
    payload.state = state;
    payload.client_clock_ms = monotonic_now_ms();
    ctf::protocol::HeartbeatAckPayload ack;
    const Status status = client.heartbeat(payload, ack);
    if (!status.is_ok()) return report_failure("heartbeat", status);
    if (!ack.accepted) {
      diagnose("ctfctl: the heartbeat was not accepted: " + ack.reasons.to_string());
      return kExitRefused;
    }
    emit("heartbeat " + std::to_string(index + 1) + " participant=" + ack.participant.to_string() +
         " state=" + std::string(to_string(ack.state)) +
         " expires_at_monotonic_ms=" + std::to_string(ack.expires_at_monotonic_ms));
    // A stated interval is a pacing request from the operator, not a
    // synchronization wait: nothing is being awaited.
    if (index + 1 < count && interval_ms != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<std::int64_t>(interval_ms)));
    }
  }
  if (!has_node) diagnose("ctfctl: no --node was stated; the participant keeps its previous node");
  return kExitAccepted;
}

// ---------------------------------------------------------------------------
// evidence
// ---------------------------------------------------------------------------
int run_evidence(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);
  const std::string* kind = options.find("--kind");
  if (kind == nullptr) return usage_error("--kind is required");
  if (*kind != "congestion" && *kind != "capacity" && *kind != "topology") {
    return usage_error("--kind '" + *kind + "' is not congestion, capacity or topology");
  }

  std::vector<Identity> links;
  for (const std::string& text : options.all("--link")) {
    Identity id;
    if (!Identity::parse(text, id)) {
      return usage_error("--link '" + text + "' is not canonical 32 character hexadecimal");
    }
    links.push_back(id);
  }
  std::vector<Identity> nodes;
  for (const std::string& text : options.all("--node")) {
    Identity id;
    if (!Identity::parse(text, id)) {
      return usage_error("--node '" + text + "' is not canonical 32 character hexadecimal");
    }
    nodes.push_back(id);
  }
  std::uint64_t utilization_bps = 0;
  if (!resolve_u64(options, "--utilization-bps", 0, utilization_bps, error)) return usage_error(error);
  if (utilization_bps > 10000) return usage_error("--utilization-bps must be in 0..10000");
  std::uint64_t available_bps = 0;
  if (!resolve_u64(options, "--available-bps", 0, available_bps, error)) return usage_error(error);
  std::uint64_t valid_for_ms = kSyntheticCongestionWindowMs;
  if (!resolve_u64(options, "--valid-for-ms", kSyntheticCongestionWindowMs, valid_for_ms, error)) {
    return usage_error(error);
  }

  const bool topology = *kind == "topology";
  if (topology && !links.empty()) {
    return usage_error("--link describes capacity or congestion, not a topology");
  }
  if (!topology && !nodes.empty()) {
    return usage_error("--node describes a topology, not capacity or congestion");
  }
  if (topology && nodes.empty()) {
    return usage_error("--kind topology requires at least one --node");
  }
  if (!topology && links.empty()) {
    return usage_error("--kind " + *kind + " requires at least one --link");
  }
  if (!topology && valid_for_ms == 0) {
    return usage_error("--valid-for-ms must be at least 1");
  }

  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;

  // Every value below was typed on a command line rather than measured, so the
  // observation is labelled synthetic: the runtime never claims physical
  // validation for evidence it was merely handed.
  ctf::protocol::EvidencePayload payload;
  if (topology) {
    payload.kind = ctf::protocol::EvidenceKindTag::kTopology;
    payload.topology.generation = ctf::mint_identity();
    payload.topology.captured_monotonic_ms = monotonic_now_ms();
    payload.topology.synthetic = true;
    for (const Identity& node : nodes) {
      ctf::TopologyNode entry;
      entry.id = node;
      // The operator stated an identity and nothing about attached endpoints.
      entry.accelerator_attached = false;
      entry.accelerator_count = 0;
      payload.topology.nodes.push_back(entry);
    }
  } else {
    const Identity topology_generation = client.hello().topology_generation;
    if (!topology_generation.is_some()) {
      diagnose("ctfctl: no topology is installed; the observation will be refused");
    }
    if (*kind == "capacity") {
      payload.kind = ctf::protocol::EvidenceKindTag::kCapacity;
      payload.capacity.generation = ctf::mint_identity();
      payload.capacity.topology_generation = topology_generation;
      payload.capacity.captured_monotonic_ms = monotonic_now_ms();
      payload.capacity.synthetic = true;
      for (const Identity& link : links) {
        ctf::LinkCapacity entry;
        entry.link = link;
        // Availability is what the operator observed; the provisioned figure is
        // stated as the same observation because a zero provisioned link is not
        // a legal claim and ctfctl never invents headroom.
        entry.available_bps = available_bps;
        entry.provisioned_bps = available_bps == 0 ? 1 : available_bps;
        entry.utilization_bps = static_cast<std::uint32_t>(utilization_bps);
        entry.oversubscribed = false;
        entry.oversubscription_ratio_per_mille = 1000;
        payload.capacity.links.push_back(entry);
      }
    } else {
      payload.kind = ctf::protocol::EvidenceKindTag::kCongestion;
      payload.congestion.generation = ctf::mint_identity();
      payload.congestion.topology_generation = topology_generation;
      payload.congestion.captured_monotonic_ms = monotonic_now_ms();
      payload.congestion.valid_for_ms = valid_for_ms;
      payload.congestion.synthetic = true;
      for (const Identity& link : links) {
        ctf::LinkCongestion entry;
        entry.link = link;
        entry.utilization_bps = static_cast<std::uint32_t>(utilization_bps);
        entry.queue_depth_bytes = 0;
        entry.ecn_marks_per_mille = 0;
        entry.pfc_pause_per_mille = 0;
        entry.level = ctf::congestion_level_from_utilization_bps(static_cast<std::uint32_t>(utilization_bps));
        payload.congestion.links.push_back(entry);
      }
    }
  }

  ctf::protocol::EvidenceAckPayload ack;
  const Status status = client.ingest_evidence(payload, ack);
  if (!status.is_ok()) return report_failure("evidence", status);
  emit("kind " + *kind);
  emit("synthetic true");
  emit(std::string("result ") + (ack.result == 0 ? "accepted" : "rejected"));
  if (ack.generation.is_some()) emit("generation " + ack.generation.to_string());
  if (!ack.reasons.empty()) emit("reasons " + ack.reasons.to_string());
  return ack.result == 0 ? kExitAccepted : kExitRefused;
}

// ---------------------------------------------------------------------------
// install-policy
// ---------------------------------------------------------------------------
// Installs the standard traffic policy under a freshly minted generation.  This
// is what makes an admission reachable for a remote peer at all: with no policy
// installed every decision is refused on the policy axis, whatever the plan is.
int run_install_policy(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);
  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;

  const ctf::TrafficPolicy policy = ctf::TrafficPolicy::standard(ctf::mint_identity());
  ctf::protocol::InstallPolicyAckPayload ack;
  const Status status = client.install_policy(policy, ack);
  if (!status.is_ok()) return report_failure("install-policy", status);
  emit("generation " + ack.generation.to_string());
  emit(std::string("result ") + (ack.result == 0 ? "installed" : "rejected"));
  emit("class_policies " + std::to_string(policy.class_policies.size()) + " collective_policies " +
       std::to_string(policy.collective_policies.size()));
  if (!ack.reasons.empty()) emit("reasons " + ack.reasons.to_string());
  return ack.result == 0 ? kExitAccepted : kExitRefused;
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
int run_shutdown(const Options& options) {
  std::uint16_t port = 0;
  std::string error;
  if (!resolve_port(options, false, port, error)) return usage_error(error);
  Client client;
  const int connected = connect_or_report(client, port);
  if (connected != kExitAccepted) return connected;
  const Status status = client.request_shutdown();
  if (!status.is_ok()) return report_failure("shutdown", status);
  emit("shutdown acknowledged");
  return kExitAccepted;
}

// ---------------------------------------------------------------------------
// selftest
// ---------------------------------------------------------------------------
// The whole supported path in process, with no peer to blame: a service, the
// standard policy, a synthetic fabric, one live participant, one registered
// collective, one attempt and one ring flow group.  It exits 0 only when the
// coordinator actually admits the flow group.
int run_selftest() {
  // One monotonic reading drives every call, so the evidence ages the decision
  // sees are exactly zero instead of depending on how this process was
  // scheduled.
  const std::uint64_t now = monotonic_now_ms();
  const SyntheticFabric fabric = make_synthetic_fabric(kSyntheticFabricSeed, now);
  emit("fabric SYNTHETIC racks=" + std::to_string(kSyntheticRackCount) + " nodes=" +
       std::to_string(fabric.nodes.size()) + " links=" + std::to_string(fabric.topology.links.size()));

  ctf::CoordinatorService service;
  Status status = service.start(now);
  if (!status.is_ok()) return report_failure("start", status);
  emit("coordinator started");

  const PolicyGeneration policy_generation = ctf::mint_identity();
  if (service.authority().install_policy(ctf::TrafficPolicy::standard(policy_generation)) !=
      ctf::InstallResult::kInstalled) {
    diagnose("ctfctl: the standard policy was refused");
    return kExitRefused;
  }
  emit("policy installed generation " + policy_generation.to_string());

  if (service.authority().install_topology(fabric.topology) != ctf::InstallResult::kInstalled) {
    diagnose("ctfctl: the synthetic topology was refused");
    return kExitRefused;
  }
  emit("topology installed generation " + fabric.topology.generation.to_string() + " SYNTHETIC");

  if (service.authority().install_capacity(fabric.capacity) != ctf::InstallResult::kInstalled) {
    diagnose("ctfctl: the synthetic capacity observation was refused");
    return kExitRefused;
  }
  emit("capacity installed generation " + fabric.capacity.generation.to_string() + " SYNTHETIC");

  if (service.authority().install_congestion(fabric.congestion) != ctf::InstallResult::kInstalled) {
    diagnose("ctfctl: the synthetic congestion observation was refused");
    return kExitRefused;
  }
  emit("congestion installed generation " + fabric.congestion.generation.to_string() + " SYNTHETIC");

  ctf::SessionBinding binding;
  status = service.open_session("ctfctl-selftest", now, binding);
  if (!status.is_ok()) return report_failure("session", status);
  ctf::ServiceRequestContext context;
  context.session = binding.id;
  context.incarnation = binding.incarnation;
  context.epoch = binding.epoch;
  context.now_monotonic_ms = now;
  context.correlation = 1;

  IdentityMinter participants_ids(kSyntheticParticipantSeed);
  std::vector<Identity> participants;
  for (const NodeId& node : fabric.nodes) {
    const ParticipantId participant = participants_ids.next();
    const BootIncarnation incarnation = participants_ids.next();
    status = service.publish_participant(context, participant, incarnation, node, ParticipantState::kLive);
    if (!status.is_ok()) return report_failure("publish", status);
    participants.push_back(participant);
    emit("participant " + participant.to_string() + " published node " + node.to_string() + " SYNTHETIC");
  }

  CollectiveDefinition definition;
  definition.id = ctf::mint_identity();
  definition.collective_class = CollectiveClass::kAllReduce;
  definition.algorithm_hint = AlgorithmHint::kRing;
  definition.label = "ctfctl-selftest-all-reduce";
  definition.participants = participants;
  // Store the canonical order so the definition is the same for every peer that
  // states the same membership.
  ctf::canonicalize_participants(definition.participants);
  definition.logical_bytes = kSyntheticLogicalBytes;

  ctf::RegistrationOutcome registration;
  status = service.register_collective(context, definition, registration);
  if (!status.is_ok()) return report_failure("register", status);
  if (registration.result != RegistrationResult::kRegistered) {
    diagnose(std::string("ctfctl: registration was refused: ") +
             std::string(to_string(registration.result)));
    return kExitRefused;
  }
  emit("collective registered id " + definition.id.to_string() + " generation " +
       registration.generation.to_string() + " class all_reduce");

  const CollectiveAttemptId attempt = ctf::mint_identity();
  ctf::AttemptOutcome attempt_outcome;
  status = service.begin_attempt(context, definition.id, registration.generation, attempt,
                                 kSyntheticLogicalBytes, attempt_outcome);
  if (!status.is_ok()) return report_failure("attempt", status);
  emit("attempt begun attempt " + attempt.to_string() + " state " +
       std::string(to_string(attempt_outcome.state)));

  DecisionRequest request;
  request.instance.id = definition.id;
  request.instance.generation = registration.generation;
  request.instance.attempt = attempt;
  request.observed_epoch = service.authority().epoch();
  request.observed_policy_generation = service.authority().policy_generation();
  request.observed_topology_generation = service.authority().topology_generation();
  request.observed_capacity_generation = service.authority().capacity_generation();
  request.observed_congestion_generation = service.authority().congestion_generation();
  request.has_flow_group_plan = true;
  request.plan = build_plan(FlowPattern::kRing, CollectiveClass::kAllReduce, request.instance,
                            definition.participants, Sequence(1));

  DecisionRecord decision;
  status = service.plan_flow_group(context, request, decision);
  if (!status.is_ok()) return report_failure("plan", status);
  emit("decision " + decision.summary());
  const bool admitted = decision.outcome == ctf::Outcome::kAdmitted;
  emit(std::string("result ") + (admitted ? "ADMITTED" : "NOT_ADMITTED"));

  const Status stopped = service.stop(monotonic_now_ms());
  if (!stopped.is_ok()) return report_failure("stop", stopped);
  return admitted ? kExitAccepted : kExitRefused;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(stderr);
    return kExitUsage;
  }
  const std::string command = argv[1];
  if (command == "--help" || command == "-h" || command == "help") {
    print_usage(stderr);
    return kExitUsage;
  }
  const std::vector<std::string> arguments(argv + 2, argv + argc);
  Options options;
  std::string error;
  if (!Options::parse(arguments, options, error)) return usage_error(error);
  if (options.has("--help")) {
    print_usage(stderr);
    return kExitUsage;
  }
  if (command == "serve") return run_serve(options);
  if (command == "register") return run_register(options);
  if (command == "begin-attempt") return run_begin_attempt(options);
  if (command == "plan") return run_plan(options);
  if (command == "inspect") return run_inspect(options);
  if (command == "explain") return run_explain(options);
  if (command == "heartbeat") return run_heartbeat(options);
  if (command == "evidence") return run_evidence(options);
  if (command == "install-policy") return run_install_policy(options);
  if (command == "selftest") return run_selftest();
  if (command == "shutdown") return run_shutdown(options);
  return usage_error("unknown command '" + command + "'");
}
