// Collective Traffic Fabric - loopback TCP transport.
// Copyright 2026 Summon Software Labs.
//
// This is a real transport: real sockets, real framing, real independent OS
// processes on the other end.  Shutdown is designed so that no thread can be
// left blocked: sockets are always shut down before threads are joined, and no
// lock is held while a join happens or while a blocking call runs.
#ifndef CTF_TRANSPORT_HPP
#define CTF_TRANSPORT_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ctf/error.hpp"
#include "ctf/limits.hpp"
#include "ctf/protocol.hpp"

namespace ctf::transport {

// How long a receive may block before it re-checks the connection state.  This
// is not a timeout on a test or on an operation: it is the granularity at which
// a blocked reader notices that it has been shut down.
inline constexpr std::uint64_t kDefaultPollIntervalMs = 20;

struct Endpoint {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
};

// Owns a socket.  The socket is closed exactly once, by the destructor this
// class defines; no other code path closes an owned descriptor.
class Connection {
 public:
  Connection() = default;
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&& other) noexcept;
  Connection& operator=(Connection&& other) noexcept;

  [[nodiscard]] bool is_open() const noexcept;

  // Client side connect.  The endpoint must already be resolved to a literal
  // address; no name resolution is performed, so no DNS dependency exists and
  // an unreachable host fails immediately and deterministically.
  [[nodiscard]] static Status connect_to(const Endpoint& endpoint, Connection& out);

  // Frames one message, writes it completely (looping over partial writes) and
  // returns only when every byte has been handed to the kernel or an error is
  // reported.
  [[nodiscard]] Status send_frame(const protocol::Frame& frame) const;
  // Reads until exactly one whole frame is available.  Returns
  // kTransportClosed when the peer closed cleanly with no partial frame
  // pending, and kFrameTruncatedPayload / kFrameTruncatedHeader when the peer
  // closed mid frame (never treated as a valid frame).
  [[nodiscard]] Status receive_frame(protocol::Frame& out) const;
  // Blocking readiness wait.  Returns true when at least one byte is readable.
  [[nodiscard]] bool wait_readable(std::uint64_t timeout_ms) const;
  [[nodiscard]] bool wait_writable(std::uint64_t timeout_ms) const;
  // Raw byte access, used by adversarial tests that must send deliberately
  // malformed framing.
  [[nodiscard]] Status send_raw(const std::uint8_t* data, std::size_t size) const;
  [[nodiscard]] Status receive_raw(std::uint8_t* data, std::size_t capacity, std::size_t& received) const;
  // Politely asks the peer to stop and unblocks the local reader where the
  // platform supports it.  Used when a command or a refusal ends a conversation.
  void shutdown_socket() const noexcept;
  // Ends the connection for real: closes the descriptor, which is the only
  // operation guaranteed to release a thread already parked in a read.  Used by
  // server teardown, after which this Connection can never be used again.
  void release_for_teardown() const noexcept;

  [[nodiscard]] const Endpoint& peer() const noexcept { return peer_; }
  [[nodiscard]] const Endpoint& local() const noexcept { return local_; }
  void set_read_buffer_capacity(std::size_t bytes) noexcept { read_capacity_ = bytes; }
  // Upper bound on how long receive_frame() may sit inside one blocking read
  // before it re-checks whether the connection is still usable.  A finite bound
  // is what makes teardown deterministic instead of platform dependent: the loop
  // always regains control and observes a closed socket.
  void set_poll_interval_ms(std::uint64_t milliseconds) noexcept { poll_interval_ms_ = milliseconds; }
  [[nodiscard]] std::uint64_t poll_interval_ms() const noexcept { return poll_interval_ms_; }
  // True when the last receive reported "nothing arrived yet" rather than a
  // failure.  Callers that own a loop use it to decide whether to keep waiting.
  [[nodiscard]] static bool is_idle_status(const Status& status) noexcept {
    return status.code() == ErrorCode::kTransportTimeout;
  }

 private:
  friend class Server;
  struct Socket;

  std::shared_ptr<Socket> socket_;
  // Receive reassembly buffer.  Mutable because receive_frame() is logically
  // const: it does not change what the connection is, only what has arrived.
  mutable std::vector<std::uint8_t> read_buffer_;
  std::size_t read_capacity_ = limits::kIoBufferBytes;
  std::uint64_t poll_interval_ms_ = kDefaultPollIntervalMs;
  Endpoint peer_{};
  Endpoint local_{};
  bool owns_ = false;
};

// Callback interface implemented by the coordinator server.  Every method is
// invoked from the session's own thread.  No transport lock is held while these
// run, so an implementation may call back into the transport (for example to
// send a response) without any lock ordering hazard.
class SessionHandler {
 public:
  virtual ~SessionHandler() = default;
  virtual void on_open(const Connection& connection) = 0;
  virtual void on_frame(const Connection& connection, const protocol::Frame& frame) = 0;
  virtual void on_close(const Connection& connection, const Status& reason) = 0;
};

struct ServerConfig {
  std::string bind_host = "127.0.0.1";
  std::uint16_t port = 0;  // 0 asks the operating system for an ephemeral port
  std::size_t max_sessions = limits::kSessionsMax;
  std::size_t read_buffer_bytes = limits::kIoBufferBytes;
  int backlog = static_cast<int>(limits::kBacklogMax);
  bool reuse_address = true;
};

class Server {
 public:
  Server();
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Starts listening.  On return, bound_port() reports the actual port, which
  // is what a test or a supervisor must use when port 0 was requested.
  [[nodiscard]] Status listen(const ServerConfig& config, SessionHandler& handler);
  // Stops accepting, shuts down every live session socket, joins every worker,
  // and leaves accounting at zero.  Safe to call more than once.
  void shutdown();
  [[nodiscard]] bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint16_t bound_port() const;
  [[nodiscard]] std::size_t session_count() const;
  [[nodiscard]] std::uint64_t accepted_total() const { return accepted_total_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::uint64_t rejected_total() const { return rejected_total_.load(std::memory_order_relaxed); }

 private:
  void accept_loop();
  void session_loop(std::shared_ptr<Connection> connection);

  ServerConfig config_{};
  SessionHandler* handler_ = nullptr;
  std::shared_ptr<Connection::Socket> listener_;
  std::thread acceptor_;
  mutable std::mutex mutex_;
  std::vector<std::thread> workers_;
  std::vector<std::shared_ptr<Connection>> live_;
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> accepted_total_{0};
  std::atomic<std::uint64_t> rejected_total_{0};
};

// Binds an ephemeral port and immediately releases it.  Used by tests that need
// a port number without racing another binder.
[[nodiscard]] Status reserve_ephemeral_port(std::uint16_t& out_port);

// Cross platform monotonic clock in milliseconds.  This is the only clock the
// runtime reads, and it is monotonic by construction.
[[nodiscard]] std::uint64_t monotonic_now_ms() noexcept;

}  // namespace ctf::transport

#endif  // CTF_TRANSPORT_HPP
