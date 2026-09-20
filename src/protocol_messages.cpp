// Collective Traffic Fabric - decision payloads and per message codecs.
// Copyright 2026 Summon Software Labs.
#include <algorithm>

#include "ctf/protocol.hpp"

namespace ctf::protocol {

Status encode_decision_request(const DecisionRequest& value, Writer& writer) {
  writer.identity(value.session);
  writer.identity(value.instance.id);
  writer.identity(value.instance.generation);
  writer.identity(value.instance.attempt);
  writer.u64(value.instance.attempt_sequence.value());
  writer.identity(value.observed_topology_generation);
  writer.identity(value.observed_policy_generation);
  writer.identity(value.observed_epoch);
  writer.identity(value.observed_capacity_generation);
  writer.identity(value.observed_congestion_generation);
  writer.boolean(value.has_flow_group_plan);
  if (value.has_flow_group_plan) {
    Status status = encode_flow_group_plan(value.plan, writer);
    if (!status.is_ok()) return status;
  }
  writer.boolean(value.has_requested_class);
  writer.enumeration(value.requested_class);
  writer.u64(value.requested_transfer_bytes);
  return Status::ok();
}

Status decode_decision_request(Reader& reader, DecisionRequest& out) {
  DecisionRequest request;
  Status status = reader.identity(request.session);
  if (!status.is_ok()) return status;
  status = reader.identity(request.instance.id);
  if (!status.is_ok()) return status;
  status = reader.identity(request.instance.generation);
  if (!status.is_ok()) return status;
  status = reader.identity(request.instance.attempt);
  if (!status.is_ok()) return status;
  std::uint64_t attempt_sequence = 0;
  status = reader.u64(attempt_sequence);
  if (!status.is_ok()) return status;
  request.instance.attempt_sequence = Sequence(attempt_sequence);
  status = reader.identity(request.observed_topology_generation);
  if (!status.is_ok()) return status;
  status = reader.identity(request.observed_policy_generation);
  if (!status.is_ok()) return status;
  status = reader.identity(request.observed_epoch);
  if (!status.is_ok()) return status;
  status = reader.identity(request.observed_capacity_generation);
  if (!status.is_ok()) return status;
  status = reader.identity(request.observed_congestion_generation);
  if (!status.is_ok()) return status;
  status = reader.boolean(request.has_flow_group_plan);
  if (!status.is_ok()) return status;
  if (request.has_flow_group_plan) {
    status = decode_flow_group_plan(reader, request.plan);
    if (!status.is_ok()) return status;
  }
  status = reader.boolean(request.has_requested_class);
  if (!status.is_ok()) return status;
  std::uint8_t raw = 0;
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(TrafficClass::kProbe)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "requested traffic class is not defined by this version");
  }
  request.requested_class = static_cast<TrafficClass>(raw);
  status = reader.u64(request.requested_transfer_bytes);
  if (!status.is_ok()) return status;
  out = std::move(request);
  return Status::ok();
}

Status encode_decision_record(const DecisionRecord& value, Writer& writer) {
  writer.identity(value.instance.id);
  writer.identity(value.instance.generation);
  writer.identity(value.instance.attempt);
  writer.u64(value.instance.attempt_sequence.value());
  writer.enumeration(value.collective_class);
  writer.enumeration(value.algorithm_hint);
  writer.enumeration(value.outcome);
  writer.enumeration(value.axis);
  writer.u32(static_cast<std::uint32_t>(value.code));
  writer.identity(value.flow_group);
  writer.boolean(value.flow_group_constructed);
  Status status = encode_flow_group(value.group, writer);
  if (!status.is_ok()) return status;
  writer.u32(static_cast<std::uint32_t>(value.participants.size()));
  for (const ParticipantId& participant : value.participants) writer.identity(participant);
  writer.enumeration(value.traffic_class);
  writer.u64(value.priority);
  writer.enumeration(value.isolation);
  writer.enumeration(value.pacing.mode);
  writer.identity(value.flow_group);
  writer.identity(value.instance.id);
  writer.identity(value.instance.generation);
  writer.identity(value.instance.attempt);
  writer.u64(value.pacing.admitted_bandwidth_bps);
  writer.u64(value.pacing.ceiling_bandwidth_bps);
  writer.u64(value.pacing.burst_bytes);
  writer.u64(value.pacing.utilization_bps);
  writer.u64(value.pacing.release_after_monotonic_ms);
  writer.enumeration(value.sync.kind);
  writer.enumeration(value.sync.basis);
  writer.boolean(value.sync.latency_critical);
  writer.boolean(value.sync.all_members_must_arrive);
  writer.boolean(value.sync.completion_ordering_matters);
  writer.u32(value.sync.barrier_member_count);
  writer.u64(value.sync.maximum_skew_micros);
  writer.identity(value.collective_generation);
  writer.identity(value.topology_generation);
  writer.identity(value.policy_generation);
  writer.identity(value.capacity_generation);
  writer.identity(value.congestion_generation);
  writer.identity(value.epoch);
  writer.boolean(value.congestion_evidence_used);
  writer.boolean(value.capacity_evidence_used);
  writer.boolean(value.all_members_live);
  writer.u64(value.phase_index.value());
  writer.u64(value.step_index.value());
  status = encode_reason_chain(value.reasons, writer);
  if (!status.is_ok()) return status;
  writer.u64(value.decided_monotonic_ms);
  writer.u64(value.decision_sequence.value());
  return Status::ok();
}

Status decode_decision_record(Reader& reader, DecisionRecord& out) {
  DecisionRecord record;
  Status status = reader.identity(record.instance.id);
  if (!status.is_ok()) return status;
  status = reader.identity(record.instance.generation);
  if (!status.is_ok()) return status;
  status = reader.identity(record.instance.attempt);
  if (!status.is_ok()) return status;
  std::uint64_t attempt_sequence = 0;
  status = reader.u64(attempt_sequence);
  if (!status.is_ok()) return status;
  record.instance.attempt_sequence = Sequence(attempt_sequence);

  std::uint8_t raw = 0;
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(CollectiveClass::kUnknownVendor)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "collective class is not defined by this version");
  }
  record.collective_class = static_cast<CollectiveClass>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(AlgorithmHint::kVendorSpecific)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "algorithm hint is not defined by this version");
  }
  record.algorithm_hint = static_cast<AlgorithmHint>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(Outcome::kUnknownSemantics)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "outcome is not defined by this version");
  }
  record.outcome = static_cast<Outcome>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(AuthorityAxis::kPlan)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "authority axis is not defined by this version");
  }
  record.axis = static_cast<AuthorityAxis>(raw);
  std::uint32_t code = 0;
  status = reader.u32(code);
  if (!status.is_ok()) return status;
  if (code > static_cast<std::uint32_t>(ErrorCode::kInternalUnsupported)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "error code is not defined by this version");
  }
  record.code = static_cast<ErrorCode>(code);
  status = reader.identity(record.flow_group);
  if (!status.is_ok()) return status;
  status = reader.boolean(record.flow_group_constructed);
  if (!status.is_ok()) return status;
  status = decode_flow_group(reader, record.group);
  if (!status.is_ok()) return status;
  std::uint32_t participants = 0;
  status = reader.count(participants, limits::kParticipantsMaxPerCollective);
  if (!status.is_ok()) return status;
  record.participants.resize(participants);
  for (std::uint32_t index = 0; index < participants; ++index) {
    status = reader.identity(record.participants[index]);
    if (!status.is_ok()) return status;
  }
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(TrafficClass::kProbe)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "traffic class is not defined by this version");
  }
  record.traffic_class = static_cast<TrafficClass>(raw);
  status = reader.u64(record.priority);
  if (!status.is_ok()) return status;
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(IsolationMode::kDedicatedPath)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "isolation mode is not defined by this version");
  }
  record.isolation = static_cast<IsolationMode>(raw);

  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(PacingMode::kShaped)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "pacing mode is not defined by this version");
  }
  record.pacing.mode = static_cast<PacingMode>(raw);
  status = reader.identity(record.pacing.flow_group);
  if (!status.is_ok()) return status;
  status = reader.identity(record.pacing.instance.id);
  if (!status.is_ok()) return status;
  status = reader.identity(record.pacing.instance.generation);
  if (!status.is_ok()) return status;
  status = reader.identity(record.pacing.instance.attempt);
  if (!status.is_ok()) return status;
  status = reader.u64(record.pacing.admitted_bandwidth_bps);
  if (!status.is_ok()) return status;
  status = reader.u64(record.pacing.ceiling_bandwidth_bps);
  if (!status.is_ok()) return status;
  status = reader.u64(record.pacing.burst_bytes);
  if (!status.is_ok()) return status;
  status = reader.u64(record.pacing.utilization_bps);
  if (!status.is_ok()) return status;
  status = reader.u64(record.pacing.release_after_monotonic_ms);
  if (!status.is_ok()) return status;

  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(SyncKind::kStreamOrder)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "sync kind is not defined by this version");
  }
  record.sync.kind = static_cast<SyncKind>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(SyncBasis::kDerivedFromPolicy)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "sync basis is not defined by this version");
  }
  record.sync.basis = static_cast<SyncBasis>(raw);
  status = reader.boolean(record.sync.latency_critical);
  if (!status.is_ok()) return status;
  status = reader.boolean(record.sync.all_members_must_arrive);
  if (!status.is_ok()) return status;
  status = reader.boolean(record.sync.completion_ordering_matters);
  if (!status.is_ok()) return status;
  status = reader.u32(record.sync.barrier_member_count);
  if (!status.is_ok()) return status;
  status = reader.u64(record.sync.maximum_skew_micros);
  if (!status.is_ok()) return status;

  status = reader.identity(record.collective_generation);
  if (!status.is_ok()) return status;
  status = reader.identity(record.topology_generation);
  if (!status.is_ok()) return status;
  status = reader.identity(record.policy_generation);
  if (!status.is_ok()) return status;
  status = reader.identity(record.capacity_generation);
  if (!status.is_ok()) return status;
  status = reader.identity(record.congestion_generation);
  if (!status.is_ok()) return status;
  status = reader.identity(record.epoch);
  if (!status.is_ok()) return status;
  status = reader.boolean(record.congestion_evidence_used);
  if (!status.is_ok()) return status;
  status = reader.boolean(record.capacity_evidence_used);
  if (!status.is_ok()) return status;
  status = reader.boolean(record.all_members_live);
  if (!status.is_ok()) return status;
  std::uint64_t phase = 0;
  std::uint64_t step = 0;
  status = reader.u64(phase);
  if (!status.is_ok()) return status;
  status = reader.u64(step);
  if (!status.is_ok()) return status;
  record.phase_index = Sequence(phase);
  record.step_index = Sequence(step);
  status = decode_reason_chain(reader, record.reasons);
  if (!status.is_ok()) return status;
  status = reader.u64(record.decided_monotonic_ms);
  if (!status.is_ok()) return status;
  std::uint64_t sequence = 0;
  status = reader.u64(sequence);
  if (!status.is_ok()) return status;
  record.decision_sequence = Sequence(sequence);
  out = std::move(record);
  return Status::ok();
}

namespace {

// Every message codec follows the same shape: build the structure, encode it,
// and require that decoding consumed the payload exactly.
template <typename Payload, typename Decode>
Status decode_one(const std::vector<std::uint8_t>& in, Payload& out, Decode decode) {
  Reader reader(in.data(), in.size());
  Status status = decode(reader, out);
  if (!status.is_ok()) return status;
  return reader.require_end();
}

}  // namespace

Status encode_hello(const HelloPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.string(value.client_label);
  writer.string(value.client_version);
  writer.u64(value.client_boot_monotonic_ms);
  out = writer.data();
  return Status::ok();
}

Status decode_hello(const std::vector<std::uint8_t>& in, HelloPayload& out) {
  HelloPayload value;
  Status status = decode_one(in, value, [](Reader& reader, HelloPayload& target) {
    Status status = reader.string(target.client_label, limits::kLabelBytesMax);
    if (!status.is_ok()) return status;
    status = reader.string(target.client_version, limits::kLabelBytesMax);
    if (!status.is_ok()) return status;
    return reader.u64(target.client_boot_monotonic_ms);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_hello_ack(const HelloAckPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.session);
  writer.identity(value.incarnation);
  writer.identity(value.epoch);
  writer.identity(value.policy_generation);
  writer.identity(value.topology_generation);
  writer.identity(value.capacity_generation);
  writer.identity(value.congestion_generation);
  writer.u32(value.max_frame_payload_bytes);
  writer.string(value.coordinator_label);
  out = writer.data();
  return Status::ok();
}

Status decode_hello_ack(const std::vector<std::uint8_t>& in, HelloAckPayload& out) {
  HelloAckPayload value;
  Status status = decode_one(in, value, [](Reader& reader, HelloAckPayload& target) {
    Status status = reader.identity(target.session);
    if (!status.is_ok()) return status;
    status = reader.identity(target.incarnation);
    if (!status.is_ok()) return status;
    status = reader.identity(target.epoch);
    if (!status.is_ok()) return status;
    status = reader.identity(target.policy_generation);
    if (!status.is_ok()) return status;
    status = reader.identity(target.topology_generation);
    if (!status.is_ok()) return status;
    status = reader.identity(target.capacity_generation);
    if (!status.is_ok()) return status;
    status = reader.identity(target.congestion_generation);
    if (!status.is_ok()) return status;
    status = reader.u32(target.max_frame_payload_bytes);
    if (!status.is_ok()) return status;
    if (target.max_frame_payload_bytes > limits::kFramePayloadMaxBytes) {
      return Status(ErrorCode::kDecodeLengthOutOfRange, "peer advertised an unsupported frame size");
    }
    return reader.string(target.coordinator_label, limits::kLabelBytesMax);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_heartbeat(const HeartbeatPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.participant);
  writer.identity(value.incarnation);
  writer.enumeration(value.state);
  writer.identity(value.node);
  writer.u64(value.client_clock_ms);
  out = writer.data();
  return Status::ok();
}

Status decode_heartbeat(const std::vector<std::uint8_t>& in, HeartbeatPayload& out) {
  HeartbeatPayload value;
  Status status = decode_one(in, value, [](Reader& reader, HeartbeatPayload& target) {
    Status status = reader.identity(target.participant);
    if (!status.is_ok()) return status;
    status = reader.identity(target.incarnation);
    if (!status.is_ok()) return status;
    std::uint8_t raw = 0;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(ParticipantState::kRetired)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "participant state is not defined by this version");
    }
    target.state = static_cast<ParticipantState>(raw);
    status = reader.identity(target.node);
    if (!status.is_ok()) return status;
    return reader.u64(target.client_clock_ms);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_heartbeat_ack(const HeartbeatAckPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.participant);
  writer.identity(value.incarnation);
  writer.enumeration(value.state);
  writer.u64(value.expires_at_monotonic_ms);
  writer.boolean(value.accepted);
  const Status status = encode_reason_chain(value.reasons, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_heartbeat_ack(const std::vector<std::uint8_t>& in, HeartbeatAckPayload& out) {
  HeartbeatAckPayload value;
  const Status status = decode_one(in, value, [](Reader& reader, HeartbeatAckPayload& target) {
    Status status = reader.identity(target.participant);
    if (!status.is_ok()) return status;
    status = reader.identity(target.incarnation);
    if (!status.is_ok()) return status;
    std::uint8_t raw = 0;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(ParticipantState::kRetired)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "participant state is not defined by this version");
    }
    target.state = static_cast<ParticipantState>(raw);
    status = reader.u64(target.expires_at_monotonic_ms);
    if (!status.is_ok()) return status;
    status = reader.boolean(target.accepted);
    if (!status.is_ok()) return status;
    return decode_reason_chain(reader, target.reasons);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_register_collective(const RegisterCollectivePayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  Status status = encode_definition(value.definition, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_register_collective(const std::vector<std::uint8_t>& in, RegisterCollectivePayload& out) {
  RegisterCollectivePayload value;
  Status status = decode_one(in, value, [](Reader& reader, RegisterCollectivePayload& target) {
    return decode_definition(reader, target.definition);
  });
  if (!status.is_ok()) return status;
  CollectiveDefinition definition = value.definition;
  canonicalize_participants(definition.participants);
  std::sort(definition.phases.begin(), definition.phases.end(),
            [](const CollectivePhase& a, const CollectivePhase& b) { return a.phase_index < b.phase_index; });
  std::sort(definition.steps.begin(), definition.steps.end(), [](const CollectiveStep& a, const CollectiveStep& b) {
    if (a.phase_index != b.phase_index) return a.phase_index < b.phase_index;
    return a.step_index < b.step_index;
  });
  // Duplicate participants collapse during canonicalisation; duplicates in the
  // phase or step list are ambiguous and are refused instead of merged.
  if (std::adjacent_find(definition.phases.begin(), definition.phases.end(),
                         [](const CollectivePhase& a, const CollectivePhase& b) {
                           return a.phase_index == b.phase_index;
                         }) != definition.phases.end()) {
    return Status(ErrorCode::kDecodeDuplicateEntry, "collective definition states the same phase twice");
  }
  if (std::adjacent_find(definition.steps.begin(), definition.steps.end(),
                         [](const CollectiveStep& a, const CollectiveStep& b) {
                           return a.phase_index == b.phase_index && a.step_index == b.step_index;
                         }) != definition.steps.end()) {
    return Status(ErrorCode::kDecodeDuplicateEntry, "collective definition states the same step twice");
  }
  value.definition = std::move(definition);
  out = std::move(value);
  return Status::ok();
}

Status encode_register_ack(const RegisterAckPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.id);
  writer.identity(value.generation);
  writer.u8(value.result);
  writer.u64(value.registration_sequence.value());
  Status status = encode_reason_chain(value.reasons, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_register_ack(const std::vector<std::uint8_t>& in, RegisterAckPayload& out) {
  RegisterAckPayload value;
  Status status = decode_one(in, value, [](Reader& reader, RegisterAckPayload& target) {
    Status status = reader.identity(target.id);
    if (!status.is_ok()) return status;
    status = reader.identity(target.generation);
    if (!status.is_ok()) return status;
    status = reader.u8(target.result);
    if (!status.is_ok()) return status;
    std::uint64_t sequence = 0;
    status = reader.u64(sequence);
    if (!status.is_ok()) return status;
    target.registration_sequence = Sequence(sequence);
    return decode_reason_chain(reader, target.reasons);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_begin_attempt(const BeginAttemptPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.id);
  writer.identity(value.generation);
  writer.identity(value.attempt);
  writer.u64(value.transfer_bytes);
  out = writer.data();
  return Status::ok();
}

Status decode_begin_attempt(const std::vector<std::uint8_t>& in, BeginAttemptPayload& out) {
  BeginAttemptPayload value;
  Status status = decode_one(in, value, [](Reader& reader, BeginAttemptPayload& target) {
    Status status = reader.identity(target.id);
    if (!status.is_ok()) return status;
    status = reader.identity(target.generation);
    if (!status.is_ok()) return status;
    status = reader.identity(target.attempt);
    if (!status.is_ok()) return status;
    return reader.u64(target.transfer_bytes);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_attempt_ack(const AttemptAckPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.id);
  writer.identity(value.attempt);
  writer.identity(value.previous_attempt);
  writer.enumeration(value.state);
  Status status = encode_reason_chain(value.reasons, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_attempt_ack(const std::vector<std::uint8_t>& in, AttemptAckPayload& out) {
  AttemptAckPayload value;
  Status status = decode_one(in, value, [](Reader& reader, AttemptAckPayload& target) {
    Status status = reader.identity(target.id);
    if (!status.is_ok()) return status;
    status = reader.identity(target.attempt);
    if (!status.is_ok()) return status;
    status = reader.identity(target.previous_attempt);
    if (!status.is_ok()) return status;
    std::uint8_t raw = 0;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(CollectiveState::kTimedOut)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "collective state is not defined by this version");
    }
    target.state = static_cast<CollectiveState>(raw);
    return decode_reason_chain(reader, target.reasons);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_plan_flow_group(const PlanFlowGroupPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  Status status = encode_decision_request(value.request, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_plan_flow_group(const std::vector<std::uint8_t>& in, PlanFlowGroupPayload& out) {
  PlanFlowGroupPayload value;
  Status status = decode_one(in, value, [](Reader& reader, PlanFlowGroupPayload& target) {
    return decode_decision_request(reader, target.request);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_decision(const DecisionPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  Status status = encode_decision_record(value.decision, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_decision(const std::vector<std::uint8_t>& in, DecisionPayload& out) {
  DecisionPayload value;
  Status status = decode_one(in, value, [](Reader& reader, DecisionPayload& target) {
    return decode_decision_record(reader, target.decision);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_lifecycle(const LifecyclePayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.id);
  writer.identity(value.generation);
  writer.identity(value.attempt);
  writer.string(value.reason);
  out = writer.data();
  return Status::ok();
}

Status decode_lifecycle(const std::vector<std::uint8_t>& in, LifecyclePayload& out) {
  LifecyclePayload value;
  Status status = decode_one(in, value, [](Reader& reader, LifecyclePayload& target) {
    Status status = reader.identity(target.id);
    if (!status.is_ok()) return status;
    status = reader.identity(target.generation);
    if (!status.is_ok()) return status;
    status = reader.identity(target.attempt);
    if (!status.is_ok()) return status;
    return reader.string(target.reason, limits::kReasonDetailBytesMax);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_lifecycle_ack(const LifecycleAckPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.id);
  writer.enumeration(value.state);
  Status status = encode_reason_chain(value.reasons, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_lifecycle_ack(const std::vector<std::uint8_t>& in, LifecycleAckPayload& out) {
  LifecycleAckPayload value;
  Status status = decode_one(in, value, [](Reader& reader, LifecycleAckPayload& target) {
    Status status = reader.identity(target.id);
    if (!status.is_ok()) return status;
    std::uint8_t raw = 0;
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(CollectiveState::kTimedOut)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "collective state is not defined by this version");
    }
    target.state = static_cast<CollectiveState>(raw);
    return decode_reason_chain(reader, target.reasons);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_evidence(const EvidencePayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.enumeration(value.kind);
  Status status = Status::ok();
  switch (value.kind) {
    case EvidenceKindTag::kCapacity:
      status = encode_capacity(value.capacity, writer);
      break;
    case EvidenceKindTag::kCongestion:
      status = encode_congestion(value.congestion, writer);
      break;
    case EvidenceKindTag::kTopology:
      status = encode_topology(value.topology, writer);
      break;
    case EvidenceKindTag::kPolicy:
      status = encode_traffic_policy(value.policy, writer);
      break;
    case EvidenceKindTag::kNone:
    default:
      return Status(ErrorCode::kDecodeInvalidTag, "evidence payload states no evidence kind");
  }
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_evidence(const std::vector<std::uint8_t>& in, EvidencePayload& out) {
  EvidencePayload value;
  Status status = decode_one(in, value, [](Reader& reader, EvidencePayload& target) {
    std::uint8_t raw = 0;
    Status status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(EvidenceKindTag::kPolicy)) {
      return Status(ErrorCode::kDecodeInvalidTag, "evidence kind is not defined by this version");
    }
    target.kind = static_cast<EvidenceKindTag>(raw);
    switch (target.kind) {
      case EvidenceKindTag::kCapacity:
        return decode_capacity(reader, target.capacity);
      case EvidenceKindTag::kCongestion:
        return decode_congestion(reader, target.congestion);
      case EvidenceKindTag::kTopology:
        return decode_topology(reader, target.topology);
      case EvidenceKindTag::kPolicy:
        return decode_traffic_policy(reader, target.policy);
      case EvidenceKindTag::kNone:
      default:
        return Status(ErrorCode::kDecodeInvalidTag, "evidence payload states no evidence kind");
    }
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_evidence_ack(const EvidenceAckPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.enumeration(value.kind);
  writer.u8(value.result);
  writer.identity(value.generation);
  Status status = encode_reason_chain(value.reasons, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_evidence_ack(const std::vector<std::uint8_t>& in, EvidenceAckPayload& out) {
  EvidenceAckPayload value;
  Status status = decode_one(in, value, [](Reader& reader, EvidenceAckPayload& target) {
    std::uint8_t raw = 0;
    Status status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(EvidenceKindTag::kPolicy)) {
      return Status(ErrorCode::kDecodeInvalidTag, "evidence kind is not defined by this version");
    }
    target.kind = static_cast<EvidenceKindTag>(raw);
    status = reader.u8(target.result);
    if (!status.is_ok()) return status;
    status = reader.identity(target.generation);
    if (!status.is_ok()) return status;
    return decode_reason_chain(reader, target.reasons);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_inspect_request(const InspectRequestPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.u8(value.subject);
  writer.identity(value.id);
  writer.u32(value.offset);
  writer.u32(value.limit);
  out = writer.data();
  return Status::ok();
}

Status decode_inspect_request(const std::vector<std::uint8_t>& in, InspectRequestPayload& out) {
  InspectRequestPayload value;
  Status status = decode_one(in, value, [](Reader& reader, InspectRequestPayload& target) {
    Status status = reader.u8(target.subject);
    if (!status.is_ok()) return status;
    status = reader.identity(target.id);
    if (!status.is_ok()) return status;
    status = reader.u32(target.offset);
    if (!status.is_ok()) return status;
    status = reader.u32(target.limit);
    if (!status.is_ok()) return status;
    if (target.limit > limits::kInspectionPageMax) {
      return Status(ErrorCode::kDecodeCountOutOfRange, "inspection page size exceeds the bound");
    }
    return Status::ok();
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_inspect_response(const InspectResponsePayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.u8(value.subject);
  writer.string(value.body);
  writer.boolean(value.truncated);
  out = writer.data();
  return Status::ok();
}

Status decode_inspect_response(const std::vector<std::uint8_t>& in, InspectResponsePayload& out) {
  InspectResponsePayload value;
  Status status = decode_one(in, value, [](Reader& reader, InspectResponsePayload& target) {
    Status status = reader.u8(target.subject);
    if (!status.is_ok()) return status;
    status = reader.string(target.body, limits::kFramePayloadMaxBytes);
    if (!status.is_ok()) return status;
    return reader.boolean(target.truncated);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_explain_request(const ExplainRequestPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.id);
  writer.identity(value.attempt);
  writer.u64(value.correlation);
  writer.boolean(value.has_correlation);
  out = writer.data();
  return Status::ok();
}

Status decode_explain_request(const std::vector<std::uint8_t>& in, ExplainRequestPayload& out) {
  ExplainRequestPayload value;
  Status status = decode_one(in, value, [](Reader& reader, ExplainRequestPayload& target) {
    Status status = reader.identity(target.id);
    if (!status.is_ok()) return status;
    status = reader.identity(target.attempt);
    if (!status.is_ok()) return status;
    status = reader.u64(target.correlation);
    if (!status.is_ok()) return status;
    return reader.boolean(target.has_correlation);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_explain_response(const ExplainResponsePayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.boolean(value.found);
  Status status = encode_decision_record(value.decision, writer);
  if (!status.is_ok()) return status;
  writer.string(value.explanation);
  out = writer.data();
  return Status::ok();
}

Status decode_explain_response(const std::vector<std::uint8_t>& in, ExplainResponsePayload& out) {
  ExplainResponsePayload value;
  Status status = decode_one(in, value, [](Reader& reader, ExplainResponsePayload& target) {
    Status status = reader.boolean(target.found);
    if (!status.is_ok()) return status;
    status = decode_decision_record(reader, target.decision);
    if (!status.is_ok()) return status;
    return reader.string(target.explanation, limits::kFramePayloadMaxBytes);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_install_policy(const InstallPolicyPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  const Status status = encode_traffic_policy(value.policy, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_install_policy(const std::vector<std::uint8_t>& in, InstallPolicyPayload& out) {
  InstallPolicyPayload value;
  const Status status = decode_one(in, value, [](Reader& reader, InstallPolicyPayload& target) {
    return decode_traffic_policy(reader, target.policy);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_install_policy_ack(const InstallPolicyAckPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.identity(value.generation);
  writer.u8(value.result);
  const Status status = encode_reason_chain(value.reasons, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_install_policy_ack(const std::vector<std::uint8_t>& in, InstallPolicyAckPayload& out) {
  InstallPolicyAckPayload value;
  const Status status = decode_one(in, value, [](Reader& reader, InstallPolicyAckPayload& target) {
    Status status = reader.identity(target.generation);
    if (!status.is_ok()) return status;
    status = reader.u8(target.result);
    if (!status.is_ok()) return status;
    return decode_reason_chain(reader, target.reasons);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

Status encode_error(const ErrorPayload& value, std::vector<std::uint8_t>& out) {
  Writer writer;
  writer.u32(static_cast<std::uint32_t>(value.code));
  writer.string(value.message);
  Status status = encode_reason_chain(value.reasons, writer);
  if (!status.is_ok()) return status;
  out = writer.data();
  return Status::ok();
}

Status decode_error(const std::vector<std::uint8_t>& in, ErrorPayload& out) {
  ErrorPayload value;
  Status status = decode_one(in, value, [](Reader& reader, ErrorPayload& target) {
    std::uint32_t code = 0;
    Status status = reader.u32(code);
    if (!status.is_ok()) return status;
    if (code > static_cast<std::uint32_t>(ErrorCode::kInternalUnsupported)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "error code is not defined by this version");
    }
    target.code = static_cast<ErrorCode>(code);
    status = reader.string(target.message, limits::kReasonDetailBytesMax);
    if (!status.is_ok()) return status;
    return decode_reason_chain(reader, target.reasons);
  });
  if (!status.is_ok()) return status;
  out = std::move(value);
  return Status::ok();
}

}  // namespace ctf::protocol
