// Collective Traffic Fabric - helper process behaviour shared by the
// participant and coordinator executables.
// Copyright 2026 Summon Software Labs.
#include "support/process_helper.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ctf/coordinator.hpp"
#include "ctf/identity.hpp"
#include "ctf/transport.hpp"
#include "support/client_session.hpp"

namespace ctf::test {
namespace {

bool parse_hex_identity(const std::string& text, Identity& out) {
  return Identity::parse(text, out);
}

std::string option_value(int argc, char** argv, const char* name, const std::string& fallback) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::strcmp(argv[index], name) == 0) return std::string(argv[index + 1]);
  }
  return fallback;
}

bool option_present(int argc, char** argv, const char* name) {
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], name) == 0) return true;
  }
  return false;
}

}  // namespace

Status parse_participant_options(int argc, char** argv, ParticipantOptions& out) {
  const std::string port = option_value(argc, argv, "--port", "");
  if (port.empty()) return Status(ErrorCode::kTransportAddressInvalid, "--port is required");
  out.port = static_cast<std::uint16_t>(std::strtoul(port.c_str(), nullptr, 10));
  if (out.port == 0) return Status(ErrorCode::kTransportAddressInvalid, "--port must be non zero");
  out.label = option_value(argc, argv, "--label", "participant");
  out.participant_hex = option_value(argc, argv, "--participant", "");
  out.incarnation_hex = option_value(argc, argv, "--incarnation", "");
  out.node_hex = option_value(argc, argv, "--node", "");
  out.heartbeats = static_cast<std::uint32_t>(std::strtoul(option_value(argc, argv, "--heartbeats", "0").c_str(), nullptr, 10));
  out.interval_ms = std::strtoull(option_value(argc, argv, "--interval-ms", "200").c_str(), nullptr, 10);
  out.lifetime_ms = std::strtoull(option_value(argc, argv, "--lifetime-ms", "0").c_str(), nullptr, 10);
  out.backdate_incarnation = option_present(argc, argv, "--backdate-incarnation");
  return Status::ok();
}

int run_participant(const ParticipantOptions& options) {
  Identity participant;
  Identity incarnation;
  Identity node;
  if (!options.participant_hex.empty() && !parse_hex_identity(options.participant_hex, participant)) {
    std::fprintf(stderr, "participant: --participant is not a canonical identity\n");
    return 2;
  }
  if (!options.incarnation_hex.empty() && !parse_hex_identity(options.incarnation_hex, incarnation)) {
    std::fprintf(stderr, "participant: --incarnation is not a canonical identity\n");
    return 2;
  }
  if (!options.node_hex.empty() && !parse_hex_identity(options.node_hex, node)) {
    std::fprintf(stderr, "participant: --node is not a canonical identity\n");
    return 2;
  }
  if (participant.is_none()) participant = mint_identity();
  if (incarnation.is_none()) incarnation = mint_identity();
  if (options.backdate_incarnation) {
    // A deliberately older boot identity, used to prove that an earlier
    // incarnation cannot come back and be believed.
    incarnation = Identity(1, 1);
  }

  emit_line(std::string("participant") + " id=" + participant.to_string() + " incarnation=" +
            incarnation.to_string());

  ClientSession session;
  Status status = session.connect("127.0.0.1", options.port, options.label, transport::monotonic_now_ms());
  if (!status.is_ok()) {
    emit_line(std::string("connect_failed code=") + std::string(to_string(status.code())) + " " +
              status.message());
    return 3;
  }
  emit_line(std::string("connected epoch=") + session.hello().epoch.to_string());

  const std::uint64_t started = transport::monotonic_now_ms();
  std::uint32_t sent = 0;
  for (;;) {
    status = session.heartbeat(participant, incarnation, node, ParticipantState::kLive);
    if (!status.is_ok()) {
      emit_line(std::string("heartbeat_failed code=") + std::string(to_string(status.code())) + " count=" +
                std::to_string(sent));
      return 4;
    }
    ++sent;
    emit_line(std::string("heartbeat count=") + std::to_string(sent));
    if (options.heartbeats != 0 && sent >= options.heartbeats) break;
    if (options.lifetime_ms != 0 && transport::monotonic_now_ms() - started >= options.lifetime_ms) break;
    if (options.heartbeats == 0 && options.lifetime_ms == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(options.interval_ms));
  }
  emit_line("participant_done");
  return 0;
}

Status parse_coordinator_options(int argc, char** argv, CoordinatorOptions& out) {
  const std::string port = option_value(argc, argv, "--port", "");
  if (port.empty()) return Status(ErrorCode::kTransportAddressInvalid, "--port is required");
  out.port = static_cast<std::uint16_t>(std::strtoul(port.c_str(), nullptr, 10));
  if (out.port == 0) return Status(ErrorCode::kTransportAddressInvalid, "--port must be non zero");
  out.snapshot_path = option_value(argc, argv, "--snapshot", "");
  out.label = option_value(argc, argv, "--label", "multiprocess-coordinator");
  out.participant_ttl_ms =
      std::strtoull(option_value(argc, argv, "--participant-ttl-ms", "3000").c_str(), nullptr, 10);
  out.persist_on_mutation = option_present(argc, argv, "--persist-on-mutation");
  return Status::ok();
}

int run_coordinator(const CoordinatorOptions& options) {
  CoordinatorConfig config;
  config.transport.port = options.port;
  config.transport.bind_host = "127.0.0.1";
  config.service.coordinator_label = options.label;
  config.service.participant_ttl_ms = options.participant_ttl_ms;
  config.snapshot_path = options.snapshot_path;
  config.persist_on_mutation = options.persist_on_mutation;

  CoordinatorServer server(config);
  const Status status = server.start(transport::monotonic_now_ms());
  if (!status.is_ok()) {
    emit_line(std::string("start_failed code=") + std::string(to_string(status.code())) + " " +
              status.message());
    return 3;
  }
  emit_line(std::string("listening port=") + std::to_string(server.port()));
  emit_line(std::string("epoch ") + server.service().epoch().to_string());

  // A coordinator helper serves until a peer asks it to stop.  It also ends by
  // itself once it has served at least one session and then been idle for a
  // while with no session attached: a helper process that outlives the test
  // that started it is a resource leak, and a test must never have to kill a
  // process it can no longer talk to in order to make progress.
  constexpr std::uint64_t kIdleShutdownMs = 40000;
  const std::uint64_t started = transport::monotonic_now_ms();
  std::uint64_t idle_since = started;
  bool served_any = false;
  while (!server.shutdown_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const std::size_t sessions = server.session_count();
    if (sessions != 0) {
      served_any = true;
      idle_since = transport::monotonic_now_ms();
      continue;
    }
    if (!served_any) {
      // Never served anyone: keep waiting, but not forever.
      if (transport::monotonic_now_ms() - started > kIdleShutdownMs * 4) break;
      continue;
    }
    if (transport::monotonic_now_ms() - idle_since > kIdleShutdownMs) {
      emit_line("idle_shutdown no sessions attached for the idle bound");
      break;
    }
  }
  const Status stopped = server.stop(transport::monotonic_now_ms());
  emit_line(std::string("stopped code=") + std::string(to_string(stopped.code())));
  return stopped.is_ok() ? 0 : 4;
}

}  // namespace ctf::test
