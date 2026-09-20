// Collective Traffic Fabric - framed wire protocol and canonical payload codec.
// Copyright 2026 Summon Software Labs.
#include "ctf/protocol.hpp"

#include <algorithm>
#include <cstring>

#include "ctf/persistence.hpp"

namespace ctf::protocol {
namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint32_t read_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (int index = 3; index >= 0; --index) {
    value = (value << 8) | data[index];
  }
  return value;
}

std::uint64_t read_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) | data[index];
  }
  return value;
}

}  // namespace

std::string_view to_string(MessageKind value) noexcept {
  switch (value) {
    case MessageKind::kInvalid: return "invalid";
    case MessageKind::kHello: return "hello";
    case MessageKind::kHelloAck: return "hello_ack";
    case MessageKind::kHeartbeat: return "heartbeat";
    case MessageKind::kHeartbeatAck: return "heartbeat_ack";
    case MessageKind::kRegisterCollective: return "register_collective";
    case MessageKind::kRegisterAck: return "register_ack";
    case MessageKind::kBeginAttempt: return "begin_attempt";
    case MessageKind::kAttemptAck: return "attempt_ack";
    case MessageKind::kPlanFlowGroup: return "plan_flow_group";
    case MessageKind::kFlowGroupDecision: return "flow_group_decision";
    case MessageKind::kCancelCollective: return "cancel_collective";
    case MessageKind::kRetireCollective: return "retire_collective";
    case MessageKind::kLifecycleAck: return "lifecycle_ack";
    case MessageKind::kIngestEvidence: return "ingest_evidence";
    case MessageKind::kEvidenceAck: return "evidence_ack";
    case MessageKind::kInspectRequest: return "inspect_request";
    case MessageKind::kInspectResponse: return "inspect_response";
    case MessageKind::kExplainRequest: return "explain_request";
    case MessageKind::kExplainResponse: return "explain_response";
    case MessageKind::kErrorResponse: return "error_response";
    case MessageKind::kInstallPolicy: return "install_policy";
    case MessageKind::kInstallPolicyAck: return "install_policy_ack";
    case MessageKind::kShutdown: return "shutdown";
    case MessageKind::kShutdownAck: return "shutdown_ack";
    case MessageKind::kKindCount: return "kind_count";
  }
  return "invalid";
}

bool message_kind_is_known(std::uint16_t raw) noexcept {
  return raw > 0 && raw < static_cast<std::uint16_t>(MessageKind::kKindCount);
}

// ---- Writer ----------------------------------------------------------------
void Writer::u8(std::uint8_t value) { data_.push_back(value); }

void Writer::u16(std::uint16_t value) { put_u16(data_, value); }

void Writer::u32(std::uint32_t value) { put_u32(data_, value); }

void Writer::u64(std::uint64_t value) { put_u64(data_, value); }

void Writer::i64(std::int64_t value) { put_u64(data_, static_cast<std::uint64_t>(value)); }

void Writer::boolean(bool value) { u8(value ? 1u : 0u); }

void Writer::identity(Identity value) {
  u64(value.high());
  u64(value.low());
}

void Writer::string(std::string_view value) {
  const std::uint32_t length = static_cast<std::uint32_t>(value.size());
  u32(length);
  data_.insert(data_.end(), value.begin(), value.end());
}

void Writer::raw(const std::vector<std::uint8_t>& value) {
  u32(static_cast<std::uint32_t>(value.size()));
  data_.insert(data_.end(), value.begin(), value.end());
}

// ---- Reader ----------------------------------------------------------------
Status Reader::take(std::size_t count) {
  if (count > size_ - offset_) {
    return Status(ErrorCode::kDecodeTruncated, "payload ended before the field was complete");
  }
  return Status::ok();
}

Status Reader::u8(std::uint8_t& out) {
  Status status = take(1);
  if (!status.is_ok()) return status;
  out = data_[offset_];
  offset_ += 1;
  return Status::ok();
}

Status Reader::u16(std::uint16_t& out) {
  Status status = take(2);
  if (!status.is_ok()) return status;
  out = read_u16(data_ + offset_);
  offset_ += 2;
  return Status::ok();
}

Status Reader::u32(std::uint32_t& out) {
  Status status = take(4);
  if (!status.is_ok()) return status;
  out = read_u32(data_ + offset_);
  offset_ += 4;
  return Status::ok();
}

Status Reader::u64(std::uint64_t& out) {
  Status status = take(8);
  if (!status.is_ok()) return status;
  out = read_u64(data_ + offset_);
  offset_ += 8;
  return Status::ok();
}

Status Reader::i64(std::int64_t& out) {
  std::uint64_t raw = 0;
  Status status = u64(raw);
  if (!status.is_ok()) return status;
  out = static_cast<std::int64_t>(raw);
  return Status::ok();
}

Status Reader::boolean(bool& out) {
  std::uint8_t raw = 0;
  Status status = u8(raw);
  if (!status.is_ok()) return status;
  if (raw > 1) {
    return Status(ErrorCode::kDecodeInvalidBoolean, "boolean field carried a value other than 0 or 1");
  }
  out = raw == 1;
  return Status::ok();
}

Status Reader::identity(Identity& out) {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  Status status = u64(hi);
  if (!status.is_ok()) return status;
  status = u64(lo);
  if (!status.is_ok()) return status;
  out = Identity(hi, lo);
  return Status::ok();
}

Status Reader::string(std::string& out, std::uint32_t max_bytes) {
  std::uint32_t length = 0;
  Status status = u32(length);
  if (!status.is_ok()) return status;
  if (length > max_bytes) {
    return Status(ErrorCode::kDecodeLengthOutOfRange, "string field exceeds its bound");
  }
  status = take(length);
  if (!status.is_ok()) return status;
  out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
  offset_ += length;
  return Status::ok();
}

Status Reader::raw(std::vector<std::uint8_t>& out, std::uint32_t max_bytes) {
  std::uint32_t length = 0;
  Status status = u32(length);
  if (!status.is_ok()) return status;
  if (length > max_bytes) {
    return Status(ErrorCode::kDecodeLengthOutOfRange, "byte field exceeds its bound");
  }
  status = take(length);
  if (!status.is_ok()) return status;
  out.assign(data_ + offset_, data_ + offset_ + length);
  offset_ += length;
  return Status::ok();
}

Status Reader::count(std::uint32_t& out, std::uint32_t max) {
  Status status = u32(out);
  if (!status.is_ok()) return status;
  if (out > max) {
    return Status(ErrorCode::kDecodeCountOutOfRange, "collection count exceeds its bound");
  }
  return Status::ok();
}

Status Reader::require_end() const {
  if (offset_ != size_) {
    return Status(ErrorCode::kDecodeTrailingBytes, "payload carried trailing bytes");
  }
  return Status::ok();
}

// ---- frame -----------------------------------------------------------------
Status encode_envelope(const Envelope& envelope, std::vector<std::uint8_t>& out) {
  out.clear();
  out.reserve(kEnvelopeBytes);
  put_u64(out, envelope.incarnation.high());
  put_u64(out, envelope.incarnation.low());
  put_u64(out, envelope.epoch.high());
  put_u64(out, envelope.epoch.low());
  put_u64(out, envelope.frame_sequence.value());
  // The correlation value is caller supplied and opaque to the coordinator: it is
  // echoed on the response and recorded in the bounded correlation index so a
  // request can be explained later.  It carries no authority, and a missing or
  // zero value simply means "this peer did not ask to be correlated".
  put_u64(out, envelope.correlation);
  return Status::ok();
}

Status decode_envelope(const std::uint8_t* data, std::size_t size, Envelope& out) {
  if (size < kEnvelopeBytes) {
    return Status(ErrorCode::kDecodeTruncated, "frame envelope is incomplete");
  }
  out.incarnation = Identity(read_u64(data), read_u64(data + 8));
  out.epoch = Identity(read_u64(data + 16), read_u64(data + 24));
  out.frame_sequence = Sequence(read_u64(data + 32));
  out.correlation = read_u64(data + 40);
  return Status::ok();
}

Status encode_frame(const Frame& frame, std::vector<std::uint8_t>& out) {
  if (!message_kind_is_known(frame.kind)) {
    return Status(ErrorCode::kFrameKindUnknown, "cannot encode an unknown message kind");
  }
  if ((frame.flags & ~kFlagKnownMask) != 0) {
    return Status(ErrorCode::kFrameFlagsInvalid, "frame flags carry bits this version does not define");
  }
  if (frame.payload.size() > limits::kFramePayloadMaxBytes) {
    return Status(ErrorCode::kFrameLengthOutOfRange, "frame payload exceeds the size bound");
  }
  if (frame.payload.size() > 0xFFFFFFFFull - kEnvelopeBytes) {
    return Status(ErrorCode::kFrameLengthArithmeticOverflow, "frame length would overflow the header field");
  }
  std::vector<std::uint8_t> envelope;
  Status status = encode_envelope(frame.envelope, envelope);
  if (!status.is_ok()) return status;

  std::vector<std::uint8_t> body;
  body.reserve(envelope.size() + frame.payload.size());
  body.insert(body.end(), envelope.begin(), envelope.end());
  body.insert(body.end(), frame.payload.begin(), frame.payload.end());

  std::vector<std::uint8_t> header;
  header.reserve(kHeaderBytes);
  put_u32(header, kFrameMagic);
  put_u16(header, static_cast<std::uint16_t>(kHeaderBytes));
  put_u16(header, frame.kind);
  put_u16(header, frame.flags);
  put_u16(header, static_cast<std::uint16_t>(limits::kProtocolMajor));
  put_u16(header, static_cast<std::uint16_t>(limits::kProtocolMinor));
  put_u32(header, static_cast<std::uint32_t>(body.size()));
  put_u32(header, crc32(body.data(), body.size()));
  put_u32(header, crc32(header.data(), header.size()));
  put_u16(header, 0);
  put_u64(header, frame.envelope.session.high());
  put_u64(header, frame.envelope.session.low());
  if (header.size() != kHeaderBytes) {
    return Status(ErrorCode::kInternalInvariant, "frame header has an unexpected size");
  }
  out.clear();
  out.reserve(header.size() + body.size());
  out.insert(out.end(), header.begin(), header.end());
  out.insert(out.end(), body.begin(), body.end());
  return Status::ok();
}

Status decode_frame(const std::uint8_t* data, std::size_t size, Frame& out, std::size_t& consumed) {
  consumed = 0;
  if (size == 0) {
    return Status(ErrorCode::kTransportClosed, "no bytes are available");
  }
  if (size < kHeaderBytes) {
    return Status(ErrorCode::kFrameTruncatedHeader, "frame header is incomplete");
  }
  if (read_u32(data) != kFrameMagic) {
    return Status(ErrorCode::kFrameMagicMismatch, "frame magic does not match this protocol");
  }
  if (read_u16(data + 4) != static_cast<std::uint16_t>(kHeaderBytes)) {
    return Status(ErrorCode::kFrameLengthOutOfRange, "frame header size field is not the defined size");
  }
  const std::uint16_t kind = read_u16(data + 6);
  const std::uint16_t flags = read_u16(data + 8);
  const std::uint16_t major = read_u16(data + 10);
  const std::uint16_t minor = read_u16(data + 12);
  const std::uint32_t body_bytes = read_u32(data + 14);
  const std::uint32_t body_crc = read_u32(data + 18);
  const std::uint32_t header_crc = read_u32(data + 22);
  const std::uint16_t reserved = read_u16(data + 26);

  if (crc32(data, 22) != header_crc) {
    return Status(ErrorCode::kFrameIntegrityMismatch, "frame header CRC32 does not match");
  }
  if (major != limits::kProtocolMajor) {
    return Status(ErrorCode::kFrameVersionUnsupported, "frame protocol major version is not supported");
  }
  if (minor > limits::kProtocolMinor) {
    return Status(ErrorCode::kFrameVersionUnsupported, "frame protocol minor version is newer than this build");
  }
  if (reserved != 0) {
    return Status(ErrorCode::kFrameFlagsInvalid, "frame reserved field must be zero");
  }
  if ((flags & ~kFlagKnownMask) != 0) {
    return Status(ErrorCode::kFrameFlagsInvalid, "frame flags carry bits this version does not define");
  }
  if (!message_kind_is_known(kind)) {
    return Status(ErrorCode::kFrameKindUnknown, "frame kind is not defined by this version");
  }
  if (body_bytes < kEnvelopeBytes) {
    return Status(ErrorCode::kFrameLengthOutOfRange, "frame body is shorter than the envelope it must carry");
  }
  if (body_bytes > limits::kFramePayloadMaxBytes + kEnvelopeBytes) {
    return Status(ErrorCode::kFrameLengthOutOfRange, "frame body exceeds the size bound");
  }
  if (size < static_cast<std::size_t>(kHeaderBytes) + body_bytes) {
    return Status(ErrorCode::kFrameTruncatedPayload, "frame body is incomplete");
  }
  const std::uint8_t* body = data + kHeaderBytes;
  if (crc32(body, body_bytes) != body_crc) {
    return Status(ErrorCode::kFrameIntegrityMismatch, "frame body CRC32 does not match");
  }

  Frame frame;
  frame.kind = kind;
  frame.flags = flags;
  frame.envelope.session = Identity(read_u64(data + 28), read_u64(data + 36));
  Status status = decode_envelope(body, body_bytes, frame.envelope);
  if (!status.is_ok()) return status;
  frame.payload.assign(body + kEnvelopeBytes, body + body_bytes);
  out = std::move(frame);
  consumed = static_cast<std::size_t>(kHeaderBytes) + body_bytes;
  return Status::ok();
}

// ---- shared nested codecs --------------------------------------------------
Status encode_reason_chain(const ReasonChain& value, Writer& writer) {
  if (value.reasons().size() > limits::kReasonChainMax) {
    return Status(ErrorCode::kValidationReasonChainTooLong, "reason chain exceeds its bound");
  }
  writer.u32(static_cast<std::uint32_t>(value.reasons().size()));
  for (const Reason& reason : value.reasons()) {
    writer.string(reason.code);
    writer.string(reason.detail);
    writer.identity(reason.subject);
  }
  return Status::ok();
}

Status decode_reason_chain(Reader& reader, ReasonChain& out) {
  std::uint32_t count = 0;
  Status status = reader.count(count, limits::kReasonChainMax);
  if (!status.is_ok()) return status;
  ReasonChain chain;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string code;
    std::string detail;
    Identity subject;
    status = reader.string(code, 128);
    if (!status.is_ok()) return status;
    status = reader.string(detail, limits::kReasonDetailBytesMax);
    if (!status.is_ok()) return status;
    status = reader.identity(subject);
    if (!status.is_ok()) return status;
    if (!chain.add(code, detail, subject)) {
      return Status(ErrorCode::kValidationReasonChainTooLong, "reason chain exceeded its bound while decoding");
    }
  }
  out = chain;
  return Status::ok();
}

Status encode_definition(const CollectiveDefinition& value, Writer& writer) {
  writer.identity(value.id);
  writer.identity(value.generation);
  writer.enumeration(value.collective_class);
  writer.enumeration(value.algorithm_hint);
  writer.enumeration(value.scope);
  writer.string(value.label);
  writer.identity(value.origin_node);
  writer.identity(value.fabric);
  writer.u32(static_cast<std::uint32_t>(value.participants.size()));
  for (const ParticipantId& participant : value.participants) writer.identity(participant);
  writer.u32(static_cast<std::uint32_t>(value.phases.size()));
  for (const CollectivePhase& phase : value.phases) {
    writer.u64(phase.phase_index.value());
    writer.string(phase.label);
    writer.boolean(phase.synchronization_point);
    writer.u32(phase.step_count);
  }
  writer.u32(static_cast<std::uint32_t>(value.steps.size()));
  for (const CollectiveStep& step : value.steps) {
    writer.u64(step.phase_index.value());
    writer.u64(step.step_index.value());
    writer.enumeration(step.hint);
    writer.boolean(step.synchronization_point);
  }
  writer.u32(static_cast<std::uint32_t>(value.metadata.entries().size()));
  for (const auto& entry : value.metadata.entries()) {
    writer.string(entry.first);
    writer.string(entry.second);
  }
  writer.boolean(value.declares_barrier_semantics);
  writer.u64(value.logical_bytes);
  writer.u64(value.registration_sequence.value());
  return Status::ok();
}

Status decode_definition(Reader& reader, CollectiveDefinition& out) {
  CollectiveDefinition definition;
  Status status = reader.identity(definition.id);
  if (!status.is_ok()) return status;
  status = reader.identity(definition.generation);
  if (!status.is_ok()) return status;
  std::uint8_t raw = 0;
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(CollectiveClass::kUnknownVendor)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "collective class is not defined by this version");
  }
  definition.collective_class = static_cast<CollectiveClass>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(AlgorithmHint::kVendorSpecific)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "algorithm hint is not defined by this version");
  }
  definition.algorithm_hint = static_cast<AlgorithmHint>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(CollectiveScope::kHybrid)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "collective scope is not defined by this version");
  }
  definition.scope = static_cast<CollectiveScope>(raw);
  status = reader.string(definition.label, limits::kLabelBytesMax);
  if (!status.is_ok()) return status;
  status = reader.identity(definition.origin_node);
  if (!status.is_ok()) return status;
  status = reader.identity(definition.fabric);
  if (!status.is_ok()) return status;

  std::uint32_t participants = 0;
  status = reader.count(participants, limits::kParticipantsMaxPerCollective);
  if (!status.is_ok()) return status;
  if (participants == 0) {
    return Status(ErrorCode::kDecodeEmptyRequiredField, "a collective definition must name its participants");
  }
  definition.participants.resize(participants);
  for (std::uint32_t index = 0; index < participants; ++index) {
    status = reader.identity(definition.participants[index]);
    if (!status.is_ok()) return status;
  }
  std::uint32_t phases = 0;
  status = reader.count(phases, limits::kPhasesMaxPerCollective);
  if (!status.is_ok()) return status;
  definition.phases.resize(phases);
  for (std::uint32_t index = 0; index < phases; ++index) {
    CollectivePhase& phase = definition.phases[index];
    std::uint64_t sequence = 0;
    status = reader.u64(sequence);
    if (!status.is_ok()) return status;
    phase.phase_index = Sequence(sequence);
    status = reader.string(phase.label, limits::kLabelBytesMax);
    if (!status.is_ok()) return status;
    status = reader.boolean(phase.synchronization_point);
    if (!status.is_ok()) return status;
    status = reader.u32(phase.step_count);
    if (!status.is_ok()) return status;
  }
  std::uint32_t steps = 0;
  status = reader.count(steps, limits::kPhasesMaxPerCollective * limits::kStepsMaxPerPhase);
  if (!status.is_ok()) return status;
  definition.steps.resize(steps);
  for (std::uint32_t index = 0; index < steps; ++index) {
    CollectiveStep& step = definition.steps[index];
    std::uint64_t phase = 0;
    std::uint64_t sequence = 0;
    status = reader.u64(phase);
    if (!status.is_ok()) return status;
    status = reader.u64(sequence);
    if (!status.is_ok()) return status;
    step.phase_index = Sequence(phase);
    step.step_index = Sequence(sequence);
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(AlgorithmHint::kVendorSpecific)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "step algorithm hint is not defined by this version");
    }
    step.hint = static_cast<AlgorithmHint>(raw);
    status = reader.boolean(step.synchronization_point);
    if (!status.is_ok()) return status;
  }
  std::uint32_t metadata_count = 0;
  status = reader.count(metadata_count, limits::kMetadataEntriesMax);
  if (!status.is_ok()) return status;
  for (std::uint32_t index = 0; index < metadata_count; ++index) {
    std::string key;
    std::string value;
    status = reader.string(key, limits::kMetadataKeyBytesMax);
    if (!status.is_ok()) return status;
    status = reader.string(value, limits::kMetadataValueBytesMax);
    if (!status.is_ok()) return status;
    if (!definition.metadata.set(key, value)) {
      return Status(ErrorCode::kDecodeDuplicateEntry, "metadata carries a duplicate key");
    }
  }
  status = reader.boolean(definition.declares_barrier_semantics);
  if (!status.is_ok()) return status;
  status = reader.u64(definition.logical_bytes);
  if (!status.is_ok()) return status;
  std::uint64_t registration_sequence = 0;
  status = reader.u64(registration_sequence);
  if (!status.is_ok()) return status;
  definition.registration_sequence = Sequence(registration_sequence);
  out = std::move(definition);
  return Status::ok();
}

Status encode_flow_group_plan(const FlowGroupPlan& value, Writer& writer) {
  writer.identity(value.instance.id);
  writer.identity(value.instance.generation);
  writer.identity(value.instance.attempt);
  writer.u64(value.instance.attempt_sequence.value());
  writer.enumeration(value.collective_class);
  writer.enumeration(value.pattern);
  writer.enumeration(value.direction);
  writer.u64(value.plan_sequence.value());
  writer.boolean(value.requires_simultaneous_start);
  writer.u32(static_cast<std::uint32_t>(value.edges.size()));
  for (const FlowEdge& edge : value.edges) {
    writer.identity(edge.source);
    writer.identity(edge.destination);
    writer.u64(edge.logical_bytes);
    writer.u64(edge.step_index.value());
    writer.enumeration(edge.hint);
    writer.boolean(edge.crosses_rack);
  }
  return Status::ok();
}

Status decode_flow_group_plan(Reader& reader, FlowGroupPlan& out) {
  FlowGroupPlan plan;
  Status status = reader.identity(plan.instance.id);
  if (!status.is_ok()) return status;
  status = reader.identity(plan.instance.generation);
  if (!status.is_ok()) return status;
  status = reader.identity(plan.instance.attempt);
  if (!status.is_ok()) return status;
  std::uint64_t attempt_sequence = 0;
  status = reader.u64(attempt_sequence);
  if (!status.is_ok()) return status;
  plan.instance.attempt_sequence = Sequence(attempt_sequence);
  std::uint8_t raw = 0;
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(CollectiveClass::kUnknownVendor)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "collective class is not defined by this version");
  }
  plan.collective_class = static_cast<CollectiveClass>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(FlowPattern::kCustom)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "flow pattern is not defined by this version");
  }
  plan.pattern = static_cast<FlowPattern>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(FlowDirection::kBidirectional)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "flow direction is not defined by this version");
  }
  plan.direction = static_cast<FlowDirection>(raw);
  std::uint64_t plan_sequence = 0;
  status = reader.u64(plan_sequence);
  if (!status.is_ok()) return status;
  plan.plan_sequence = Sequence(plan_sequence);
  status = reader.boolean(plan.requires_simultaneous_start);
  if (!status.is_ok()) return status;
  std::uint32_t edges = 0;
  status = reader.count(edges, limits::kEdgesMaxPerFlowGroup);
  if (!status.is_ok()) return status;
  plan.edges.resize(edges);
  for (std::uint32_t index = 0; index < edges; ++index) {
    FlowEdge& edge = plan.edges[index];
    status = reader.identity(edge.source);
    if (!status.is_ok()) return status;
    status = reader.identity(edge.destination);
    if (!status.is_ok()) return status;
    status = reader.u64(edge.logical_bytes);
    if (!status.is_ok()) return status;
    std::uint64_t step = 0;
    status = reader.u64(step);
    if (!status.is_ok()) return status;
    edge.step_index = Sequence(step);
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(AlgorithmHint::kVendorSpecific)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "edge algorithm hint is not defined by this version");
    }
    edge.hint = static_cast<AlgorithmHint>(raw);
    status = reader.boolean(edge.crosses_rack);
    if (!status.is_ok()) return status;
  }
  out = std::move(plan);
  return Status::ok();
}

Status encode_flow_group(const FlowGroup& value, Writer& writer) {
  writer.identity(value.id);
  writer.identity(value.instance.id);
  writer.identity(value.instance.generation);
  writer.identity(value.instance.attempt);
  writer.enumeration(value.collective_class);
  writer.enumeration(value.pattern);
  writer.enumeration(value.direction);
  writer.u32(static_cast<std::uint32_t>(value.edges.size()));
  for (const FlowEdge& edge : value.edges) {
    writer.identity(edge.source);
    writer.identity(edge.destination);
    writer.u64(edge.logical_bytes);
    writer.u64(edge.step_index.value());
    writer.enumeration(edge.hint);
    writer.boolean(edge.crosses_rack);
  }
  writer.u32(static_cast<std::uint32_t>(value.participants.size()));
  for (const ParticipantId& participant : value.participants) writer.identity(participant);
  writer.u64(value.total_logical_bytes);
  writer.u32(value.max_fan_out);
  writer.u32(value.max_fan_in);
  writer.u32(value.cross_rack_edge_count);
  writer.boolean(value.requires_simultaneous_start);
  writer.u64(value.plan_sequence.value());
  return Status::ok();
}

Status decode_flow_group(Reader& reader, FlowGroup& out) {
  FlowGroup group;
  Status status = reader.identity(group.id);
  if (!status.is_ok()) return status;
  status = reader.identity(group.instance.id);
  if (!status.is_ok()) return status;
  status = reader.identity(group.instance.generation);
  if (!status.is_ok()) return status;
  status = reader.identity(group.instance.attempt);
  if (!status.is_ok()) return status;
  std::uint8_t raw = 0;
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(CollectiveClass::kUnknownVendor)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "collective class is not defined by this version");
  }
  group.collective_class = static_cast<CollectiveClass>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(FlowPattern::kCustom)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "flow pattern is not defined by this version");
  }
  group.pattern = static_cast<FlowPattern>(raw);
  status = reader.u8(raw);
  if (!status.is_ok()) return status;
  if (raw > static_cast<std::uint8_t>(FlowDirection::kBidirectional)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "flow direction is not defined by this version");
  }
  group.direction = static_cast<FlowDirection>(raw);
  std::uint32_t edges = 0;
  status = reader.count(edges, limits::kEdgesMaxPerFlowGroup);
  if (!status.is_ok()) return status;
  group.edges.resize(edges);
  for (std::uint32_t index = 0; index < edges; ++index) {
    FlowEdge& edge = group.edges[index];
    status = reader.identity(edge.source);
    if (!status.is_ok()) return status;
    status = reader.identity(edge.destination);
    if (!status.is_ok()) return status;
    status = reader.u64(edge.logical_bytes);
    if (!status.is_ok()) return status;
    std::uint64_t step = 0;
    status = reader.u64(step);
    if (!status.is_ok()) return status;
    edge.step_index = Sequence(step);
    status = reader.u8(raw);
    if (!status.is_ok()) return status;
    if (raw > static_cast<std::uint8_t>(AlgorithmHint::kVendorSpecific)) {
      return Status(ErrorCode::kDecodeInvalidEnum, "edge algorithm hint is not defined by this version");
    }
    edge.hint = static_cast<AlgorithmHint>(raw);
    status = reader.boolean(edge.crosses_rack);
    if (!status.is_ok()) return status;
  }
  std::uint32_t participants = 0;
  status = reader.count(participants, limits::kParticipantsMaxPerCollective);
  if (!status.is_ok()) return status;
  group.participants.resize(participants);
  for (std::uint32_t index = 0; index < participants; ++index) {
    status = reader.identity(group.participants[index]);
    if (!status.is_ok()) return status;
  }
  status = reader.u64(group.total_logical_bytes);
  if (!status.is_ok()) return status;
  status = reader.u32(group.max_fan_out);
  if (!status.is_ok()) return status;
  status = reader.u32(group.max_fan_in);
  if (!status.is_ok()) return status;
  status = reader.u32(group.cross_rack_edge_count);
  if (!status.is_ok()) return status;
  status = reader.boolean(group.requires_simultaneous_start);
  if (!status.is_ok()) return status;
  std::uint64_t plan_sequence = 0;
  status = reader.u64(plan_sequence);
  if (!status.is_ok()) return status;
  group.plan_sequence = Sequence(plan_sequence);
  out = std::move(group);
  return Status::ok();
}

}  // namespace ctf::protocol
