// Collective Traffic Fabric - networked coordinator implementation.
// Copyright 2026 Summon Software Labs.
#include "ctf/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace ctf {
namespace {

ReasonChain reasons_from(ErrorCode code, const std::string& message) {
  ReasonChain chain;
  chain.add(std::string(to_string(code)), message);
  return chain;
}

}  // namespace

struct CoordinatorHandler final : public transport::SessionHandler {
  explicit CoordinatorHandler(CoordinatorServer& owner) : owner_(owner) {}

  void on_open(const transport::Connection& connection) override {
    const std::uint64_t now = owner_.now_ms();
    SessionBinding binding;
    Status status = owner_.service_.open_session("pending-handshake", now, binding);
    if (!status.is_ok()) {
      // The session bound was reached between accept and registration.  The
      // connection is closed immediately rather than served without identity.
      ConnectionState state;
      state.session = SessionId{};
      state.rejected = true;
      std::lock_guard<std::mutex> guard(mutex_);
      sessions_.emplace(connection_key(connection), state);
      connection.shutdown_socket();
      return;
    }
    ConnectionState state;
    state.session = binding.id;
    state.incarnation = binding.incarnation;
    state.epoch = binding.epoch;
    state.hellos_pending = 1;
    state.opened_monotonic_ms = now;
    std::lock_guard<std::mutex> guard(mutex_);
    sessions_.emplace(connection_key(connection), state);
  }

  void on_frame(const transport::Connection& connection, const protocol::Frame& frame) override {
    const std::uint64_t now = owner_.now_ms();
    ConnectionState snapshot;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      auto existing = sessions_.find(connection_key(connection));
      if (existing == sessions_.end()) {
        connection.shutdown_socket();
        return;
      }
      snapshot = existing->second;
    }
    if (snapshot.rejected) {
      connection.shutdown_socket();
      return;
    }

    // The hello handshake establishes the session.  Until it is complete, every
    // other kind is refused: identity is minted by the coordinator, never
    // accepted from the peer.
    if (snapshot.hellos_pending != 0) {
      if (frame.kind != static_cast<std::uint16_t>(protocol::MessageKind::kHello)) {
        ++owner_.protocol_violations_;
        send_error(connection, frame, ErrorCode::kEnvelopeSessionUnknown,
                   "the first frame of a session must be a hello");
        connection.shutdown_socket();
        return;
      }
      protocol::HelloPayload hello;
      const Status decoded = protocol::decode_hello(frame.payload, hello);
      if (!decoded.is_ok()) {
        ++owner_.malformed_frames_;
        send_error(connection, frame, decoded.code(), decoded.message());
        connection.shutdown_socket();
        return;
      }
      {
        std::lock_guard<std::mutex> guard(mutex_);
        auto existing = sessions_.find(connection_key(connection));
        if (existing != sessions_.end()) {
          existing->second.hellos_pending = 0;
          existing->second.peer_label = hello.client_label;
        }
      }
      protocol::HelloAckPayload ack;
      ack.session = snapshot.session;
      ack.incarnation = snapshot.incarnation;
      ack.epoch = snapshot.epoch;
      ack.policy_generation = owner_.service_.authority().policy_generation();
      ack.topology_generation = owner_.service_.authority().topology_generation();
      ack.capacity_generation = owner_.service_.authority().capacity_generation();
      ack.congestion_generation = owner_.service_.authority().congestion_generation();
      ack.max_frame_payload_bytes = limits::kFramePayloadMaxBytes;
      ack.coordinator_label = owner_.config_.service.coordinator_label;
      send_typed(connection, snapshot, protocol::MessageKind::kHelloAck, frame, ack,
                 [](const protocol::HelloAckPayload& value, std::vector<std::uint8_t>& out) {
                   return protocol::encode_hello_ack(value, out);
                 });
      return;
    }

    // The connection owns exactly one session.  A frame that names a different
    // session - or the same session under a different boot incarnation - is not
    // merely stale, it is an attempt to act as somebody else, and it is refused
    // here rather than handed to the service where it would otherwise be
    // dispatched under the named session's authority.
    if (frame.envelope.session != snapshot.session || frame.envelope.incarnation != snapshot.incarnation) {
      ++owner_.protocol_violations_;
      send_error(connection, frame, ErrorCode::kEnvelopeSessionMismatch,
                 "the frame names a session or boot incarnation that this connection does not own");
      connection.shutdown_socket();
      return;
    }

    EnvelopeView envelope;
    envelope.session = frame.envelope.session;
    envelope.incarnation = frame.envelope.incarnation;
    envelope.epoch = frame.envelope.epoch;
    envelope.frame_sequence = frame.envelope.frame_sequence;
    envelope.correlation = frame.envelope.correlation;

    ReasonChain reasons;
    const Status envelope_status = owner_.service_.validate_envelope(envelope, now, reasons);
    if (!envelope_status.is_ok()) {
      ++owner_.stale_frames_;
      send_error_with_reasons(connection, frame, envelope_status.code(), envelope_status.message(), reasons);
      // A frame that fails envelope validation is not necessarily fatal: a
      // replayed or reordered frame is refused and the session continues, while
      // an unknown session or a fenced incarnation closes the connection.
      if (envelope_status.code() == ErrorCode::kEnvelopeSessionUnknown ||
          envelope_status.code() == ErrorCode::kEnvelopeSessionMismatch ||
          envelope_status.code() == ErrorCode::kEnvelopeIncarnationStale) {
        connection.shutdown_socket();
      }
      return;
    }

    ServiceRequestContext context;
    context.session = envelope.session;
    context.incarnation = envelope.incarnation;
    context.epoch = envelope.epoch;
    context.now_monotonic_ms = now;
    context.correlation = envelope.correlation;

    // Classification is always the coordinator's decision.  The kind is the
    // frame kind the transport reported; a flag that contradicts it (for
    // example a response where a request is required) is a protocol violation.
    const bool is_request = (frame.flags & protocol::kFlagRequest) != 0;
    const protocol::MessageKind kind = static_cast<protocol::MessageKind>(frame.kind);
    if (protocol_message_requires_request(kind) && !is_request) {
      ++owner_.protocol_violations_;
      send_error(connection, frame, ErrorCode::kFrameFlagsInvalid,
                 "this message kind must be sent as a request");
      return;
    }

    dispatch(connection, frame, kind, context);
  }

  void on_close(const transport::Connection& connection, const Status& reason) override {
    static_cast<void>(reason);
    const std::uint64_t now = owner_.now_ms();
    SessionId session;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      auto existing = sessions_.find(connection_key(connection));
      if (existing != sessions_.end()) {
        session = existing->second.session;
        sessions_.erase(existing);
      }
    }
    if (session.is_some()) {
      owner_.service_.close_session(session, now);
    }
  }

  [[nodiscard]] std::size_t tracked_sessions() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return sessions_.size();
  }

 private:
  struct ConnectionState {
    SessionId session{};
    BootIncarnation incarnation{};
    CoordinatorEpoch epoch{};
    std::string peer_label;
    std::uint64_t opened_monotonic_ms = 0;
    int hellos_pending = 1;
    bool rejected = false;
  };

  static std::uintptr_t connection_key(const transport::Connection& connection) {
    return reinterpret_cast<std::uintptr_t>(&connection);
  }

  static bool protocol_message_requires_request(protocol::MessageKind kind) {
    switch (kind) {
      case protocol::MessageKind::kHeartbeat:
      case protocol::MessageKind::kRegisterCollective:
      case protocol::MessageKind::kBeginAttempt:
      case protocol::MessageKind::kPlanFlowGroup:
      case protocol::MessageKind::kCancelCollective:
      case protocol::MessageKind::kRetireCollective:
      case protocol::MessageKind::kIngestEvidence:
      case protocol::MessageKind::kInspectRequest:
      case protocol::MessageKind::kExplainRequest:
      case protocol::MessageKind::kInstallPolicy:
      case protocol::MessageKind::kShutdown:
        return true;
      default:
        return false;
    }
  }

  template <typename Payload, typename Encode>
  void send_typed(const transport::Connection& connection, const ConnectionState& state,
                  protocol::MessageKind kind, const protocol::Frame& request, const Payload& payload,
                  Encode encode) {
    std::vector<std::uint8_t> body;
    const Status status = encode(payload, body);
    if (!status.is_ok()) {
      send_error(connection, request, status.code(), status.message());
      return;
    }
    protocol::Frame response;
    response.kind = static_cast<std::uint16_t>(kind);
    response.flags = static_cast<std::uint16_t>(protocol::kFlagResponse | protocol::kFlagFinal);
    response.envelope.session = state.session;
    response.envelope.incarnation = state.incarnation;
    response.envelope.epoch = state.epoch;
    response.envelope.frame_sequence = Sequence(next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1);
    response.envelope.correlation = request.envelope.correlation;
    response.payload = std::move(body);
    const Status sent = connection.send_frame(response);
    if (!sent.is_ok()) {
      ++owner_.send_failures_;
    }
  }

  void send_error(const transport::Connection& connection, const protocol::Frame& request, ErrorCode code,
                  const std::string& message) {
    send_error_with_reasons(connection, request, code, message, reasons_from(code, message));
  }

  void send_error_with_reasons(const transport::Connection& connection, const protocol::Frame& request,
                               ErrorCode code, const std::string& message, const ReasonChain& reasons) {
    ConnectionState state;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      auto existing = sessions_.find(connection_key(connection));
      if (existing == sessions_.end()) return;
      state = existing->second;
    }
    protocol::ErrorPayload payload;
    payload.code = code;
    payload.message = message;
    payload.reasons = reasons;
    send_typed(connection, state, protocol::MessageKind::kErrorResponse, request, payload,
               [](const protocol::ErrorPayload& value, std::vector<std::uint8_t>& out) {
                 return protocol::encode_error(value, out);
               });
  }

  void dispatch(const transport::Connection& connection, const protocol::Frame& frame,
                protocol::MessageKind kind, const ServiceRequestContext& context) {
    ConnectionState state;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      auto existing = sessions_.find(connection_key(connection));
      if (existing == sessions_.end()) return;
      state = existing->second;
    }

    switch (kind) {
      case protocol::MessageKind::kHeartbeat: {
        protocol::HeartbeatPayload payload;
        const Status decoded = protocol::decode_heartbeat(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        // Identity binding: the participant identity and incarnation come from
        // the frame, but the session that owns them is the connection's, so a
        // peer cannot heartbeat on behalf of another session.
        const Status status = owner_.service_.publish_participant(
            context, payload.participant, payload.incarnation, payload.node, payload.state);
        if (!status.is_ok()) {
          // A heartbeat from a fenced boot is answered, not dropped: the peer has
          // to learn that the incarnation it is using is spent.
          send_error(connection, frame, status.code(), status.message());
          return;
        }
        {
          protocol::HeartbeatAckPayload ack;
          ack.participant = payload.participant;
          ack.incarnation = payload.incarnation;
          ack.state = payload.state;
          ack.accepted = true;
          const PeerLiveness stored = owner_.service_.authority().peer_snapshot(payload.participant);
          ack.expires_at_monotonic_ms = stored.expires_at_monotonic_ms;
          ack.reasons.add("heartbeat_accepted",
                          "liveness accepted for this incarnation; the participant now holds authority "
                          "until the expiry instant reported here",
                          payload.participant);
          send_typed(connection, state, protocol::MessageKind::kHeartbeatAck, frame, ack,
                     [](const protocol::HeartbeatAckPayload& value, std::vector<std::uint8_t>& out) {
                       return protocol::encode_heartbeat_ack(value, out);
                     });
        }
        return;
      }
      case protocol::MessageKind::kRegisterCollective: {
        protocol::RegisterCollectivePayload payload;
        const Status decoded = protocol::decode_register_collective(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        RegistrationOutcome outcome;
        const Status status = owner_.service_.register_collective(context, payload.definition, outcome);
        protocol::RegisterAckPayload ack;
        ack.id = payload.definition.id;
        ack.generation = outcome.generation;
        ack.result = static_cast<std::uint8_t>(outcome.result);
        ack.registration_sequence = outcome.created ? Sequence(1) : Sequence(0);
        ack.reasons = outcome.reasons;
        if (!status.is_ok()) {
          ack.reasons.add(std::string(to_string(status.code())), status.message(), payload.definition.id);
        }
        persist_if_configured();
        send_typed(connection, state, protocol::MessageKind::kRegisterAck, frame, ack,
                   [](const protocol::RegisterAckPayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_register_ack(value, out);
                   });
        return;
      }
      case protocol::MessageKind::kBeginAttempt: {
        protocol::BeginAttemptPayload payload;
        const Status decoded = protocol::decode_begin_attempt(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        AttemptOutcome outcome;
        const Status status = owner_.service_.begin_attempt(context, payload.id, payload.generation,
                                                            payload.attempt, payload.transfer_bytes, outcome);
        protocol::AttemptAckPayload ack;
        ack.id = payload.id;
        ack.attempt = payload.attempt;
        ack.previous_attempt = outcome.previous_attempt;
        ack.state = outcome.state;
        ack.reasons = outcome.reasons;
        if (!status.is_ok() && ack.reasons.empty()) {
          ack.reasons.add(std::string(to_string(status.code())), status.message(), payload.id);
        }
        persist_if_configured();
        send_typed(connection, state, protocol::MessageKind::kAttemptAck, frame, ack,
                   [](const protocol::AttemptAckPayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_attempt_ack(value, out);
                   });
        return;
      }
      case protocol::MessageKind::kPlanFlowGroup: {
        protocol::PlanFlowGroupPayload payload;
        const Status decoded = protocol::decode_plan_flow_group(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        DecisionRecord decision;
        const Status status = owner_.service_.plan_flow_group(context, payload.request, decision);
        if (!status.is_ok()) {
          send_error(connection, frame, status.code(), status.message());
          return;
        }
        protocol::DecisionPayload response;
        response.decision = decision;
        send_typed(connection, state, protocol::MessageKind::kFlowGroupDecision, frame, response,
                   [](const protocol::DecisionPayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_decision(value, out);
                   });
        return;
      }
      case protocol::MessageKind::kCancelCollective:
      case protocol::MessageKind::kRetireCollective: {
        protocol::LifecyclePayload payload;
        const Status decoded = protocol::decode_lifecycle(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        CollectiveState out_state = CollectiveState::kUnregistered;
        ReasonChain reasons;
        const Status status = kind == protocol::MessageKind::kCancelCollective
                                  ? owner_.service_.cancel_collective(context, payload.id, payload.generation,
                                                                      payload.attempt, out_state, reasons)
                                  : owner_.service_.retire_collective(context, payload.id, payload.generation,
                                                                      payload.attempt, out_state, reasons);
        protocol::LifecycleAckPayload ack;
        ack.id = payload.id;
        ack.state = out_state;
        ack.reasons = reasons;
        if (!status.is_ok() && ack.reasons.empty()) {
          ack.reasons.add(std::string(to_string(status.code())), status.message(), payload.id);
        }
        persist_if_configured();
        send_typed(connection, state, protocol::MessageKind::kLifecycleAck, frame, ack,
                   [](const protocol::LifecycleAckPayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_lifecycle_ack(value, out);
                   });
        return;
      }
      case protocol::MessageKind::kIngestEvidence: {
        protocol::EvidencePayload payload;
        const Status decoded = protocol::decode_evidence(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        protocol::EvidenceAckPayload ack;
        ack.kind = payload.kind;
        ReasonChain reasons;
        ErrorCode code = ErrorCode::kOk;
        switch (payload.kind) {
          case protocol::EvidenceKindTag::kCapacity: {
            EvidenceGeneration generation;
            const Status status = owner_.service_.ingest_capacity(context, payload.capacity, generation, reasons);
            ack.generation = generation;
            ack.result = status.is_ok() ? 0 : 1;
            code = status.code();
            break;
          }
          case protocol::EvidenceKindTag::kCongestion: {
            EvidenceGeneration generation;
            const Status status =
                owner_.service_.ingest_congestion(context, payload.congestion, generation, reasons);
            ack.generation = generation;
            ack.result = status.is_ok() ? 0 : 1;
            code = status.code();
            break;
          }
          case protocol::EvidenceKindTag::kTopology: {
            TopologyGeneration generation;
            const Status status = owner_.service_.ingest_topology(context, payload.topology, generation, reasons);
            ack.generation = generation;
            ack.result = status.is_ok() ? 0 : 1;
            code = status.code();
            break;
          }
          case protocol::EvidenceKindTag::kPolicy: {
            PolicyGeneration generation;
            const Status status = owner_.service_.install_policy(context, payload.policy, generation,
                                                                 reasons);
            ack.generation = generation;
            ack.result = status.is_ok() ? 0 : 1;
            code = status.code();
            break;
          }
          case protocol::EvidenceKindTag::kNone:
          default:
            code = ErrorCode::kDecodeInvalidTag;
            reasons.add("evidence_kind_unknown", "the evidence kind is not defined by this version");
            break;
        }
        if (code != ErrorCode::kOk && reasons.empty()) {
          reasons.add(std::string(to_string(code)), "evidence ingestion failed");
        }
        ack.reasons = reasons;
        persist_if_configured();
        send_typed(connection, state, protocol::MessageKind::kEvidenceAck, frame, ack,
                   [](const protocol::EvidenceAckPayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_evidence_ack(value, out);
                   });
        return;
      }
      case protocol::MessageKind::kInspectRequest: {
        protocol::InspectRequestPayload payload;
        const Status decoded = protocol::decode_inspect_request(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        protocol::InspectResponsePayload response;
        response.subject = payload.subject;
        switch (payload.subject) {
          case 0:
            response.body = owner_.service_.inspect_summary();
            break;
          case 1:
            response.body = owner_.service_.inspect_collective(payload.id);
            break;
          case 2:
            response.body = owner_.service_.inspect_decisions_text(payload.offset, payload.limit);
            break;
          case 3:
            response.body = owner_.service_.inspect_peers();
            break;
          case 4:
            response.body = owner_.service_.inspect_policy();
            break;
          case 5:
            response.body = owner_.service_.inspect_topology();
            break;
          default:
            response.body = "unknown inspection subject\n";
            break;
        }
        if (response.body.size() > limits::kFramePayloadMaxBytes / 2) {
          response.body.resize(limits::kFramePayloadMaxBytes / 2);
          response.truncated = true;
        }
        send_typed(connection, state, protocol::MessageKind::kInspectResponse, frame, response,
                   [](const protocol::InspectResponsePayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_inspect_response(value, out);
                   });
        return;
      }
      case protocol::MessageKind::kExplainRequest: {
        protocol::ExplainRequestPayload payload;
        const Status decoded = protocol::decode_explain_request(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        protocol::ExplainResponsePayload response;
        std::string text;
        if (payload.has_correlation) {
          response.found = owner_.service_.explain_correlation(payload.correlation, response.decision, text);
        } else {
          response.found = owner_.service_.explain(payload.id, payload.attempt, response.decision, text);
        }
        response.explanation = text;
        send_typed(connection, state, protocol::MessageKind::kExplainResponse, frame, response,
                   [](const protocol::ExplainResponsePayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_explain_response(value, out);
                   });
        return;
      }
      case protocol::MessageKind::kInstallPolicy: {
        protocol::InstallPolicyPayload payload;
        const Status decoded = protocol::decode_install_policy(frame.payload, payload);
        if (!decoded.is_ok()) return reject(connection, frame, decoded);
        protocol::InstallPolicyAckPayload ack;
        PolicyGeneration generation;
        ReasonChain reasons;
        const Status status = owner_.service_.install_policy(context, payload.policy, generation, reasons);
        ack.generation = generation;
        ack.result = status.is_ok() ? 0 : 1;
        ack.reasons = reasons;
        if (!status.is_ok() && ack.reasons.empty()) {
          ack.reasons.add(std::string(to_string(status.code())), status.message());
        }
        persist_if_configured();
        send_typed(connection, state, protocol::MessageKind::kInstallPolicyAck, frame, ack,
                   [](const protocol::InstallPolicyAckPayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_install_policy_ack(value, out);
                   });
        return;
      }
      case protocol::MessageKind::kShutdown: {
        // A shutdown request asks this coordinator to stop accepting new work.
        // It is answered before the transport begins closing, so the peer sees a
        // definite acknowledgement rather than a dropped connection.
        protocol::InspectResponsePayload ack;
        ack.subject = 255;
        ack.body = "shutdown acknowledged; no new work will be accepted";
        send_typed(connection, state, protocol::MessageKind::kShutdownAck, frame, ack,
                   [](const protocol::InspectResponsePayload& value, std::vector<std::uint8_t>& out) {
                     return protocol::encode_inspect_response(value, out);
                   });
        owner_.shutdown_requested_.store(true, std::memory_order_release);
        return;
      }
      case protocol::MessageKind::kHello:
        ++owner_.protocol_violations_;
        send_error(connection, frame, ErrorCode::kEnvelopeProvenanceRejected,
                   "a second hello on an established session is refused");
        return;
      default:
        ++owner_.malformed_frames_;
        send_error(connection, frame, ErrorCode::kFrameKindUnknown,
                   "the frame kind is not valid on an established session");
        return;
    }
  }

  void reject(const transport::Connection& connection, const protocol::Frame& frame, const Status& status) {
    ++owner_.malformed_frames_;
    send_error(connection, frame, status.code(), status.message());
  }

  void persist_if_configured() {
    if (!owner_.config_.persist_on_mutation) return;
    const Status status = owner_.service_.flush(owner_.now_ms());
    static_cast<void>(status);
  }

  CoordinatorServer& owner_;
  mutable std::mutex mutex_;
  // Keyed by the address of the connection object the transport owns.  That
  // address is stable for the lifetime of the session and is never used as an
  // identity on the wire.
  std::unordered_map<std::uintptr_t, ConnectionState> sessions_;
  std::atomic<std::uint64_t> next_sequence_{0};
};

CoordinatorServer::CoordinatorServer(CoordinatorConfig config)
    : config_(std::move(config)),
      service_(config_.service),
      handler_(std::make_unique<CoordinatorHandler>(*this)) {}

CoordinatorServer::~CoordinatorServer() {
  if (running_.load(std::memory_order_acquire)) {
    stop(now_ms());
  }
}

std::uint64_t CoordinatorServer::now_ms() const { return transport::monotonic_now_ms(); }

Status CoordinatorServer::start(std::uint64_t now_monotonic_ms) {
  static_cast<void>(now_monotonic_ms);
  if (running_.load(std::memory_order_acquire)) {
    return Status(ErrorCode::kStateDuplicateRegistration, "the coordinator is already running");
  }
  Status status = service_.start(now_ms());
  if (!status.is_ok()) return status;
  if (!config_.snapshot_path.empty()) {
    service_.configure_persistence(config_.snapshot_path, now_ms());
    LoadDisposition disposition = LoadDisposition::kMissing;
    const Status restored = service_.restore(config_.snapshot_path, now_ms(), disposition);
    // A missing snapshot is a first boot, not a failure.  A snapshot that exists
    // but is rejected is reported and the coordinator still starts, because
    // starting with durable definitions is not the same as trusting them.
    if (!restored.is_ok() && disposition != LoadDisposition::kMissing) {
      service_.configure_persistence(config_.snapshot_path, now_ms());
    }
  }
  status = server_.listen(config_.transport, *handler_);
  if (!status.is_ok()) {
    service_.stop(now_ms());
    return status;
  }
  running_.store(true, std::memory_order_release);
  return Status::ok();
}

Status CoordinatorServer::stop(std::uint64_t now_monotonic_ms) {
  static_cast<void>(now_monotonic_ms);
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return Status::ok();
  }

  // Shutdown order matters, and this order is the one that cannot deadlock.
  //
  // 1. Stop the service first.  It takes its own lock, marks itself not running,
  //    drops every session and performs the durable flush with no lock held.
  //    This must happen BEFORE the transport is torn down: a session thread that
  //    finishes at that moment calls back into the service from on_close(), and
  //    if the service were still holding its state lock across the waiting part
  //    of teardown, that call would block forever and the transport join would
  //    never return.
  const Status stopped = service_.stop(now_ms());
  // 2. Stop accepting, unblock every session thread and join them.  By now no
  //    callback can need service state, and every receive is interruptible, so
  //    the joins are guaranteed to complete.
  server_.shutdown();
  return stopped;
}

bool CoordinatorServer::is_running() const noexcept { return running_.load(std::memory_order_acquire); }

std::uint16_t CoordinatorServer::port() const { return server_.bound_port(); }

std::size_t CoordinatorServer::session_count() const { return server_.session_count(); }

std::uint64_t CoordinatorServer::accepted_total() const { return server_.accepted_total(); }

std::uint64_t CoordinatorServer::rejected_total() const { return server_.rejected_total(); }

std::uint64_t CoordinatorServer::stale_frame_count() const { return stale_frames_.load(std::memory_order_relaxed); }

std::uint64_t CoordinatorServer::malformed_frame_count() const {
  return malformed_frames_.load(std::memory_order_relaxed);
}

std::uint64_t CoordinatorServer::protocol_violation_count() const {
  return protocol_violations_.load(std::memory_order_relaxed);
}

std::uint64_t CoordinatorServer::send_failure_count() const {
  return send_failures_.load(std::memory_order_relaxed);
}

std::uint64_t CoordinatorServer::clock_offset_ms() const { return config_.clock_offset_ms; }

CoordinatorConfig CoordinatorServer::config() const { return config_; }

Status CoordinatorServer::flush(std::uint64_t now_monotonic_ms) {
  static_cast<void>(now_monotonic_ms);
  return service_.flush(now_ms());
}

Status CoordinatorServer::restore(std::uint64_t now_monotonic_ms, LoadDisposition& disposition) {
  static_cast<void>(now_monotonic_ms);
  return service_.restore(config_.snapshot_path, now_ms(), disposition);
}

}  // namespace ctf
