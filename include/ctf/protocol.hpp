// Collective Traffic Fabric - framed, versioned, integrity checked wire
// protocol.  All wire input is untrusted: every field is length checked, every
// count is bounded, and every identity carried in the frame envelope is
// re-validated against the coordinator's own authority before it is believed.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_PROTOCOL_HPP
#define CTF_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ctf/decision.hpp"
#include "ctf/error.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"
#include "ctf/participant.hpp"

namespace ctf::protocol {

inline constexpr std::uint32_t kFrameMagic = 0x46544331u;  // "CTF1" little endian

enum class MessageKind : std::uint16_t {
  kInvalid = 0,
  kHello = 1,
  kHelloAck = 2,
  kHeartbeat = 3,
  kHeartbeatAck = 22,
  kRegisterCollective = 4,
  kRegisterAck = 5,
  kBeginAttempt = 6,
  kAttemptAck = 7,
  kPlanFlowGroup = 8,
  kFlowGroupDecision = 9,
  kCancelCollective = 10,
  kRetireCollective = 11,
  kLifecycleAck = 12,
  kIngestEvidence = 13,
  kEvidenceAck = 14,
  kInspectRequest = 15,
  kInspectResponse = 16,
  kExplainRequest = 17,
  kExplainResponse = 18,
  kErrorResponse = 19,
  kInstallPolicy = 20,
  kInstallPolicyAck = 21,
  kShutdown = 23,
  kShutdownAck = 24,
  // Every value below this one is a defined message kind, and no value at or
  // above it is ever accepted from the wire.
  kKindCount = 25,
};

[[nodiscard]] std::string_view to_string(MessageKind value) noexcept;
[[nodiscard]] bool message_kind_is_known(std::uint16_t raw) noexcept;

// ---------------------------------------------------------------------------
// Frame.  Fixed 40 byte header.  Layout (little endian):
//   0  magic             u32
//   4  header_bytes      u16   (must equal kHeaderBytes)
//   6  kind              u16
//   8  flags             u16
//  10  protocol_major    u16
//  12  protocol_minor    u16
//  14  payload_bytes     u32
//  18  payload_crc32     u32
//  22  header_crc32      u32   (CRC of bytes 0..21)
//  26  reserved          u16   (must be zero)
//  28  session_hi        u64
//  36  session_lo        u64  -> wait: 8 + 8 = 16 bytes at 28..43
// The header is therefore 44 bytes: 28 fixed + 16 session.
//
// Envelope (first 40 bytes of the payload) carries boot incarnation, epoch,
// frame sequence and correlation.  It is integrity checked by payload_crc32
// and, more importantly, validated against coordinator authority.
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kHeaderBytes = 44;
// envelope: incarnation (16) + epoch (16) + frame sequence (8) + correlation (8)
inline constexpr std::uint32_t kEnvelopeBytes = 48;

inline constexpr std::uint16_t kFlagRequest = 0x0001;
inline constexpr std::uint16_t kFlagResponse = 0x0002;
inline constexpr std::uint16_t kFlagFinal = 0x0004;
inline constexpr std::uint16_t kFlagKnownMask = kFlagRequest | kFlagResponse | kFlagFinal;

struct Envelope {
  SessionId session{};
  BootIncarnation incarnation{};
  CoordinatorEpoch epoch{};
  Sequence frame_sequence{};
  std::uint64_t correlation = 0;
};

struct Frame {
  std::uint16_t kind = 0;
  std::uint16_t flags = 0;
  Envelope envelope{};
  std::vector<std::uint8_t> payload;  // message body, envelope excluded
};

[[nodiscard]] Status encode_envelope(const Envelope& envelope, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_envelope(const std::uint8_t* data, std::size_t size, Envelope& out);

[[nodiscard]] Status encode_frame(const Frame& frame, std::vector<std::uint8_t>& out);
// Decodes exactly one frame from the front of the buffer.  Reports
// kTransportClosed when the buffer is empty and kFrameTruncatedHeader /
// kFrameTruncatedPayload when more bytes are required.  Never accepts trailing
// bytes: consumed is reported so the caller can detect them.
[[nodiscard]] Status decode_frame(const std::uint8_t* data, std::size_t size, Frame& out,
                                  std::size_t& consumed);

// ---------------------------------------------------------------------------
// Payload codec.  Deterministic, canonical and strict: no trailing bytes, no
// non-minimal encodings, no unbounded counts, no silent truncation.
// ---------------------------------------------------------------------------
class Writer {
 public:
  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void identity(Identity value);
  void string(std::string_view value);
  void raw(const std::vector<std::uint8_t>& value);
  template <typename Enum>
  void enumeration(Enum value) {
    u8(static_cast<std::uint8_t>(value));
  }
  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

 private:
  std::vector<std::uint8_t> data_;
};

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] Status u8(std::uint8_t& out);
  [[nodiscard]] Status u16(std::uint16_t& out);
  [[nodiscard]] Status u32(std::uint32_t& out);
  [[nodiscard]] Status u64(std::uint64_t& out);
  [[nodiscard]] Status i64(std::int64_t& out);
  [[nodiscard]] Status boolean(bool& out);
  [[nodiscard]] Status identity(Identity& out);
  [[nodiscard]] Status string(std::string& out, std::uint32_t max_bytes = limits::kLabelBytesMax);
  [[nodiscard]] Status raw(std::vector<std::uint8_t>& out, std::uint32_t max_bytes = limits::kFramePayloadMaxBytes);
  template <typename Enum>
  [[nodiscard]] Status enumeration(Enum& out) {
    std::uint8_t raw = 0;
    Status status = u8(raw);
    if (!status.is_ok()) return status;
    out = static_cast<Enum>(raw);
    return Status::ok();
  }
  // Reads a bounded item count and rejects anything outside [1, max].
  [[nodiscard]] Status count(std::uint32_t& out, std::uint32_t max);
  [[nodiscard]] Status require_end() const;
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

 private:
  [[nodiscard]] Status take(std::size_t count);

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

// ---- payload structures ---------------------------------------------------
struct HelloPayload {
  std::string client_label;
  std::string client_version;
  std::uint64_t client_boot_monotonic_ms = 0;
};

struct HelloAckPayload {
  SessionId session{};
  BootIncarnation incarnation{};
  CoordinatorEpoch epoch{};
  PolicyGeneration policy_generation{};
  TopologyGeneration topology_generation{};
  EvidenceGeneration capacity_generation{};
  EvidenceGeneration congestion_generation{};
  std::uint32_t max_frame_payload_bytes = limits::kFramePayloadMaxBytes;
  std::string coordinator_label;
};

struct HeartbeatPayload {
  ParticipantId participant{};
  BootIncarnation incarnation{};
  ParticipantState state = ParticipantState::kLive;
  NodeId node{};
  std::uint64_t client_clock_ms = 0;
};

// Every request is answered.  The heartbeat acknowledgement carries back the
// coordinator's own view of the participant, including the expiry instant it
// recorded, so a peer can see exactly what authority it currently holds rather
// than assuming that sending a message established anything.
struct HeartbeatAckPayload {
  ParticipantId participant{};
  BootIncarnation incarnation{};
  ParticipantState state = ParticipantState::kUnspecified;
  std::uint64_t expires_at_monotonic_ms = 0;
  bool accepted = false;
  ReasonChain reasons{};
};

struct RegisterCollectivePayload {
  CollectiveDefinition definition{};
};

struct RegisterAckPayload {
  CollectiveId id{};
  CollectiveGeneration generation{};
  std::uint8_t result = 0;
  Sequence registration_sequence{};
  ReasonChain reasons{};
};

struct BeginAttemptPayload {
  CollectiveId id{};
  CollectiveGeneration generation{};
  CollectiveAttemptId attempt{};
  std::uint64_t transfer_bytes = 0;
};

struct AttemptAckPayload {
  CollectiveId id{};
  CollectiveAttemptId attempt{};
  CollectiveAttemptId previous_attempt{};
  CollectiveState state = CollectiveState::kUnregistered;
  ReasonChain reasons{};
};

struct PlanFlowGroupPayload {
  DecisionRequest request{};
};

struct DecisionPayload {
  DecisionRecord decision{};
};

struct LifecyclePayload {
  CollectiveId id{};
  CollectiveGeneration generation{};
  CollectiveAttemptId attempt{};
  std::string reason;
};

struct LifecycleAckPayload {
  CollectiveId id{};
  CollectiveState state = CollectiveState::kUnregistered;
  ReasonChain reasons{};
};

enum class EvidenceKindTag : std::uint8_t {
  kNone = 0,
  kCapacity = 1,
  kCongestion = 2,
  kTopology = 3,
  // A traffic policy is not an observation, but it travels the same way and is
  // governed by the same generation rules, so it shares the tag space rather
  // than acquiring a second, subtly different installation path.
  kPolicy = 4,
};

struct InstallPolicyPayload {
  TrafficPolicy policy{};
};

struct InstallPolicyAckPayload {
  PolicyGeneration generation{};
  std::uint8_t result = 0;
  ReasonChain reasons{};
};

struct EvidencePayload {
  EvidenceKindTag kind = EvidenceKindTag::kNone;
  CapacityEvidence capacity{};
  CongestionEvidence congestion{};
  TopologyEvidence topology{};
  TrafficPolicy policy{};
};

struct EvidenceAckPayload {
  EvidenceKindTag kind = EvidenceKindTag::kNone;
  std::uint8_t result = 0;
  EvidenceGeneration generation{};
  ReasonChain reasons{};
};

struct InspectRequestPayload {
  std::uint8_t subject = 0;  // 0 summary, 1 collective, 2 decisions, 3 peers, 4 policy
  CollectiveId id{};
  std::uint32_t offset = 0;
  std::uint32_t limit = 64;
};

struct InspectResponsePayload {
  std::uint8_t subject = 0;
  std::string body;
  bool truncated = false;
};

struct ExplainRequestPayload {
  CollectiveId id{};
  CollectiveAttemptId attempt{};
  std::uint64_t correlation = 0;
  bool has_correlation = false;
};

struct ExplainResponsePayload {
  bool found = false;
  DecisionRecord decision{};
  std::string explanation;
};

struct ErrorPayload {
  ErrorCode code = ErrorCode::kOk;
  std::string message;
  ReasonChain reasons{};
};

// ---- per message codecs ---------------------------------------------------
[[nodiscard]] Status encode_hello(const HelloPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_hello(const std::vector<std::uint8_t>& in, HelloPayload& out);
[[nodiscard]] Status encode_hello_ack(const HelloAckPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_hello_ack(const std::vector<std::uint8_t>& in, HelloAckPayload& out);
[[nodiscard]] Status encode_heartbeat(const HeartbeatPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_heartbeat(const std::vector<std::uint8_t>& in, HeartbeatPayload& out);
[[nodiscard]] Status encode_heartbeat_ack(const HeartbeatAckPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_heartbeat_ack(const std::vector<std::uint8_t>& in, HeartbeatAckPayload& out);
[[nodiscard]] Status encode_register_collective(const RegisterCollectivePayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_register_collective(const std::vector<std::uint8_t>& in, RegisterCollectivePayload& out);
[[nodiscard]] Status encode_register_ack(const RegisterAckPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_register_ack(const std::vector<std::uint8_t>& in, RegisterAckPayload& out);
[[nodiscard]] Status encode_begin_attempt(const BeginAttemptPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_begin_attempt(const std::vector<std::uint8_t>& in, BeginAttemptPayload& out);
[[nodiscard]] Status encode_attempt_ack(const AttemptAckPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_attempt_ack(const std::vector<std::uint8_t>& in, AttemptAckPayload& out);
[[nodiscard]] Status encode_plan_flow_group(const PlanFlowGroupPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_plan_flow_group(const std::vector<std::uint8_t>& in, PlanFlowGroupPayload& out);
[[nodiscard]] Status encode_decision(const DecisionPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_decision(const std::vector<std::uint8_t>& in, DecisionPayload& out);
[[nodiscard]] Status encode_lifecycle(const LifecyclePayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_lifecycle(const std::vector<std::uint8_t>& in, LifecyclePayload& out);
[[nodiscard]] Status encode_lifecycle_ack(const LifecycleAckPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_lifecycle_ack(const std::vector<std::uint8_t>& in, LifecycleAckPayload& out);
[[nodiscard]] Status encode_evidence(const EvidencePayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_evidence(const std::vector<std::uint8_t>& in, EvidencePayload& out);
[[nodiscard]] Status encode_evidence_ack(const EvidenceAckPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_evidence_ack(const std::vector<std::uint8_t>& in, EvidenceAckPayload& out);
[[nodiscard]] Status encode_inspect_request(const InspectRequestPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_inspect_request(const std::vector<std::uint8_t>& in, InspectRequestPayload& out);
[[nodiscard]] Status encode_inspect_response(const InspectResponsePayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_inspect_response(const std::vector<std::uint8_t>& in, InspectResponsePayload& out);
[[nodiscard]] Status encode_explain_request(const ExplainRequestPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_explain_request(const std::vector<std::uint8_t>& in, ExplainRequestPayload& out);
[[nodiscard]] Status encode_explain_response(const ExplainResponsePayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_explain_response(const std::vector<std::uint8_t>& in, ExplainResponsePayload& out);
[[nodiscard]] Status encode_error(const ErrorPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_error(const std::vector<std::uint8_t>& in, ErrorPayload& out);

// ---- shared nested codecs -------------------------------------------------
[[nodiscard]] Status encode_definition(const CollectiveDefinition& value, Writer& writer);
[[nodiscard]] Status decode_definition(Reader& reader, CollectiveDefinition& out);
[[nodiscard]] Status encode_flow_group_plan(const FlowGroupPlan& value, Writer& writer);
[[nodiscard]] Status decode_flow_group_plan(Reader& reader, FlowGroupPlan& out);
[[nodiscard]] Status encode_flow_group(const FlowGroup& value, Writer& writer);
[[nodiscard]] Status decode_flow_group(Reader& reader, FlowGroup& out);
[[nodiscard]] Status encode_decision_request(const DecisionRequest& value, Writer& writer);
[[nodiscard]] Status decode_decision_request(Reader& reader, DecisionRequest& out);
[[nodiscard]] Status encode_decision_record(const DecisionRecord& value, Writer& writer);
[[nodiscard]] Status decode_decision_record(Reader& reader, DecisionRecord& out);
[[nodiscard]] Status encode_reason_chain(const ReasonChain& value, Writer& writer);
[[nodiscard]] Status decode_reason_chain(Reader& reader, ReasonChain& out);
[[nodiscard]] Status encode_capacity(const CapacityEvidence& value, Writer& writer);
[[nodiscard]] Status decode_capacity(Reader& reader, CapacityEvidence& out);
[[nodiscard]] Status encode_congestion(const CongestionEvidence& value, Writer& writer);
[[nodiscard]] Status decode_congestion(Reader& reader, CongestionEvidence& out);
[[nodiscard]] Status encode_topology(const TopologyEvidence& value, Writer& writer);
[[nodiscard]] Status decode_topology(Reader& reader, TopologyEvidence& out);
[[nodiscard]] Status encode_traffic_policy(const TrafficPolicy& value, Writer& writer);
[[nodiscard]] Status decode_traffic_policy(Reader& reader, TrafficPolicy& out);
[[nodiscard]] Status encode_install_policy(const InstallPolicyPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_install_policy(const std::vector<std::uint8_t>& in, InstallPolicyPayload& out);
[[nodiscard]] Status encode_install_policy_ack(const InstallPolicyAckPayload& value, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_install_policy_ack(const std::vector<std::uint8_t>& in, InstallPolicyAckPayload& out);

}  // namespace ctf::protocol

#endif  // CTF_PROTOCOL_HPP
