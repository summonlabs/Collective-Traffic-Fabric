// Collective Traffic Fabric - client session implementation.
// Copyright 2026 Summon Software Labs.
#include "support/client_session.hpp"

#include <utility>

#include "ctf/transport.hpp"

namespace ctf::test {
namespace {

Status decode_or_error(protocol::MessageKind response_kind,
                       const std::vector<std::uint8_t>& response_payload, ErrorCode& code,
                       std::string& message) {
  if (response_kind != protocol::MessageKind::kErrorResponse) return Status::ok();
  protocol::ErrorPayload error;
  const Status decoded = protocol::decode_error(response_payload, error);
  if (!decoded.is_ok()) return decoded;
  code = error.code;
  message = error.message;
  if (!error.reasons.empty()) {
    message += " reasons=[";
    message += error.reasons.to_string();
    message += "]";
  }
  return Status(ErrorCode::kInternalInvariant, "the coordinator returned an error response");
}

}  // namespace

Status ClientSession::connect(const std::string& host, std::uint16_t port, const std::string& label,
                              std::uint64_t now_ms) {
  transport::Endpoint endpoint;
  endpoint.host = host;
  endpoint.port = port;
  Status status = transport::Connection::connect_to(endpoint, connection_);
  if (!status.is_ok()) return status;

  protocol::HelloPayload hello;
  hello.client_label = label;
  hello.client_version = "ctf-test-client/1.0.0";
  hello.client_boot_monotonic_ms = now_ms;
  std::vector<std::uint8_t> payload;
  status = protocol::encode_hello(hello, payload);
  if (!status.is_ok()) return status;

  protocol::Frame frame;
  frame.kind = static_cast<std::uint16_t>(protocol::MessageKind::kHello);
  frame.flags = protocol::kFlagRequest;
  frame.payload = std::move(payload);
  status = connection_.send_frame(frame);
  if (!status.is_ok()) return status;

  protocol::Frame response;
  status = connection_.receive_frame(response);
  if (!status.is_ok()) return status;
  if (static_cast<protocol::MessageKind>(response.kind) != protocol::MessageKind::kHelloAck) {
    return Status(ErrorCode::kEnvelopeSessionUnknown, "the coordinator did not acknowledge the hello");
  }
  status = protocol::decode_hello_ack(response.payload, hello_);
  if (!status.is_ok()) return status;
  connected_ = true;
  sequence_ = 0;
  return Status::ok();
}

Status ClientSession::send(protocol::MessageKind kind, std::uint16_t flags,
                           const std::vector<std::uint8_t>& payload) {
  if (!connected_) {
    return Status(ErrorCode::kTransportNotStarted, "the session is not connected");
  }
  ++sequence_;
  ++correlation_;
  protocol::Frame frame;
  frame.kind = static_cast<std::uint16_t>(kind);
  frame.flags = flags;
  frame.envelope.session = hello_.session;
  frame.envelope.incarnation = hello_.incarnation;
  frame.envelope.epoch = hello_.epoch;
  frame.envelope.frame_sequence = Sequence(sequence_);
  frame.envelope.correlation = correlation_;
  frame.payload = payload;
  return connection_.send_frame(frame);
}

Status ClientSession::request(protocol::MessageKind kind, const std::vector<std::uint8_t>& payload,
                              protocol::MessageKind& response_kind,
                              std::vector<std::uint8_t>& response_payload) {
  Status status = send(kind, protocol::kFlagRequest, payload);
  if (!status.is_ok()) return status;
  protocol::Frame response;
  status = connection_.receive_frame(response);
  if (!status.is_ok()) return status;
  response_kind = static_cast<protocol::MessageKind>(response.kind);
  response_payload = response.payload;
  ErrorCode code = ErrorCode::kOk;
  std::string message;
  const Status failure = decode_or_error(response_kind, response_payload, code, message);
  if (!failure.is_ok()) return Status(code, message);
  return Status::ok();
}

Status ClientSession::heartbeat(ParticipantId participant, BootIncarnation incarnation, NodeId node,
                                ParticipantState state) {
  protocol::HeartbeatPayload payload;
  payload.participant = participant;
  payload.incarnation = incarnation;
  payload.node = node;
  payload.state = state;
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_heartbeat(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kHeartbeat, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kHeartbeatAck) {
    return Status(ErrorCode::kInternalInvariant, "unexpected heartbeat response");
  }
  protocol::HeartbeatAckPayload ack;
  status = protocol::decode_heartbeat_ack(response_body, ack);
  if (!status.is_ok()) return status;
  if (!ack.accepted) {
    return Status(ErrorCode::kEnvelopeIncarnationStale,
                  "the coordinator did not accept this liveness publication");
  }
  return Status::ok();
}

Status ClientSession::register_collective(const CollectiveDefinition& definition,
                                          protocol::RegisterAckPayload& out) {
  protocol::RegisterCollectivePayload payload;
  payload.definition = definition;
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_register_collective(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kRegisterCollective, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kRegisterAck) {
    return Status(ErrorCode::kInternalInvariant, "unexpected registration response");
  }
  return protocol::decode_register_ack(response_body, out);
}

Status ClientSession::begin_attempt(CollectiveId id, CollectiveGeneration generation,
                                    CollectiveAttemptId attempt, std::uint64_t transfer_bytes,
                                    protocol::AttemptAckPayload& out) {
  protocol::BeginAttemptPayload payload;
  payload.id = id;
  payload.generation = generation;
  payload.attempt = attempt;
  payload.transfer_bytes = transfer_bytes;
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_begin_attempt(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kBeginAttempt, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kAttemptAck) {
    return Status(ErrorCode::kInternalInvariant, "unexpected attempt response");
  }
  return protocol::decode_attempt_ack(response_body, out);
}

Status ClientSession::plan_flow_group(const DecisionRequest& request_value, DecisionRecord& out) {
  protocol::PlanFlowGroupPayload payload;
  payload.request = request_value;
  payload.request.session = hello_.session;
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_plan_flow_group(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kPlanFlowGroup, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kFlowGroupDecision) {
    return Status(ErrorCode::kInternalInvariant, "unexpected decision response");
  }
  protocol::DecisionPayload decision;
  status = protocol::decode_decision(response_body, decision);
  if (!status.is_ok()) return status;
  out = std::move(decision.decision);
  return Status::ok();
}

Status ClientSession::cancel(CollectiveId id, CollectiveGeneration generation, CollectiveAttemptId attempt,
                             protocol::LifecycleAckPayload& out) {
  protocol::LifecyclePayload payload;
  payload.id = id;
  payload.generation = generation;
  payload.attempt = attempt;
  payload.reason = "test cancellation";
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_lifecycle(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kCancelCollective, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kLifecycleAck) {
    return Status(ErrorCode::kInternalInvariant, "unexpected lifecycle response");
  }
  return protocol::decode_lifecycle_ack(response_body, out);
}

Status ClientSession::retire(CollectiveId id, CollectiveGeneration generation, CollectiveAttemptId attempt,
                             protocol::LifecycleAckPayload& out) {
  protocol::LifecyclePayload payload;
  payload.id = id;
  payload.generation = generation;
  payload.attempt = attempt;
  payload.reason = "test retirement";
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_lifecycle(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kRetireCollective, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kLifecycleAck) {
    return Status(ErrorCode::kInternalInvariant, "unexpected lifecycle response");
  }
  return protocol::decode_lifecycle_ack(response_body, out);
}

Status ClientSession::inspect(std::uint8_t subject, CollectiveId id, std::uint32_t offset,
                              std::uint32_t limit, std::string& body) {
  protocol::InspectRequestPayload payload;
  payload.subject = subject;
  payload.id = id;
  payload.offset = offset;
  payload.limit = limit;
  std::vector<std::uint8_t> encoded;
  Status status = protocol::encode_inspect_request(payload, encoded);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kInspectRequest, encoded, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kInspectResponse) {
    return Status(ErrorCode::kInternalInvariant, "unexpected inspection response");
  }
  protocol::InspectResponsePayload response;
  status = protocol::decode_inspect_response(response_body, response);
  if (!status.is_ok()) return status;
  body = std::move(response.body);
  return Status::ok();
}

Status ClientSession::explain(CollectiveId id, CollectiveAttemptId attempt, std::string& text) {
  protocol::ExplainRequestPayload payload;
  payload.id = id;
  payload.attempt = attempt;
  std::vector<std::uint8_t> encoded;
  Status status = protocol::encode_explain_request(payload, encoded);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kExplainRequest, encoded, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kExplainResponse) {
    return Status(ErrorCode::kInternalInvariant, "unexpected explanation response");
  }
  protocol::ExplainResponsePayload response;
  status = protocol::decode_explain_response(response_body, response);
  if (!status.is_ok()) return status;
  text = std::move(response.explanation);
  if (!response.found) {
    return Status(ErrorCode::kCompletionUnknownCollective, "no retained decision to explain");
  }
  return Status::ok();
}

Status ClientSession::ingest_capacity(const CapacityEvidence& evidence) {
  protocol::EvidencePayload payload;
  payload.kind = protocol::EvidenceKindTag::kCapacity;
  payload.capacity = evidence;
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_evidence(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kIngestEvidence, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kEvidenceAck) {
    return Status(ErrorCode::kInternalInvariant, "unexpected evidence response");
  }
  protocol::EvidenceAckPayload ack;
  return protocol::decode_evidence_ack(response_body, ack);
}

Status ClientSession::ingest_congestion(const CongestionEvidence& evidence) {
  protocol::EvidencePayload payload;
  payload.kind = protocol::EvidenceKindTag::kCongestion;
  payload.congestion = evidence;
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_evidence(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kIngestEvidence, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kEvidenceAck) {
    return Status(ErrorCode::kInternalInvariant, "unexpected evidence response");
  }
  protocol::EvidenceAckPayload ack;
  return protocol::decode_evidence_ack(response_body, ack);
}

Status ClientSession::ingest_topology(const TopologyEvidence& evidence) {
  protocol::EvidencePayload payload;
  payload.kind = protocol::EvidenceKindTag::kTopology;
  payload.topology = evidence;
  std::vector<std::uint8_t> body;
  Status status = protocol::encode_evidence(payload, body);
  if (!status.is_ok()) return status;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  status = request(protocol::MessageKind::kIngestEvidence, body, response_kind, response_body);
  if (!status.is_ok()) return status;
  if (response_kind != protocol::MessageKind::kEvidenceAck) {
    return Status(ErrorCode::kInternalInvariant, "unexpected evidence response");
  }
  protocol::EvidenceAckPayload ack;
  return protocol::decode_evidence_ack(response_body, ack);
}

Status ClientSession::shutdown_coordinator() {
  std::vector<std::uint8_t> empty;
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  const Status status = request(protocol::MessageKind::kShutdown, empty, response_kind, response_body);
  if (!status.is_ok()) return status;
  connected_ = false;
  return Status::ok();
}

void ClientSession::close() {
  connection_.shutdown_socket();
  connected_ = false;
}

}  // namespace ctf::test
