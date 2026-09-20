// Collective Traffic Fabric - networked coordinator: a real coordinator process
// that serves real peers over loopback TCP.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_COORDINATOR_HPP
#define CTF_COORDINATOR_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ctf/service.hpp"
#include "ctf/transport.hpp"

namespace ctf {

class CoordinatorServer;

// Translates transport frames into service calls.  Declared here, defined in
// the implementation, so the header does not expose the session table.
struct CoordinatorHandler;

struct CoordinatorConfig {
  transport::ServerConfig transport{};
  ServiceConfig service{};
  std::string snapshot_path;   // empty disables persistence
  bool persist_on_mutation = false;  // write the snapshot on every durable change
  std::uint64_t clock_offset_ms = 0;
};

// Owns a CoordinatorService and serves it over a real socket.  Shutdown order is
// fixed and deliberate: stop accepting, shut down sessions, join worker threads,
// then stop the service so the final durable flush happens last.
class CoordinatorServer {
 public:
  explicit CoordinatorServer(CoordinatorConfig config = {});
  ~CoordinatorServer();
  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;

  Status start(std::uint64_t now_monotonic_ms);
  Status stop(std::uint64_t now_monotonic_ms);
  [[nodiscard]] bool is_running() const noexcept;
  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] std::size_t session_count() const;
  [[nodiscard]] std::uint64_t accepted_total() const;
  [[nodiscard]] std::uint64_t rejected_total() const;
  [[nodiscard]] std::uint64_t stale_frame_count() const;
  [[nodiscard]] std::uint64_t send_failure_count() const;
  // True once a peer has asked this coordinator to stop accepting new work.
  [[nodiscard]] bool shutdown_requested() const noexcept {
    return shutdown_requested_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t malformed_frame_count() const;
  [[nodiscard]] std::uint64_t protocol_violation_count() const;
  [[nodiscard]] std::uint64_t clock_offset_ms() const;
  [[nodiscard]] CoordinatorService& service() noexcept { return service_; }
  [[nodiscard]] const CoordinatorService& service() const noexcept { return service_; }
  [[nodiscard]] CoordinatorConfig config() const;

  // Forces one durable flush.  Exposed so tests can prove that an
  // acknowledgement never precedes its durability point.
  Status flush(std::uint64_t now_monotonic_ms);
  Status restore(std::uint64_t now_monotonic_ms, LoadDisposition& disposition);

  // The single monotonic clock the coordinator uses.  Exposed because the
  // protocol handler and tests must observe exactly the same clock.
  [[nodiscard]] std::uint64_t now_ms() const;

 private:
  friend struct CoordinatorHandler;

  CoordinatorConfig config_;
  CoordinatorService service_;
  std::unique_ptr<CoordinatorHandler> handler_;
  transport::Server server_;
  std::atomic<bool> running_{false};
  std::atomic<bool> shutdown_requested_{false};
  // Refusal accounting.  These are counted, never silently dropped.
  std::atomic<std::uint64_t> stale_frames_{0};
  std::atomic<std::uint64_t> malformed_frames_{0};
  std::atomic<std::uint64_t> protocol_violations_{0};
  std::atomic<std::uint64_t> send_failures_{0};
};

}  // namespace ctf

#endif  // CTF_COORDINATOR_HPP
