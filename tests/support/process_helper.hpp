// Collective Traffic Fabric - independent OS process management for the
// multiprocess proof suite, plus the shared definitions of what the helper
// processes do.
// Copyright 2026 Summon Software Labs.
//
// These tests start real child processes.  The parent controls them through
// their command lines and their standard output, and kills them abruptly where
// the proof requires an ungraceful death rather than a graceful shutdown.
#ifndef CTF_TESTS_PROCESS_HELPER_HPP
#define CTF_TESTS_PROCESS_HELPER_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ctf/error.hpp"

namespace ctf::test {

// ---------------------------------------------------------------------------
// Child process control
// ---------------------------------------------------------------------------
struct ChildProcess {
  long long handle = 0;   // opaque: platform process handle or pid
  int process_id = 0;
  std::string label;
  bool running = false;
};

// Starts a process.  The child inherits the parent's standard output, which is
// how the parent observes the child's identity lines without a separate pipe.
[[nodiscard]] Status spawn_process(const std::vector<std::string>& arguments, bool inherit_console,
                                   ChildProcess& out);

// Terminates the process abruptly.  This is deliberately not a graceful
// shutdown: the point of these tests is what a crash leaves behind.
[[nodiscard]] Status kill_process(const ChildProcess& process);

// Waits for the process to exit and reports its exit code.
[[nodiscard]] Status wait_process(const ChildProcess& process, int& exit_code);

// True while the process is still alive.
[[nodiscard]] bool process_is_running(const ChildProcess& process);

// Paths to the helper executables, injected by the build.
[[nodiscard]] std::string participant_executable();
[[nodiscard]] std::string coordinator_executable();

// ---------------------------------------------------------------------------
// Participant helper process
// ---------------------------------------------------------------------------
struct ParticipantOptions {
  std::uint16_t port = 0;
  std::string label = "participant";
  std::string participant_hex;    // empty asks the process to mint one
  std::string incarnation_hex;    // empty asks the process to mint one
  std::string node_hex;           // empty means "not stated"
  std::uint32_t heartbeats = 0;   // 0 means "until asked to stop"
  std::uint64_t interval_ms = 200;
  std::uint64_t lifetime_ms = 0;  // 0 means "no self imposed deadline"
  bool backdate_incarnation = false;  // start with a deliberately older boot
};

[[nodiscard]] Status parse_participant_options(int argc, char** argv, ParticipantOptions& out);
[[nodiscard]] int run_participant(const ParticipantOptions& options);

// ---------------------------------------------------------------------------
// Coordinator helper process
// ---------------------------------------------------------------------------
struct CoordinatorOptions {
  std::uint16_t port = 0;
  std::string snapshot_path;
  std::string label = "multiprocess-coordinator";
  std::uint64_t participant_ttl_ms = 3000;
  bool persist_on_mutation = true;
};

[[nodiscard]] Status parse_coordinator_options(int argc, char** argv, CoordinatorOptions& out);
// Runs until a shutdown request arrives.  Prints the listening port and the
// epoch, then serves.
[[nodiscard]] int run_coordinator(const CoordinatorOptions& options);

// Writes one machine readable line to stdout and flushes it, so a parent that
// shares the console sees it in order.
void emit_line(const std::string& line);

}  // namespace ctf::test

#endif  // CTF_TESTS_PROCESS_HELPER_HPP
