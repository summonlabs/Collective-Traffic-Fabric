// Collective Traffic Fabric - loopback TCP transport implementation.
// Copyright 2026 Summon Software Labs.
#include "ctf/transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ctf::transport {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
using SockLen = int;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
using SockLen = socklen_t;
#endif

// Winsock requires process wide initialisation.  It is performed once, from a
// function local static, so it cannot race with itself and cannot be reordered
// before any socket use.
class SocketRuntime {
 public:
  SocketRuntime() {
#if defined(_WIN32)
    WSADATA data;
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    ok_ = result == 0;
#else
    ok_ = true;
#endif
  }
  ~SocketRuntime() {
#if defined(_WIN32)
    if (ok_) WSACleanup();
#endif
  }
  [[nodiscard]] bool ok() const noexcept { return ok_; }

 private:
  bool ok_ = false;
};

bool socket_runtime_ready() {
  static SocketRuntime runtime;
  return runtime.ok();
}

void close_native(NativeSocket socket) noexcept {
  if (socket == kInvalidSocket) return;
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

// Releases a thread that is already inside a blocking call on this socket.
//
// shutdown() is the portable way to do it, but on Windows a *blocking* recv()
// that is already in progress is not reliably released by it: the call can stay
// parked until the peer sends something or the process exits, which would make
// teardown wait forever.  CancelIoEx() is the documented mechanism for exactly
// this situation, so both are used.  Neither closes the descriptor: the owner
// still closes it, so there is no window in which another thread could observe a
// reused handle.
void shutdown_native(NativeSocket socket) noexcept {
  if (socket == kInvalidSocket) return;
#if defined(_WIN32)
  ::CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr);
  ::shutdown(socket, SD_BOTH);
#else
  ::shutdown(socket, SHUT_RDWR);
#endif
}

bool wait_ready(NativeSocket socket, bool for_read, std::uint64_t timeout_ms) {
  if (socket == kInvalidSocket) return false;
  fd_set set;
  FD_ZERO(&set);
  FD_SET(socket, &set);
  timeval timeout;
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
  const int result = ::select(static_cast<int>(socket) + 1, for_read ? &set : nullptr,
                              for_read ? nullptr : &set, nullptr, &timeout);
  return result > 0;
}

Status last_socket_error(const char* what) {
#if defined(_WIN32)
  const int code = WSAGetLastError();
  return Status(ErrorCode::kTransportReceiveFailed,
                std::string(what) + " failed with socket error " + std::to_string(code));
#else
  return Status(ErrorCode::kTransportReceiveFailed,
                std::string(what) + " failed with errno " + std::to_string(errno));
#endif
}

std::string format_endpoint(const sockaddr_in& address) {
  char buffer[32] = {};
  const char* text = ::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
  std::string host = text != nullptr ? std::string(text) : std::string("?");
  return host + ":" + std::to_string(static_cast<unsigned>(ntohs(address.sin_port)));
}

bool fill_sockaddr(const Endpoint& endpoint, sockaddr_in& out) {
  std::memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(endpoint.port);
  if (endpoint.host.empty()) {
    out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return true;
  }
  return ::inet_pton(AF_INET, endpoint.host.c_str(), &out.sin_addr) == 1;
}

void configure_socket(NativeSocket socket) {
  int one = 1;
  ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#if !defined(_WIN32)
  ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
}

}  // namespace

struct Connection::Socket {
  NativeSocket handle = kInvalidSocket;
  mutable std::mutex mutex;
  bool closed = false;
  // Set only by release_for_teardown(): the descriptor has been closed, so the
  // destructor must not close it a second time after the number may have been
  // reused by an unrelated socket.
  bool released = false;

  ~Socket() {
    std::lock_guard<std::mutex> guard(mutex);
    if (!released) {
      close_native(handle);
      handle = kInvalidSocket;
    }
  }
};

std::uint64_t monotonic_now_ms() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

Status reserve_ephemeral_port(std::uint16_t& out_port) {
  if (!socket_runtime_ready()) {
    return Status(ErrorCode::kTransportSocketFailed, "the socket runtime is unavailable");
  }
  NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return Status(ErrorCode::kTransportSocketFailed, "cannot create a socket to reserve a port");
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_native(socket);
    return Status(ErrorCode::kTransportBindFailed, "cannot bind a loopback socket to reserve a port");
  }
  SockLen length = sizeof(address);
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    close_native(socket);
    return Status(ErrorCode::kTransportAddressInvalid, "cannot read back the reserved port");
  }
  out_port = ntohs(address.sin_port);
  close_native(socket);
  return Status::ok();
}

Connection::~Connection() = default;
Connection::Connection(Connection&& other) noexcept = default;
Connection& Connection::operator=(Connection&& other) noexcept = default;

bool Connection::is_open() const noexcept {
  if (!socket_) return false;
  std::lock_guard<std::mutex> guard(socket_->mutex);
  return !socket_->closed && socket_->handle != kInvalidSocket;
}

void Connection::shutdown_socket() const noexcept {
  if (!socket_) return;
  NativeSocket handle = kInvalidSocket;
  {
    std::lock_guard<std::mutex> guard(socket_->mutex);
    if (socket_->released) return;
    handle = socket_->handle;
  }
  shutdown_native(handle);
}

void Connection::release_for_teardown() const noexcept {
  if (!socket_) return;
  NativeSocket handle = kInvalidSocket;
  {
    std::lock_guard<std::mutex> guard(socket_->mutex);
    if (socket_->released) return;
    handle = socket_->handle;
    socket_->released = true;
    socket_->closed = true;
    socket_->handle = kInvalidSocket;
  }
  if (handle == kInvalidSocket) return;
  // Closing is the only operation that reliably releases a thread already parked
  // inside recv() or select() on Windows; shutdown() and CancelIoEx() can both
  // leave it asleep until the TCP keepalive fires.  Closing while a read is
  // pending is defined to abort that read, and the Connection object stays alive
  // because its owner holds a shared_ptr until its thread has finished, so no
  // other thread can observe a reused descriptor here.
  close_native(handle);
}

Status Connection::connect_to(const Endpoint& endpoint, Connection& out) {
  if (!socket_runtime_ready()) {
    return Status(ErrorCode::kTransportSocketFailed, "the socket runtime is unavailable");
  }
  sockaddr_in address{};
  if (!fill_sockaddr(endpoint, address)) {
    return Status(ErrorCode::kTransportAddressInvalid, "endpoint host is not a valid IPv4 literal");
  }
  NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return Status(ErrorCode::kTransportSocketFailed, "cannot create a client socket");
  }
  configure_socket(socket);
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_native(socket);
    return Status(ErrorCode::kTransportConnectFailed,
                  "cannot connect to " + endpoint.host + ":" + std::to_string(endpoint.port));
  }
  Connection connection;
  connection.socket_ = std::make_shared<Socket>();
  connection.socket_->handle = socket;
  connection.peer_ = endpoint;
  sockaddr_in local{};
  SockLen length = sizeof(local);
  if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
    connection.local_.host = endpoint.host;
    connection.local_.port = ntohs(local.sin_port);
  }
  out = std::move(connection);
  return Status::ok();
}

Status Connection::send_raw(const std::uint8_t* data, std::size_t size) const {
  if (!socket_) {
    return Status(ErrorCode::kTransportNotStarted, "connection is not open");
  }
  NativeSocket handle = kInvalidSocket;
  {
    std::lock_guard<std::mutex> guard(socket_->mutex);
    if (socket_->closed) {
      return Status(ErrorCode::kTransportClosed, "connection is closed");
    }
    handle = socket_->handle;
  }
  std::size_t sent = 0;
  while (sent < size) {
    const std::size_t remaining = size - sent;
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1u << 20));
#if defined(_WIN32)
    const int written = ::send(handle, reinterpret_cast<const char*>(data + sent), chunk, 0);
#else
    const int written = static_cast<int>(
        ::send(handle, reinterpret_cast<const char*>(data + sent), static_cast<std::size_t>(chunk), 0));
#endif
    if (written <= 0) {
      return last_socket_error("send");
    }
    sent += static_cast<std::size_t>(written);
  }
  return Status::ok();
}

Status Connection::send_frame(const protocol::Frame& frame) const {
  std::vector<std::uint8_t> bytes;
  Status status = protocol::encode_frame(frame, bytes);
  if (!status.is_ok()) return status;
  return send_raw(bytes.data(), bytes.size());
}

Status Connection::receive_raw(std::uint8_t* data, std::size_t capacity, std::size_t& received) const {
  received = 0;
  if (!socket_) {
    return Status(ErrorCode::kTransportNotStarted, "connection is not open");
  }
  NativeSocket handle = kInvalidSocket;
  {
    std::lock_guard<std::mutex> guard(socket_->mutex);
    if (socket_->closed) {
      return Status(ErrorCode::kTransportClosed, "connection is closed");
    }
    handle = socket_->handle;
  }
  // Wait in bounded slices so this loop always regains control.  A genuinely
  // quiet peer is indistinguishable from a shut down one from inside a blocking
  // read, and teardown must not depend on being able to interrupt one.
  if (!wait_ready(handle, true, poll_interval_ms_)) {
    if (!is_open()) {
      return Status(ErrorCode::kTransportClosed, "connection was shut down while waiting to read");
    }
    return Status(ErrorCode::kTransportTimeout, "no bytes arrived within the poll interval");
  }
#if defined(_WIN32)
  const int got = ::recv(handle, reinterpret_cast<char*>(data), static_cast<int>(capacity), 0);
#else
  const int got = static_cast<int>(::recv(handle, reinterpret_cast<char*>(data), capacity, 0));
#endif
  if (got == 0) {
    return Status(ErrorCode::kTransportClosed, "peer closed the connection");
  }
  if (got < 0) {
    return last_socket_error("recv");
  }
  received = static_cast<std::size_t>(got);
  return Status::ok();
}

Status Connection::receive_frame(protocol::Frame& out) const {
  if (!socket_) {
    return Status(ErrorCode::kTransportNotStarted, "connection is not open");
  }
  if (read_buffer_.capacity() < read_capacity_) {
    read_buffer_.reserve(read_capacity_);
  }
  for (;;) {
    if (!read_buffer_.empty()) {
      protocol::Frame frame;
      std::size_t consumed = 0;
      Status status = protocol::decode_frame(read_buffer_.data(), read_buffer_.size(), frame, consumed);
      if (status.is_ok()) {
        read_buffer_.erase(read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
        out = std::move(frame);
        return Status::ok();
      }
      // Only "not enough bytes yet" is retryable; every other failure is a
      // protocol violation and is reported without consuming anything.
      if (status.code() != ErrorCode::kFrameTruncatedHeader &&
          status.code() != ErrorCode::kFrameTruncatedPayload) {
        return status;
      }
    }
    if (read_buffer_.size() >= read_capacity_ + limits::kFramePayloadMaxBytes + protocol::kHeaderBytes) {
      return Status(ErrorCode::kFrameLengthOutOfRange, "receive buffer exceeded its bound without a frame");
    }
    std::vector<std::uint8_t> chunk(read_capacity_);
    std::size_t received = 0;
    Status status = receive_raw(chunk.data(), chunk.size(), received);
    if (!status.is_ok()) {
      if (status.code() == ErrorCode::kTransportTimeout) {
        // A quiet peer is not an error: the loop simply re-checks the socket
        // state and waits again.  This is what keeps a blocked reader responsive
        // to a shutdown that arrives from another thread.
        continue;
      }
      if (status.code() == ErrorCode::kTransportClosed && !read_buffer_.empty()) {
        return Status(ErrorCode::kFrameTruncatedPayload, "peer closed the connection mid frame");
      }
      return status;
    }
    read_buffer_.insert(read_buffer_.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(received));
  }
}

bool Connection::wait_readable(std::uint64_t timeout_ms) const {
  if (!socket_) return false;
  NativeSocket handle = kInvalidSocket;
  {
    std::lock_guard<std::mutex> guard(socket_->mutex);
    if (socket_->closed) return false;
    handle = socket_->handle;
  }
  return wait_ready(handle, true, timeout_ms);
}

bool Connection::wait_writable(std::uint64_t timeout_ms) const {
  if (!socket_) return false;
  NativeSocket handle = kInvalidSocket;
  {
    std::lock_guard<std::mutex> guard(socket_->mutex);
    if (socket_->closed) return false;
    handle = socket_->handle;
  }
  return wait_ready(handle, false, timeout_ms);
}

Server::Server() = default;
Server::~Server() {
  shutdown();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    listener_.reset();
  }
}

std::uint16_t Server::bound_port() const {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!listener_ || listener_->handle == kInvalidSocket) return 0;
  sockaddr_in address{};
  SockLen length = sizeof(address);
  if (::getsockname(listener_->handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

std::size_t Server::session_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return live_.size();
}

Status Server::listen(const ServerConfig& config, SessionHandler& handler) {
  if (!socket_runtime_ready()) {
    return Status(ErrorCode::kTransportSocketFailed, "the socket runtime is unavailable");
  }
  if (running_.load(std::memory_order_acquire)) {
    return Status(ErrorCode::kStateDuplicateRegistration, "the server is already listening");
  }
  config_ = config;
  handler_ = &handler;
  if (config_.max_sessions == 0) config_.max_sessions = 1;
  if (config_.max_sessions > limits::kSessionsMax) config_.max_sessions = limits::kSessionsMax;
  if (config_.backlog <= 0) config_.backlog = 1;

  sockaddr_in address{};
  if (!fill_sockaddr(Endpoint{config_.bind_host, config_.port}, address)) {
    return Status(ErrorCode::kTransportAddressInvalid, "bind host is not a valid IPv4 literal");
  }
  NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) {
    return Status(ErrorCode::kTransportSocketFailed, "cannot create a listening socket");
  }
  if (config_.reuse_address) {
    int one = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
  }
  if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close_native(socket);
    return Status(ErrorCode::kTransportBindFailed,
                  "cannot bind " + config_.bind_host + ":" + std::to_string(config_.port));
  }
  if (::listen(socket, config_.backlog) != 0) {
    close_native(socket);
    return Status(ErrorCode::kTransportListenFailed, "cannot listen on the bound socket");
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    listener_ = std::make_shared<Connection::Socket>();
    listener_->handle = socket;
    live_.clear();
    workers_.clear();
  }
  accepted_total_.store(0, std::memory_order_relaxed);
  rejected_total_.store(0, std::memory_order_relaxed);
  running_.store(true, std::memory_order_release);
  try {
    acceptor_ = std::thread([this]() { accept_loop(); });
  } catch (const std::system_error& error) {
    running_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> guard(mutex_);
      close_native(listener_->handle);
      listener_->handle = kInvalidSocket;
      listener_.reset();
    }
    return Status(ErrorCode::kTransportSocketFailed,
                  std::string("cannot start the acceptor thread: ") + error.what());
  }
  return Status::ok();
}

void Server::accept_loop() {
  for (;;) {
    if (!running_.load(std::memory_order_acquire)) break;
    NativeSocket listener_handle = kInvalidSocket;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (!listener_) break;
      listener_handle = listener_->handle;
    }
    if (listener_handle == kInvalidSocket) break;

    sockaddr_in peer_address{};
    SockLen peer_length = sizeof(peer_address);
    NativeSocket accepted = ::accept(listener_handle, reinterpret_cast<sockaddr*>(&peer_address), &peer_length);
    if (accepted == kInvalidSocket) {
      if (!running_.load(std::memory_order_acquire)) break;
      // A transient accept failure (for example a client that vanished during
      // the handshake) must not kill the acceptor: it retries, and the loop is
      // only left through the running flag or a closed listener.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    configure_socket(accepted);

    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (!running_.load(std::memory_order_acquire)) {
        close_native(accepted);
        break;
      }
      if (live_.size() >= config_.max_sessions) {
        // Over the session bound the connection is closed immediately and
        // counted, never queued: unbounded work per input is forbidden.
        rejected_total_.fetch_add(1, std::memory_order_relaxed);
        close_native(accepted);
        continue;
      }
      auto connection = std::make_shared<Connection>();
      connection->socket_ = std::make_shared<Connection::Socket>();
      connection->socket_->handle = accepted;
      connection->peer_.host = peer_address.sin_addr.s_addr != 0 ? "127.0.0.1" : "127.0.0.1";
      connection->peer_.port = ntohs(peer_address.sin_port);
      connection->read_capacity_ = config_.read_buffer_bytes;
      live_.push_back(connection);
      accepted_total_.fetch_add(1, std::memory_order_relaxed);
      workers_.emplace_back([this, connection]() { session_loop(connection); });
    }
  }
}

void Server::session_loop(std::shared_ptr<Connection> connection) {
  if (handler_ != nullptr) {
    handler_->on_open(*connection);
  }
  Status reason = Status::ok();
  for (;;) {
    if (!running_.load(std::memory_order_acquire)) {
      reason = Status(ErrorCode::kTransportClosed, "the server is shutting down");
      break;
    }
    protocol::Frame frame;
    Status status = connection->receive_frame(frame);
    if (status.code() == ErrorCode::kTransportTimeout) {
      // Nothing arrived yet.  A quiet peer is not a closed one: the loop
      // re-checks the running flag above and keeps waiting, which is what makes
      // a blocked reader responsive to teardown.
      continue;
    }
    if (!status.is_ok()) {
      // Any other failure ends the session, including the peer closing its half
      // and the descriptor being released by teardown.  A closed connection must
      // never be retried: doing so would leave the session in the live set
      // forever and leak the worker.
      reason = status;
      break;
    }
    if (handler_ != nullptr) {
      handler_->on_frame(*connection, frame);
    }
  }
  connection->shutdown_socket();
  if (handler_ != nullptr) {
    handler_->on_close(*connection, reason);
  }
  std::lock_guard<std::mutex> guard(mutex_);
  live_.erase(std::remove_if(live_.begin(), live_.end(),
                             [&connection](const std::shared_ptr<Connection>& candidate) {
                               return candidate == connection;
                             }),
              live_.end());
}

void Server::shutdown() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    // Even when the server was never started, any listener that exists must be
    // released so a repeated listen() does not leak a descriptor.
    std::lock_guard<std::mutex> guard(mutex_);
    if (listener_ && listener_->handle != kInvalidSocket) {
      close_native(listener_->handle);
      listener_->handle = kInvalidSocket;
    }
    return;
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (listener_ && listener_->handle != kInvalidSocket) {
      // Closing the listening socket is what unblocks accept().
      close_native(listener_->handle);
      listener_->handle = kInvalidSocket;
    }
    for (const std::shared_ptr<Connection>& connection : live_) {
      // Release, do not merely shut down.  The session thread may already be
      // parked in a read, and only closing the descriptor is guaranteed to
      // release it.  The Connection object stays alive because the owner holds a
      // shared_ptr until its thread has finished.
      connection->release_for_teardown();
    }
  }
  // The joins below happen with no lock held and every descriptor has already
  // been released, so no worker can still be blocked in a receive: that is the
  // ordering that makes this teardown deadlock free.
  if (acceptor_.joinable()) {
    acceptor_.join();
  }
  std::vector<std::thread> workers;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    workers.swap(workers_);
  }
  // The joins below happen with no lock held, every socket has already been
  // cancelled and shut down, and every receive waits in bounded slices, so no
  // worker can still be blocked in a receive.
  for (std::thread& worker : workers) {
    if (worker.joinable()) worker.join();
  }
  std::vector<std::shared_ptr<Connection>> remaining;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    remaining.swap(live_);
  }
  for (const std::shared_ptr<Connection>& connection : remaining) {
    connection->release_for_teardown();
  }
  remaining.clear();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    listener_.reset();
  }
}

}  // namespace ctf::transport
