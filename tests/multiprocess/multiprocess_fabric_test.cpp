// Collective Traffic Fabric - multiprocess proof suite.
// Copyright 2026 Summon Software Labs.
//
// Every case here drives real, independent operating system processes over real
// loopback TCP.  The parent process never reads a child's console: participant
// and incarnation identities are minted in the parent and handed to the children
// on their command lines, and every observation is made through the protocol
// against the live coordinator.  All fabric evidence in this file is SYNTHETIC:
// it models a two rack fabric with oversubscribed uplinks, and no physical
// fabric is measured and no hardware claim is made anywhere below.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "ctf/catalog.hpp"
#include "ctf/collective.hpp"
#include "ctf/decision.hpp"
#include "ctf/evidence.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"
#include "ctf/participant.hpp"
#include "ctf/persistence.hpp"
#include "ctf/protocol.hpp"
#include "ctf/traffic.hpp"
#include "ctf/transport.hpp"
#include "support/client_session.hpp"
#include "support/process_helper.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;
// The helper processes, the strict protocol client and the synthetic fabric
// builders all live in ctf::test, which is as much a part of this suite's API as
// the library itself.
using namespace ctf::test;

// Bounded polling budgets.  Every wait in this file polls a real observable
// condition; these bounds exist only so that a coordinator which never reaches
// the expected state fails the case with a description instead of hanging the
// suite.  The pause between attempts yields the processor to a child that is
// still starting.
constexpr std::uint32_t kConnectAttempts = 600;
constexpr std::uint32_t kObservationAttempts = 1200;
constexpr std::uint32_t kExitAttempts = 3000;
// How long a child is given to finish on its own before the guard terminates it.
constexpr std::uint32_t kReapGraceAttempts = 300;
constexpr std::uint32_t kPollIntervalMs = 10;

// ---------------------------------------------------------------------------
// Child process lifetime and child output
// ---------------------------------------------------------------------------

// The directory temporary files of this suite live in.
std::filesystem::path scratch_directory() {
  std::error_code error;
  std::filesystem::path directory = std::filesystem::temp_directory_path(error);
  if (error) {
    error.clear();
    directory = std::filesystem::current_path(error);
    if (error) directory = std::filesystem::path(".");
  }
  return directory;
}

// A private log file for one child process.  Child output is redirected into its
// own file rather than inherited into the console, so the suite's output stays in
// one ordered stream and a failing case can quote the child's own evidence.  The
// name carries a minted identity so concurrent suites and cases never share one.
std::string child_log_path(const std::string& label) {
  return (scratch_directory() / ("ctf_child_log_" + label + "_" + mint_identity().to_string() + ".log"))
      .string();
}

// The tail of a child's log, for a failure message.  A missing or empty log is
// reported as such instead of being silently dropped.
std::string log_tail(const std::string& path, std::size_t max_bytes = 1500) {
  if (path.empty()) return std::string("<no log file>");
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::string("<no log file at ") + path + ">";
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (text.empty()) return std::string("<empty log at ") + path + ">";
  if (text.size() <= max_bytes) return text;
  return std::string("...") + text.substr(text.size() - max_bytes);
}

// Owns one helper process and reaps it unconditionally, so an assertion that
// unwinds a case can never leak a process that would keep a port bound and
// corrupt every later case.
class ChildGuard {
 public:
  ChildGuard() = default;
  ~ChildGuard() {
    reap();
    // The batch file only exists to start the child; the child's log is kept so a
    // failing case can still quote its evidence.
    if (!batch_path_.empty()) {
      std::error_code error;
      std::filesystem::remove(batch_path_, error);
    }
  }
  ChildGuard(const ChildGuard&) = delete;
  ChildGuard& operator=(const ChildGuard&) = delete;

  // redirect_logs names the case where the child's streams must land in its own
  // file.  A child that will be killed abruptly is started directly instead, so
  // that kill_process() terminates exactly the process whose boot incarnation the
  // case is about, rather than a shell that fronts it.
  void start(const std::vector<std::string>& arguments, const std::string& label, bool redirect_logs = true) {
    log_path_ = child_log_path(label);
#if defined(_WIN32)
    const std::string shell = read_environment("ComSpec");
    if (redirect_logs && !shell.empty()) {
      // A batch file, not a command line, carries the redirection.  The shared
      // process helper quotes every argument it is given, and the command
      // processor does not parse a quoted /c switch or a quoted redirection
      // operator, so the redirection is expressed in cmd's own language where its
      // quoting rules are defined.  This keeps every child's evidence in one
      // private file and keeps child lines out of the suite's output; the suite
      // never needed a child's stdout, because every identity a child publishes
      // was minted here and handed over on the child's command line.
      batch_path_ = log_path_ + ".bat";
      std::ofstream batch(batch_path_, std::ios::binary);
      CTF_CHECK_MSG(batch.good(), "cannot write the child batch file " + batch_path_);
      batch << "@echo off\r\n";
      for (const std::string& argument : arguments) {
        batch << '"' << argument << "\" ";
      }
      batch << "> \"" << log_path_ << "\" 2>&1\r\n";
      batch.close();
      CTF_CHECK_OK(spawn_process({shell, "/c", batch_path_}, true, process_));
      started_ = true;
      return;
    }
#else
    static_cast<void>(redirect_logs);
#endif
    CTF_CHECK_OK(spawn_process(arguments, true, process_));
    started_ = true;
  }

  // Terminates the child abruptly: it runs no shutdown path of its own, which is
  // the death mode the fencing proof requires.
  void kill() {
    CTF_CHECK_MSG(!reaped_, "the child process was already reaped");
    CTF_CHECK_OK(kill_process(process_));
  }

  // Waits for a child that is expected to end on its own and reports its exit
  // code.  A case that expects a clean exit treats a kill as a defect.
  int wait_for_exit() {
    CTF_CHECK_MSG(!reaped_, "the child process was already reaped");
    int exit_code = -1;
    CTF_CHECK_OK(wait_process(process_, exit_code));
    reaped_ = true;
    return exit_code;
  }

  [[nodiscard]] bool running() const { return !reaped_ && process_is_running(process_); }
  // Reported in failure diagnostics so a stuck child can be identified.
  [[nodiscard]] int process_id() const noexcept { return process_.process_id; }
  // The child's own output, quoted into a failure message.
  [[nodiscard]] std::string log() const { return log_tail(log_path_); }

 private:
  void reap() {
    if (!started_ || reaped_) return;
    // A child that is fronted by the command processor cannot be reached by
    // kill_process(), which stops the shell and would leave the helper running.
    // Every child is therefore given a bounded moment to finish on its own first:
    // a participant ends as soon as its next heartbeat is refused, and the shell
    // leaves with it.  Only a child that is still alive after that is terminated.
    for (std::uint32_t attempt = 0; attempt < kReapGraceAttempts && process_is_running(process_); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    if (process_is_running(process_)) {
      static_cast<void>(kill_process(process_));
    }
    int exit_code = 0;
    static_cast<void>(wait_process(process_, exit_code));
    reaped_ = true;
  }

  ChildProcess process_{};
  std::string log_path_;
  std::string batch_path_;
  bool started_ = false;
  bool reaped_ = false;
};

// ---------------------------------------------------------------------------
// Temporary files
// ---------------------------------------------------------------------------

// A uniquely named snapshot path in the system temporary directory, removed when
// the case ends however it ends.  The persistence layer replaces a snapshot
// through a "<path>.tmp" sibling, so both names are cleaned up and both are
// checked by the lifecycle case.
class TemporarySnapshot {
 public:
  explicit TemporarySnapshot(const std::string& tag)
      : prefix_("ctf_multiprocess_" + tag + "_" + mint_identity().to_string()) {
    std::error_code error;
    std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error) {
      error.clear();
      directory = std::filesystem::current_path(error);
      if (error) directory = std::filesystem::path(".");
    }
    path_ = (directory / (prefix_ + ".snapshot")).string();
  }
  ~TemporarySnapshot() { remove(); }
  TemporarySnapshot(const TemporarySnapshot&) = delete;
  TemporarySnapshot& operator=(const TemporarySnapshot&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] bool exists() const { return file_exists(path_); }
  [[nodiscard]] bool replacement_sibling_exists() const { return file_exists(path_ + ".tmp"); }

  // Every remaining file whose name carries this snapshot's unique prefix.  An
  // empty result is the proof that the case left nothing behind.
  [[nodiscard]] std::vector<std::string> leftovers() const {
    std::vector<std::string> found;
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::path(path_).parent_path();
    std::filesystem::directory_iterator iterator(directory, error);
    const std::filesystem::directory_iterator finish;
    while (!error && iterator != finish) {
      const std::string name = iterator->path().filename().string();
      if (name.rfind(prefix_, 0) == 0) found.push_back(name);
      iterator.increment(error);
    }
    return found;
  }

  void remove() const {
    std::error_code error;
    std::filesystem::remove(path_, error);
    error.clear();
    std::filesystem::remove(path_ + ".tmp", error);
  }

 private:
  static bool file_exists(const std::string& path) {
    std::error_code error;
    return std::filesystem::exists(path, error);
  }

  std::string prefix_;
  std::string path_;
};

// ---------------------------------------------------------------------------
// Coordinator and participant plumbing
// ---------------------------------------------------------------------------

// Reserves a loopback port.  The reservation is released before the child binds
// it, which is the documented way to hand a child a port nobody else selected.
std::uint16_t reserve_port() {
  std::uint16_t port = 0;
  CTF_CHECK_OK(transport::reserve_ephemeral_port(port));
  CTF_CHECK_MSG(port != 0, "the operating system returned port zero");
  return port;
}

// Starts the coordinator helper.  Only the cases that prove durable state across
// a restart hand it a snapshot path; every other case keeps the coordinator in
// memory and configures it entirely over the protocol.
void start_coordinator(ChildGuard& coordinator, std::uint16_t port, const std::string& snapshot_path) {
  const std::string executable = coordinator_executable();
  CTF_CHECK_MSG(!executable.empty(), "the coordinator helper path was not injected by the build");
  std::vector<std::string> arguments{executable, "--port", std::to_string(port), "--label",
                                     "multiprocess-coordinator"};
  if (!snapshot_path.empty()) {
    arguments.push_back("--snapshot");
    arguments.push_back(snapshot_path);
  }
  coordinator.start(arguments, "coordinator");
}

// Connects a typed session to a coordinator that may still be starting.  The
// awaited condition is a completed hello handshake, so a slow start costs one
// failed connection rather than a fixed wait.
void connect_when_ready(ClientSession& session, ChildGuard& coordinator, std::uint16_t port,
                        const std::string& label) {
  Status last(ErrorCode::kTransportNotStarted, "the coordinator was never contacted");
  for (std::uint32_t attempt = 0; attempt < kConnectAttempts; ++attempt) {
    CTF_CHECK_MSG(coordinator.running(), "the coordinator process exited before it accepted a connection");
    last = session.connect("127.0.0.1", port, label, transport::monotonic_now_ms());
    if (last.is_ok()) return;
    session.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  }
  CTF_CHECK_MSG(false, "the coordinator never completed a handshake: " + last.to_string() +
                         " | coordinator log: " + coordinator.log());
}

// Opens a raw framed session and completes its handshake by hand.  The raw path
// exists because the stale replay case must own the exact bytes a session sends.
void open_raw_session(ChildGuard& coordinator, const transport::Endpoint& endpoint, const std::string& label,
                      transport::Connection& out, protocol::HelloAckPayload& out_hello) {
  Status last(ErrorCode::kTransportNotStarted, "the coordinator was never contacted");
  for (std::uint32_t attempt = 0; attempt < kConnectAttempts; ++attempt) {
    CTF_CHECK_MSG(coordinator.running(), "the coordinator process exited before it accepted a connection");
    transport::Connection connection;
    last = transport::Connection::connect_to(endpoint, connection);
    if (last.is_ok()) {
      protocol::HelloPayload hello;
      hello.client_label = label;
      hello.client_version = "ctf-test-client/1.0.0";
      hello.client_boot_monotonic_ms = transport::monotonic_now_ms();
      std::vector<std::uint8_t> body;
      last = protocol::encode_hello(hello, body);
      if (last.is_ok()) {
        protocol::Frame frame;
        frame.kind = static_cast<std::uint16_t>(protocol::MessageKind::kHello);
        frame.flags = protocol::kFlagRequest;
        frame.payload = std::move(body);
        last = connection.send_frame(frame);
      }
      if (last.is_ok()) {
        protocol::Frame response;
        last = connection.receive_frame(response);
        if (last.is_ok() && static_cast<protocol::MessageKind>(response.kind) == protocol::MessageKind::kHelloAck) {
          last = protocol::decode_hello_ack(response.payload, out_hello);
        } else if (last.is_ok()) {
          last = Status(ErrorCode::kEnvelopeSessionUnknown, "the coordinator did not acknowledge the hello");
        }
      }
      if (last.is_ok()) {
        out = std::move(connection);
        return;
      }
      connection.shutdown_socket();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  }
  CTF_CHECK_MSG(false, "the coordinator never completed a raw handshake: " + last.to_string() +
                         " | coordinator log: " + coordinator.log());
}

// Polls a child that must finish on its own, with a bounded budget.  The
// condition is the process handle, so the wait ends the moment the child is
// gone; a child that never finishes fails the case with its identity instead of
// hanging the suite, and its guard still reaps it.
void wait_until_exited(ChildGuard& child, const std::string& description) {
  for (std::uint32_t attempt = 0; attempt < kExitAttempts; ++attempt) {
    if (!child.running()) {
      CTF_CHECK_EQ(child.wait_for_exit(), 0);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  }
  CTF_CHECK_MSG(false, description + " (process id " + std::to_string(child.process_id()) +
                         ") never exited; the suite reaps it rather than waiting forever.  Child log: " +
                         child.log());
}

// Asks the coordinator to stop over the protocol and waits for the child that
// serves it.  Every session this process owns is closed first: a peer that keeps
// an idle session open holds a coordinator worker in its receive loop, which is
// exactly the state that makes a shutdown slow, and a well behaved client does
// not leave one behind.
void stop_coordinator(ClientSession& session, ChildGuard& coordinator) {
  CTF_CHECK_OK(session.shutdown_coordinator());
  session.close();
  wait_until_exited(coordinator, "the coordinator child");
}

// ---------------------------------------------------------------------------
// Inspection helpers.  Inspection is the only protocol surface that reports the
// coordinator's own view of its authority, so every wait below is a wait on that
// view rather than on a guess about timing.
// ---------------------------------------------------------------------------
template <typename Predicate>
void poll_inspection(ClientSession& session, ChildGuard& coordinator, std::uint8_t subject,
                     const std::string& description, Predicate predicate) {
  std::string body;
  Status last(ErrorCode::kInternalInvariant, "inspection was never attempted");
  for (std::uint32_t attempt = 0; attempt < kObservationAttempts; ++attempt) {
    last = session.inspect(subject, CollectiveId{}, 0, 64, body);
    if (last.is_ok() && predicate(body)) return;
    // A dead coordinator can never satisfy the predicate; reporting it here
    // turns a full poll budget into an immediate, specific failure.
    CTF_CHECK_MSG(coordinator.running(),
                  description + " | the coordinator child (process id " +
                      std::to_string(coordinator.process_id()) + ") is gone | last inspection: " +
                      (last.is_ok() ? body : last.to_string()) + " | coordinator log: " + coordinator.log());
    std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
  }
  CTF_CHECK_MSG(false, description + " | last inspection: " + (last.is_ok() ? body : last.to_string()) +
                         " | coordinator log: " + coordinator.log());
}

// The rendered peers line for one participant, empty when it is absent.  The
// line prefix is matched exactly so an identity is never found inside another
// peer's incarnation field.
std::string peer_line(const std::string& body, ParticipantId participant) {
  const std::string needle = "\n  " + participant.to_string() + " incarnation=";
  const std::size_t at = body.find(needle);
  if (at == std::string::npos) return std::string();
  const std::size_t begin = at + 1;
  const std::size_t end = body.find('\n', begin);
  return body.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

bool peer_is_live(const std::string& body, ParticipantId participant) {
  return peer_line(body, participant).find("state=live") != std::string::npos;
}

bool peer_is_fenced(const std::string& body, ParticipantId participant) {
  return peer_line(body, participant).find("state=fenced") != std::string::npos;
}

bool peer_reports_incarnation(const std::string& body, ParticipantId participant, BootIncarnation incarnation) {
  return peer_line(body, participant).find("incarnation=" + incarnation.to_string()) != std::string::npos;
}

// Reads "key: value" from one line of an inspection body, ignoring indentation.
// The key must begin a line so that "generation" never matches the tail of
// "policy_generation".
bool inspection_field(const std::string& body, const std::string& key, std::string& out) {
  const std::string needle = key + ": ";
  std::size_t at = body.find(needle);
  while (at != std::string::npos) {
    // Walk back over the indentation: only white space may separate the key from
    // the start of its line.
    bool starts_line = true;
    for (std::size_t index = at; index > 0; --index) {
      const char previous = body[index - 1];
      if (previous == '\n') break;
      if (previous != ' ') {
        starts_line = false;
        break;
      }
    }
    if (starts_line) {
      const std::size_t begin = at + needle.size();
      const std::size_t end = body.find('\n', begin);
      out = body.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
      return true;
    }
    at = body.find(needle, at + 1);
  }
  return false;
}

bool inspection_identity(const std::string& body, const std::string& key, Identity& out) {
  std::string text;
  if (!inspection_field(body, key, text)) return false;
  return Identity::parse(text, out);
}

// The authority generations a planning peer must observe.  Reading them from the
// coordinator, instead of remembering them, is what makes a stale request a
// detectable event rather than an assumption.
struct ObservedAuthority {
  CoordinatorEpoch epoch{};
  PolicyGeneration policy{};
  TopologyGeneration topology{};
  EvidenceGeneration capacity{};
  EvidenceGeneration congestion{};
};

ObservedAuthority observe_authority(ClientSession& session) {
  std::string body;
  CTF_CHECK_OK(session.inspect(0, CollectiveId{}, 0, 64, body));
  ObservedAuthority observed;
  CTF_CHECK_MSG(inspection_identity(body, "epoch", observed.epoch), "the summary has no epoch: " + body);
  CTF_CHECK_MSG(inspection_identity(body, "policy_generation", observed.policy),
                "the summary has no policy generation: " + body);
  CTF_CHECK_MSG(inspection_identity(body, "topology_generation", observed.topology),
                "the summary has no topology generation: " + body);
  CTF_CHECK_MSG(inspection_identity(body, "capacity_generation", observed.capacity),
                "the summary has no capacity generation: " + body);
  CTF_CHECK_MSG(inspection_identity(body, "congestion_generation", observed.congestion),
                "the summary has no congestion generation: " + body);
  return observed;
}

std::string summary_text(ClientSession& session) {
  std::string body;
  CTF_CHECK_OK(session.inspect(0, CollectiveId{}, 0, 64, body));
  return body;
}

// The part of the summary that describes authority.  Counters such as sessions,
// decisions and fences legitimately move while a case runs, so they are excluded
// from a before/after comparison; nothing in this view may move when a frame is
// refused.
std::string authority_view(const std::string& summary) {
  static const char* const keys[] = {
      "coordinator: ",           "running: ",             "epoch: ",
      "policy_generation: ",     "topology_generation: ", "capacity_generation: ",
      "congestion_generation: ", "collectives: ",         "peers: ",
      "decision_history: ",      "persistence: "};
  std::string view;
  std::size_t begin = 0;
  while (begin < summary.size()) {
    std::size_t end = summary.find('\n', begin);
    if (end == std::string::npos) end = summary.size();
    const std::string line = summary.substr(begin, end - begin);
    for (const char* key : keys) {
      if (line.rfind(key, 0) == 0) {
        view += line;
        view.push_back('\n');
        break;
      }
    }
    begin = end + 1;
  }
  return view;
}

// ---------------------------------------------------------------------------
// SYNTHETIC fabric evidence and the peers that publish liveness for it
// ---------------------------------------------------------------------------
struct SyntheticEvidence {
  ctf::test::RackFabric fabric;
  TopologyEvidence topology;
  CapacityEvidence capacity;
  CongestionEvidence congestion;
};

// Builds a two rack fabric and matched observations.  Every ingest mints a fresh
// generation so an observation can never be mistaken for an earlier one, and the
// capture timestamps come from the same monotonic clock the coordinator reads so
// no observation is born already stale.
SyntheticEvidence make_evidence(std::uint64_t seed) {
  SyntheticEvidence evidence;
  evidence.fabric = ctf::test::make_fabric(seed, 2, 2);
  const std::uint64_t now = transport::monotonic_now_ms();
  evidence.topology = evidence.fabric.topology;
  evidence.topology.generation = mint_identity();
  evidence.topology.captured_monotonic_ms = now;
  evidence.capacity = ctf::test::make_capacity(evidence.topology, seed + 1, 1000);
  evidence.capacity.generation = mint_identity();
  evidence.capacity.topology_generation = evidence.topology.generation;
  evidence.capacity.captured_monotonic_ms = now;
  evidence.congestion = ctf::test::make_congestion(evidence.topology, seed + 2, 1000);
  evidence.congestion.generation = mint_identity();
  evidence.congestion.topology_generation = evidence.topology.generation;
  evidence.congestion.captured_monotonic_ms = now;
  evidence.congestion.valid_for_ms = 2000;
  return evidence;
}

// Installs topology, then capacity, then congestion.  The order is required: a
// new topology drops the observations that described the previous one.
void ingest_evidence(ClientSession& session, const SyntheticEvidence& evidence) {
  CTF_CHECK_OK(session.ingest_topology(evidence.topology));
  CTF_CHECK_OK(session.ingest_capacity(evidence.capacity));
  CTF_CHECK_OK(session.ingest_congestion(evidence.congestion));
}

// A congestion observation captured now.  Congestion is the only evidence whose
// age bound is short enough to expire while a case is still running, so a plan
// that must be admitted is preceded by a fresh observation rather than by an
// assumption that the earlier one is still current.
CongestionEvidence current_congestion(const SyntheticEvidence& evidence, std::uint64_t seed) {
  CongestionEvidence congestion = ctf::test::make_congestion(evidence.topology, seed, 1000);
  congestion.generation = mint_identity();
  congestion.topology_generation = evidence.topology.generation;
  congestion.captured_monotonic_ms = transport::monotonic_now_ms();
  congestion.valid_for_ms = 2000;
  return congestion;
}

// A participant identity minted in the parent and handed to a publisher, so this
// process knows exactly what a child publishes without reading its console.
struct ParticipantSpec {
  ParticipantId id{};
  BootIncarnation incarnation{};
  NodeId node{};
};

ParticipantSpec mint_participant(const SyntheticEvidence& evidence, std::size_t node_index) {
  ParticipantSpec spec;
  spec.id = mint_identity();
  spec.incarnation = mint_identity();
  spec.node = evidence.fabric.nodes[node_index % evidence.fabric.nodes.size()];
  return spec;
}

// Starts a participant helper process.  It publishes exactly the given identity
// and incarnation until it is killed or until the coordinator refuses it.
void start_participant(ChildGuard& participant, std::uint16_t port, const ParticipantSpec& spec,
                       const std::string& label, bool backdate_incarnation = false, bool redirect_logs = true) {
  const std::string executable = participant_executable();
  CTF_CHECK_MSG(!executable.empty(), "the participant helper path was not injected by the build");
  std::vector<std::string> arguments{executable,          "--port",        std::to_string(port),
                                     "--label",           label,           "--participant",
                                     spec.id.to_string(), "--incarnation", spec.incarnation.to_string(),
                                     "--node",            spec.node.to_string(),
                                     "--interval-ms",     "50"};
  if (backdate_incarnation) arguments.push_back("--backdate-incarnation");
  participant.start(arguments, label, redirect_logs);
}

// Publishes liveness from the parent's own session.  The coordinator binds the
// record to that session, so the record is live exactly as long as the parent
// keeps publishing it.
void publish_participant(ClientSession& session, const ParticipantSpec& spec) {
  CTF_CHECK_OK(session.heartbeat(spec.id, spec.incarnation, spec.node, ParticipantState::kLive));
}

// Installs the traffic policy over the wire.  Policy is authority rather than
// observation, so it travels as its own message and the acknowledgement carries
// the generation the coordinator installed.
void install_policy(ClientSession& session, const TrafficPolicy& policy) {
  protocol::InstallPolicyPayload payload;
  payload.policy = policy;
  std::vector<std::uint8_t> body;
  CTF_CHECK_OK(protocol::encode_install_policy(payload, body));
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response;
  CTF_CHECK_OK(session.request(protocol::MessageKind::kInstallPolicy, body, response_kind, response));
  CTF_CHECK_EQ(response_kind, protocol::MessageKind::kInstallPolicyAck);
  protocol::InstallPolicyAckPayload ack;
  CTF_CHECK_OK(protocol::decode_install_policy_ack(response, ack));
  CTF_CHECK_MSG(ack.result == 0, std::string("the coordinator refused the policy: ") + ack.reasons.to_string());
  CTF_CHECK_EQ(ack.generation, policy.generation);
}

// One planning request: the canonical ring plan for this instance, bound to the
// generations the coordinator reported as current.
DecisionRequest ring_request(const CollectiveInstance& instance, const ObservedAuthority& observed,
                             const std::vector<ParticipantId>& participants, std::uint64_t bytes_per_edge) {
  DecisionRequest request;
  request.instance = instance;
  request.observed_epoch = observed.epoch;
  request.observed_policy_generation = observed.policy;
  request.observed_topology_generation = observed.topology;
  request.observed_capacity_generation = observed.capacity;
  request.observed_congestion_generation = observed.congestion;
  request.has_flow_group_plan = true;
  request.plan = ctf::test::make_ring_plan(instance, participants, bytes_per_edge);
  return request;
}

CollectiveInstance instance_of(CollectiveId id, CollectiveGeneration generation, CollectiveAttemptId attempt) {
  CollectiveInstance instance;
  instance.id = id;
  instance.generation = generation;
  instance.attempt = attempt;
  return instance;
}

// Plans and fails with the decision's own summary when it is not admitted.
DecisionRecord plan_admitted(ClientSession& session, const DecisionRequest& request) {
  DecisionRecord decision;
  CTF_CHECK_OK(session.plan_flow_group(request, decision));
  CTF_CHECK_MSG(decision.outcome == Outcome::kAdmitted, decision.summary());
  return decision;
}

bool attempt_accepted(const protocol::AttemptAckPayload& ack) {
  return ack.state == CollectiveState::kPlanning || ack.state == CollectiveState::kActive;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Two or more publisher processes register and update collective traffic.
// ---------------------------------------------------------------------------
CTF_TEST("multiprocess_fabric", "two_publisher_processes_register_and_update_collective_traffic") {
  const std::uint16_t port = reserve_port();
  ChildGuard coordinator;
  start_coordinator(coordinator, port, std::string());
  ClientSession control;
  connect_when_ready(control, coordinator, port, "parent-control");

  // The policy is authority: the peer that owns the deployment installs it
  // before any traffic decision can exist.
  install_policy(control, TrafficPolicy::standard(mint_identity()));

  // SYNTHETIC two rack fabric; every capacity and congestion claim below is a
  // claim about this evidence and about nothing physical.
  const SyntheticEvidence evidence = make_evidence(0x5150ull);
  const ParticipantSpec first = mint_participant(evidence, 0);
  const ParticipantSpec second = mint_participant(evidence, 1);

  ChildGuard publisher_one;
  ChildGuard publisher_two;
  start_participant(publisher_one, port, first, "publisher-one");
  start_participant(publisher_two, port, second, "publisher-two");

  ingest_evidence(control, evidence);

  // The publisher processes own their liveness.  The parent proves the
  // heartbeats landed by reading the coordinator's peer table, because a child's
  // console is not available to this process.
  poll_inspection(control, coordinator, 3, "both publisher processes never became live",
                  [&](const std::string& body) {
                    return peer_is_live(body, first.id) && peer_is_live(body, second.id);
                  });

  CollectiveDefinition definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, {first.id, second.id}, "multiprocess-allreduce", 1ull << 30);
  protocol::RegisterAckPayload registration;
  CTF_CHECK_OK(control.register_collective(definition, registration));
  CTF_CHECK_EQ(registration.id, definition.id);
  CTF_CHECK_EQ(registration.result, static_cast<std::uint8_t>(RegistrationResult::kRegistered));
  CTF_CHECK(registration.generation.is_some());

  const CollectiveAttemptId attempt = mint_identity();
  protocol::AttemptAckPayload attempt_ack;
  CTF_CHECK_OK(control.begin_attempt(definition.id, registration.generation, attempt, 1ull << 20, attempt_ack));
  CTF_CHECK_MSG(attempt_accepted(attempt_ack),
                std::string("unexpected attempt state: ") + std::string(to_string(attempt_ack.state)));

  const CollectiveInstance instance = instance_of(definition.id, registration.generation, attempt);
  const DecisionRequest admitted_request =
      ring_request(instance, observe_authority(control), definition.participants, 1ull << 18);
  const DecisionRecord admitted = plan_admitted(control, admitted_request);
  CTF_CHECK_EQ(admitted.collective_generation, registration.generation);
  CTF_CHECK_EQ(admitted.epoch, admitted_request.observed_epoch);
  CTF_CHECK(admitted.flow_group_constructed);
  CTF_CHECK(admitted.all_members_live);

  // A second session re-registers the same collective with a changed member set.
  // The durable definition is generation stamped, so the update advances the
  // generation and every decision bound to the previous one is stale.
  ClientSession updater;
  connect_when_ready(updater, coordinator, port, "parent-updater");

  const ParticipantSpec third = mint_participant(evidence, 2);
  publish_participant(control, third);

  CollectiveDefinition updated = definition;
  updated.generation = CollectiveGeneration{};
  updated.participants.push_back(third.id);
  canonicalize_participants(updated.participants);
  CTF_CHECK_EQ(updated.participants.size(), static_cast<std::size_t>(3));

  protocol::RegisterAckPayload update;
  CTF_CHECK_OK(updater.register_collective(updated, update));
  CTF_CHECK_EQ(update.id, definition.id);
  CTF_CHECK_EQ(update.result, static_cast<std::uint8_t>(RegistrationResult::kGenerationAdvanced));
  CTF_CHECK_NE(update.generation, registration.generation);

  // Replaying the admitted request must now be refused on the collective
  // generation axis: the decision it produced described the old membership.
  DecisionRecord stale;
  CTF_CHECK_OK(updater.plan_flow_group(admitted_request, stale));
  CTF_CHECK_EQ(stale.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(stale.axis, AuthorityAxis::kCollectiveGeneration);
  CTF_CHECK_EQ(stale.code, ErrorCode::kCompletionStaleGeneration);

  // Traffic treatment for the new membership needs a new attempt and a new plan;
  // a registration never carries the previous authority forward.
  const CollectiveAttemptId updated_attempt = mint_identity();
  protocol::AttemptAckPayload updated_attempt_ack;
  CTF_CHECK_OK(
      control.begin_attempt(definition.id, update.generation, updated_attempt, 1ull << 20, updated_attempt_ack));
  CTF_CHECK_MSG(attempt_accepted(updated_attempt_ack), "the updated generation refused a new attempt");
  CTF_CHECK_OK(control.ingest_congestion(current_congestion(evidence, 0x5151ull)));

  const CollectiveInstance updated_instance = instance_of(definition.id, update.generation, updated_attempt);
  const DecisionRequest updated_request =
      ring_request(updated_instance, observe_authority(control), updated.participants, 1ull << 18);
  const DecisionRecord readmitted = plan_admitted(control, updated_request);
  CTF_CHECK_EQ(readmitted.collective_generation, update.generation);
  // A flow group is bound to its instance, so a new membership can never reuse
  // the authority of the old one.
  CTF_CHECK_NE(readmitted.flow_group, admitted.flow_group);

  // Both parent sessions are closed before the shutdown request, so no idle
  // session of ours can hold a coordinator worker open during its teardown.
  updater.close();
  stop_coordinator(control, coordinator);
}

// ---------------------------------------------------------------------------
// 2. Killing one participant fences its boot incarnation.
// ---------------------------------------------------------------------------
CTF_TEST("multiprocess_fabric", "killing_a_participant_fences_its_boot_incarnation") {
  const std::uint16_t port = reserve_port();
  ChildGuard coordinator;
  start_coordinator(coordinator, port, std::string());
  ClientSession control;
  connect_when_ready(control, coordinator, port, "parent-control");
  install_policy(control, TrafficPolicy::standard(mint_identity()));

  const SyntheticEvidence evidence = make_evidence(0x2A2Aull);
  ingest_evidence(control, evidence);

  // Boot incarnations are ordered evidence.  The victim starts on a deliberately
  // low incarnation so that the replacement boot has a strictly greater one,
  // which is the ordering contract participant.hpp documents.
  ParticipantSpec survivor = mint_participant(evidence, 0);
  ParticipantSpec victim = mint_participant(evidence, 1);
  victim.incarnation = BootIncarnation(0x1111111111111111ull, 0x1111111111111111ull);

  ChildGuard survivor_process;
  ChildGuard victim_process;
  start_participant(survivor_process, port, survivor, "publisher-survivor");
  // The victim is started without a log redirecting shell so that the abrupt kill
  // below terminates the publisher itself: a shell in front of it would be killed
  // instead and the publisher's session would never close.
  start_participant(victim_process, port, victim, "publisher-victim", false, false);
  poll_inspection(control, coordinator, 3, "the two publisher processes never became live",
                  [&](const std::string& body) {
                    return peer_is_live(body, survivor.id) && peer_is_live(body, victim.id);
                  });

  CollectiveDefinition definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, {survivor.id, victim.id}, "fenced-boot", 1ull << 30);
  protocol::RegisterAckPayload registration;
  CTF_CHECK_OK(control.register_collective(definition, registration));
  const CollectiveAttemptId attempt = mint_identity();
  protocol::AttemptAckPayload attempt_ack;
  CTF_CHECK_OK(control.begin_attempt(definition.id, registration.generation, attempt, 1ull << 20, attempt_ack));
  const CollectiveInstance instance = instance_of(definition.id, registration.generation, attempt);
  const DecisionRequest request =
      ring_request(instance, observe_authority(control), definition.participants, 1ull << 18);
  const DecisionRecord baseline = plan_admitted(control, request);
  CTF_CHECK_EQ(baseline.collective_generation, registration.generation);

  // Abrupt termination.  The publisher never runs a shutdown path of its own, so
  // the coordinator learns about the death the way it would learn about a crash.
  victim_process.kill();
  CTF_CHECK_MSG(victim_process.wait_for_exit() != 0, "an abruptly killed process reported success");

  // The death becomes observable through the coordinator's peer table.  The
  // parent waits on that condition because the socket teardown races this
  // process; it never waits on a fixed delay.
  poll_inspection(control, coordinator, 3, "the killed publisher was never fenced from the live set",
                  [&](const std::string& body) { return peer_is_fenced(body, victim.id); });

  // (a) Re-publishing the dead boot's own incarnation is refused: the
  // coordinator has already seen that incarnation and its owner is gone, so no
  // session may resurrect it, not even the one that is still connected.
  const Status resurrection = control.heartbeat(victim.id, victim.incarnation, victim.node, ParticipantState::kLive);
  CTF_CHECK_CODE(resurrection, ErrorCode::kEnvelopeIncarnationStale);

  // (b) The plan is refused on the liveness axis: the definition names a
  // participant set that is no longer fully live, and a definition is not
  // liveness evidence.
  DecisionRecord blocked;
  CTF_CHECK_OK(control.plan_flow_group(request, blocked));
  CTF_CHECK_EQ(blocked.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(blocked.axis, AuthorityAxis::kParticipantLiveness);
  CTF_CHECK_EQ(blocked.code, ErrorCode::kEnvelopeIncarnationStale);
  CTF_CHECK(!blocked.all_members_live);

  // A fresh boot of the same participant publishes a strictly greater
  // incarnation, and only that publication restores liveness and admission.
  ParticipantSpec replacement = victim;
  replacement.incarnation = BootIncarnation(0xFFFFFFFFFFFFFFFFull, 0x0000000000000001ull);
  CTF_CHECK_MSG(victim.incarnation < replacement.incarnation,
                "the replacement incarnation must be strictly greater than the fenced one");
  ChildGuard replacement_process;
  start_participant(replacement_process, port, replacement, "publisher-victim-rebooted");
  poll_inspection(control, coordinator, 3, "the replacement boot never became live",
                  [&](const std::string& body) {
                    return peer_is_live(body, victim.id) &&
                           peer_reports_incarnation(body, victim.id, replacement.incarnation);
                  });

  CTF_CHECK_OK(control.ingest_congestion(current_congestion(evidence, 0x2A2Bull)));
  const DecisionRequest restored_request =
      ring_request(instance, observe_authority(control), definition.participants, 1ull << 18);
  const DecisionRecord restored = plan_admitted(control, restored_request);
  CTF_CHECK(restored.all_members_live);
  CTF_CHECK_EQ(restored.epoch, restored_request.observed_epoch);

  // The fenced boot stays refused after a newer one exists, and a helper process
  // that starts backdated cannot publish itself at all.
  const Status old_boot = control.heartbeat(victim.id, victim.incarnation, victim.node, ParticipantState::kLive);
  CTF_CHECK_CODE(old_boot, ErrorCode::kEnvelopeIncarnationStale);

  ChildGuard backdated_process;
  start_participant(backdated_process, port, victim, "publisher-victim-backdated", true, false);
  // The helper reports a refused heartbeat by exiting with its documented code.
  CTF_CHECK_EQ(backdated_process.wait_for_exit(), 4);

  stop_coordinator(control, coordinator);
}

// ---------------------------------------------------------------------------
// 3. Coordinator restart advances the epoch, preserves durable definitions and
//    requires live evidence to be revalidated.
// ---------------------------------------------------------------------------
CTF_TEST("multiprocess_fabric", "coordinator_restart_advances_the_epoch_and_requires_revalidation") {
  TemporarySnapshot snapshot("restart");
  const std::uint16_t port = reserve_port();

  // Both boots describe the same SYNTHETIC fabric, which is what restarting a
  // coordinator in an unchanged fabric looks like: node identities are stable
  // while every observation gets a fresh generation.
  const SyntheticEvidence layout = make_evidence(0x3C3Cull);
  const ParticipantSpec first = mint_participant(layout, 0);
  const ParticipantSpec second = mint_participant(layout, 1);
  const CollectiveDefinition definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, {first.id, second.id}, "restart-durable", 1ull << 30);

  CoordinatorEpoch first_epoch;
  PolicyGeneration durable_policy;
  CollectiveGeneration durable_generation;
  CollectiveAttemptId attempt;
  {
    ChildGuard coordinator;
    start_coordinator(coordinator, port, snapshot.path());
    ClientSession control;
    connect_when_ready(control, coordinator, port, "parent-control-before-restart");
    first_epoch = control.hello().epoch;

    const TrafficPolicy policy = TrafficPolicy::standard(mint_identity());
    install_policy(control, policy);
    durable_policy = policy.generation;

    ingest_evidence(control, make_evidence(0x3C3Cull));
    publish_participant(control, first);
    publish_participant(control, second);

    protocol::RegisterAckPayload registration;
    CTF_CHECK_OK(control.register_collective(definition, registration));
    durable_generation = registration.generation;
    attempt = mint_identity();
    protocol::AttemptAckPayload attempt_ack;
    CTF_CHECK_OK(control.begin_attempt(definition.id, durable_generation, attempt, 1ull << 20, attempt_ack));

    const CollectiveInstance instance = instance_of(definition.id, durable_generation, attempt);
    const DecisionRequest request =
        ring_request(instance, observe_authority(control), definition.participants, 1ull << 18);
    const DecisionRecord admitted = plan_admitted(control, request);
    CTF_CHECK_EQ(admitted.epoch, first_epoch);

    // A shutdown over the protocol is the durability point: the child flushes
    // the snapshot before it exits and the parent waits on its process handle.
    stop_coordinator(control, coordinator);
  }
  CTF_CHECK_MSG(snapshot.exists(), "the coordinator did not write its snapshot on shutdown");

  {
    ChildGuard coordinator;
    start_coordinator(coordinator, port, snapshot.path());
    ClientSession control;
    connect_when_ready(control, coordinator, port, "parent-control-after-restart");
    const CoordinatorEpoch second_epoch = control.hello().epoch;
    CTF_CHECK_NE(second_epoch, first_epoch);
    // Durable authority crossed the restart; dynamic evidence did not.
    CTF_CHECK_EQ(control.hello().policy_generation, durable_policy);

    std::string collective;
    CTF_CHECK_OK(control.inspect(1, definition.id, 0, 64, collective));
    CTF_CHECK_MSG(collective.find(definition.id.to_string()) != std::string::npos,
                  "the durable collective is not the one that was registered: " + collective);
    Identity restored_generation;
    CTF_CHECK_MSG(inspection_identity(collective, "generation", restored_generation),
                  "the durable collective has no generation: " + collective);
    CTF_CHECK_EQ(restored_generation, durable_generation);

    // Capacity and congestion are observations, not durable state, so the plan
    // must ask for a replan instead of inheriting pre-restart authority.  The
    // participants are re-published first so the request reaches the evidence
    // gate rather than stopping at liveness.
    publish_participant(control, first);
    publish_participant(control, second);
    const CollectiveInstance instance = instance_of(definition.id, durable_generation, attempt);
    DecisionRecord replan;
    CTF_CHECK_OK(control.plan_flow_group(
        ring_request(instance, observe_authority(control), definition.participants, 1ull << 18), replan));
    CTF_CHECK_EQ(replan.outcome, Outcome::kRequiresReplan);
    CTF_CHECK_EQ(replan.axis, AuthorityAxis::kCongestionEvidence);
    CTF_CHECK_EQ(replan.code, ErrorCode::kValidationRateInvalid);

    // Fresh observations, then the same request is admitted again, bound to the
    // new epoch: the epoch is what makes the pre-restart decision stale.
    ingest_evidence(control, make_evidence(0x3C3Cull));
    publish_participant(control, first);
    publish_participant(control, second);
    const DecisionRequest request =
        ring_request(instance, observe_authority(control), definition.participants, 1ull << 18);
    const DecisionRecord readmitted = plan_admitted(control, request);
    CTF_CHECK_EQ(readmitted.epoch, second_epoch);
    CTF_CHECK_EQ(readmitted.epoch, request.observed_epoch);
    CTF_CHECK_EQ(readmitted.topology_generation, request.observed_topology_generation);
    CTF_CHECK_EQ(readmitted.capacity_generation, request.observed_capacity_generation);
    CTF_CHECK_EQ(readmitted.congestion_generation, request.observed_congestion_generation);
    CTF_CHECK_NE(readmitted.epoch, first_epoch);

    stop_coordinator(control, coordinator);
  }

  // The case owns the snapshot: it is deleted here, and the guard deletes it
  // again if an assertion unwound the case earlier.
  snapshot.remove();
  CTF_CHECK_MSG(!snapshot.exists(), "the snapshot survived the case cleanup");
}

// ---------------------------------------------------------------------------
// 4. Repeated process lifecycle: three boots, three epochs, one durable
//    definition, and no temporary or leftover files.
// ---------------------------------------------------------------------------
CTF_TEST("multiprocess_fabric", "repeated_coordinator_lifecycle_leaves_no_durable_or_temporary_leftovers") {
  TemporarySnapshot snapshot("lifecycle");
  const CollectiveDefinition definition =
      ctf::test::make_definition(mint_identity(), CollectiveClass::kAllReduce,
                                 {mint_identity(), mint_identity()}, "lifecycle-durable", 1ull << 30);
  CollectiveGeneration durable_generation;
  std::vector<CoordinatorEpoch> epochs;
  for (int cycle = 0; cycle < 3; ++cycle) {
    const std::uint16_t port = reserve_port();
    ChildGuard coordinator;
    start_coordinator(coordinator, port, snapshot.path());
    ClientSession control;
    connect_when_ready(control, coordinator, port, "parent-control-cycle-" + std::to_string(cycle));
    epochs.push_back(control.hello().epoch);

    if (cycle == 0) {
      protocol::RegisterAckPayload registration;
      CTF_CHECK_OK(control.register_collective(definition, registration));
      CTF_CHECK_EQ(registration.result, static_cast<std::uint8_t>(RegistrationResult::kRegistered));
      durable_generation = registration.generation;
    } else {
      std::string collective;
      CTF_CHECK_OK(control.inspect(1, definition.id, 0, 64, collective));
      CTF_CHECK_MSG(collective.find(definition.id.to_string()) != std::string::npos,
                    "the durable collective identity changed across a restart: " + collective);
      Identity restored;
      CTF_CHECK_MSG(inspection_identity(collective, "generation", restored),
                    "the durable collective did not survive a restart: " + collective);
      CTF_CHECK_EQ(restored, durable_generation);
    }

    stop_coordinator(control, coordinator);
    // After every cycle the durable file exists and the atomic replacement
    // sibling does not: a leftover sibling would mean an interrupted write.
    CTF_CHECK_MSG(snapshot.exists(), "cycle " + std::to_string(cycle) + " left no snapshot behind");
    CTF_CHECK_MSG(!snapshot.replacement_sibling_exists(),
                  "cycle " + std::to_string(cycle) + " left an atomic replacement sibling behind");
  }

  CTF_CHECK_EQ(epochs.size(), static_cast<std::size_t>(3));
  for (std::size_t left = 0; left < epochs.size(); ++left) {
    for (std::size_t right = left + 1; right < epochs.size(); ++right) {
      CTF_CHECK_MSG(epochs[left] != epochs[right], "two coordinator boots reported the same epoch");
    }
  }

  snapshot.remove();
  const std::vector<std::string> leftovers = snapshot.leftovers();
  CTF_CHECK_MSG(leftovers.empty(),
                "the case left files behind: " + (leftovers.empty() ? std::string() : leftovers.front()));
  CTF_CHECK(!snapshot.exists());
}

// ---------------------------------------------------------------------------
// 5. Stale replay refusal across processes.
// ---------------------------------------------------------------------------
CTF_TEST("multiprocess_fabric", "stale_replay_across_sessions_is_refused_without_changing_state") {
  const std::uint16_t port = reserve_port();
  ChildGuard coordinator;
  start_coordinator(coordinator, port, std::string());

  // The observer is a typed session that outlives the replayed frames and can
  // therefore compare the coordinator's state before and after them.
  ClientSession observer;
  connect_when_ready(observer, coordinator, port, "parent-observer");

  // Session A speaks raw framing so the exact bytes it sends can be captured and
  // replayed.  Its identities come from the handshake, which is the only way a
  // peer can obtain a session identity.
  transport::Endpoint endpoint;
  endpoint.host = "127.0.0.1";
  endpoint.port = port;
  transport::Connection session_a;
  protocol::HelloAckPayload hello_a;
  open_raw_session(coordinator, endpoint, "replay-source", session_a, hello_a);
  CTF_CHECK(hello_a.session.is_some());

  const CollectiveDefinition replayed_definition =
      ctf::test::make_definition(mint_identity(), CollectiveClass::kAllReduce,
                                 {mint_identity(), mint_identity()}, "replayed-definition", 1ull << 30);
  protocol::RegisterCollectivePayload register_payload;
  register_payload.definition = replayed_definition;
  std::vector<std::uint8_t> register_body;
  CTF_CHECK_OK(protocol::encode_register_collective(register_payload, register_body));

  // The captured frame is a valid request from session A: its envelope names the
  // session and boot incarnation the coordinator minted for it, and its sequence
  // is the next one that session may use.
  protocol::Frame captured;
  captured.kind = static_cast<std::uint16_t>(protocol::MessageKind::kRegisterCollective);
  captured.flags = protocol::kFlagRequest;
  captured.envelope.session = hello_a.session;
  captured.envelope.incarnation = hello_a.incarnation;
  captured.envelope.epoch = hello_a.epoch;
  captured.envelope.frame_sequence = Sequence(1);
  captured.envelope.correlation = 7;
  captured.payload = register_body;
  std::vector<std::uint8_t> captured_bytes;
  CTF_CHECK_OK(protocol::encode_frame(captured, captured_bytes));

  // Session A sends it for real, so the captured bytes are known to be valid and
  // to have an observable effect.
  CTF_CHECK_OK(session_a.send_frame(captured));
  protocol::Frame a_response;
  CTF_CHECK_OK(session_a.receive_frame(a_response));
  CTF_CHECK_EQ(static_cast<protocol::MessageKind>(a_response.kind), protocol::MessageKind::kRegisterAck);

  std::string collective;
  CTF_CHECK_OK(observer.inspect(1, replayed_definition.id, 0, 64, collective));
  Identity registered_generation;
  CTF_CHECK_MSG(inspection_identity(collective, "generation", registered_generation),
                "the captured frame was not registered: " + collective);
  const std::string before = summary_text(observer);

  // Session B completes its own handshake.  A frame that names an epoch which is
  // not current is refused as stale, which is the shape a frame captured before
  // a coordinator restart carries.
  ClientSession replay;
  connect_when_ready(replay, coordinator, port, "replay-carrier");
  protocol::Frame stale_epoch = captured;
  stale_epoch.envelope.session = replay.hello().session;
  stale_epoch.envelope.incarnation = replay.hello().incarnation;
  stale_epoch.envelope.epoch = mint_identity();
  stale_epoch.envelope.frame_sequence = Sequence(2);
  std::vector<std::uint8_t> stale_bytes;
  CTF_CHECK_OK(protocol::encode_frame(stale_epoch, stale_bytes));
  CTF_CHECK_OK(replay.connection().send_raw(stale_bytes.data(), stale_bytes.size()));
  protocol::Frame stale_refusal;
  CTF_CHECK_OK(replay.connection().receive_frame(stale_refusal));
  CTF_CHECK_EQ(static_cast<protocol::MessageKind>(stale_refusal.kind), protocol::MessageKind::kErrorResponse);
  protocol::ErrorPayload stale_error;
  CTF_CHECK_OK(protocol::decode_error(stale_refusal.payload, stale_error));
  CTF_CHECK_EQ(stale_error.code, ErrorCode::kEnvelopeEpochStale);
  const std::string after_stale = summary_text(observer);
  CTF_CHECK_MSG(authority_view(before) == authority_view(after_stale),
                "the stale epoch frame changed authority state:\n" + authority_view(before) + "->\n" +
                    authority_view(after_stale));

  // The captured bytes are now replayed through session B while session A is
  // still live.  The frame is not B's to send: the coordinator refuses it and
  // must not act on it.  The refusal closes B, so every later observation uses
  // the observer session.
  CTF_CHECK_OK(replay.connection().send_raw(captured_bytes.data(), captured_bytes.size()));
  protocol::Frame refused;
  CTF_CHECK_OK(replay.connection().receive_frame(refused));
  CTF_CHECK_EQ(static_cast<protocol::MessageKind>(refused.kind), protocol::MessageKind::kErrorResponse);
  protocol::ErrorPayload refusal;
  CTF_CHECK_OK(protocol::decode_error(refused.payload, refusal));
  CTF_CHECK_EQ(refusal.code, ErrorCode::kEnvelopeSessionMismatch);
  // The coordinator closed this connection as part of the refusal; the client
  // closes its end too so no half dead socket outlives the case.
  replay.close();

  const std::string after = summary_text(observer);
  CTF_CHECK_MSG(authority_view(before) == authority_view(after),
                "the replayed frame changed authority state:\n" + authority_view(before) + "->\n" +
                    authority_view(after));
  // The definition the captured frame registered still carries the generation
  // session A created: the replay registered nothing.
  CTF_CHECK_OK(observer.inspect(1, replayed_definition.id, 0, 64, collective));
  Identity after_generation;
  CTF_CHECK_MSG(inspection_identity(collective, "generation", after_generation),
                "the durable definition vanished: " + collective);
  CTF_CHECK_EQ(after_generation, registered_generation);

  // Session A is still the owner of its identity, and the coordinator still
  // serves it: the refusal bound the frame to the connection, it did not damage
  // the session the frame came from.
  CTF_CHECK(session_a.is_open());
  CTF_CHECK(observer.connected());

  // The raw source session is closed by its owner before the shutdown request so
  // that no idle session of this process outlives the case.
  session_a.shutdown_socket();
  stop_coordinator(observer, coordinator);
}

CTF_TEST_MAIN()
