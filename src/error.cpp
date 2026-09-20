// Collective Traffic Fabric - error codes and reason chains.
// Copyright 2026 Summon Software Labs.
#include "ctf/error.hpp"

#include <algorithm>

namespace ctf {
namespace {

// Deliberately a switch, not a table lookup: an uncompilable enumeration value
// is a build failure rather than a silent "unknown" string.
constexpr std::string_view kUnknown = "unknown_error";

}  // namespace

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kOk: return "ok";
    case ErrorCode::kFrameTruncatedHeader: return "frame_truncated_header";
    case ErrorCode::kFrameTruncatedPayload: return "frame_truncated_payload";
    case ErrorCode::kFrameMagicMismatch: return "frame_magic_mismatch";
    case ErrorCode::kFrameVersionUnsupported: return "frame_version_unsupported";
    case ErrorCode::kFrameLengthOutOfRange: return "frame_length_out_of_range";
    case ErrorCode::kFrameIntegrityMismatch: return "frame_integrity_mismatch";
    case ErrorCode::kFrameTrailingGarbage: return "frame_trailing_garbage";
    case ErrorCode::kFrameKindUnknown: return "frame_kind_unknown";
    case ErrorCode::kFrameFlagsInvalid: return "frame_flags_invalid";
    case ErrorCode::kFrameLengthArithmeticOverflow: return "frame_length_arithmetic_overflow";
    case ErrorCode::kEnvelopeSessionUnknown: return "envelope_session_unknown";
    case ErrorCode::kEnvelopeSessionMismatch: return "envelope_session_mismatch";
    case ErrorCode::kEnvelopeIncarnationStale: return "envelope_incarnation_stale";
    case ErrorCode::kEnvelopeEpochStale: return "envelope_epoch_stale";
    case ErrorCode::kEnvelopeEpochUnknown: return "envelope_epoch_unknown";
    case ErrorCode::kEnvelopeIdentityMismatch: return "envelope_identity_mismatch";
    case ErrorCode::kEnvelopeProvenanceRejected: return "envelope_provenance_rejected";
    case ErrorCode::kEnvelopeReplayed: return "envelope_replayed";
    case ErrorCode::kEnvelopeSequenceRegression: return "envelope_sequence_regression";
    case ErrorCode::kDecodeTruncated: return "decode_truncated";
    case ErrorCode::kDecodeTrailingBytes: return "decode_trailing_bytes";
    case ErrorCode::kDecodeInvalidTag: return "decode_invalid_tag";
    case ErrorCode::kDecodeInvalidEnum: return "decode_invalid_enum";
    case ErrorCode::kDecodeInvalidBoolean: return "decode_invalid_boolean";
    case ErrorCode::kDecodeInvalidString: return "decode_invalid_string";
    case ErrorCode::kDecodeLengthOutOfRange: return "decode_length_out_of_range";
    case ErrorCode::kDecodeCountOutOfRange: return "decode_count_out_of_range";
    case ErrorCode::kDecodeMessageTooLarge: return "decode_message_too_large";
    case ErrorCode::kDecodeDuplicateEntry: return "decode_duplicate_entry";
    case ErrorCode::kDecodeNonCanonical: return "decode_non_canonical";
    case ErrorCode::kDecodeArithmeticOverflow: return "decode_arithmetic_overflow";
    case ErrorCode::kDecodeEmptyRequiredField: return "decode_empty_required_field";
    case ErrorCode::kValidationParticipantUnset: return "validation_participant_unset";
    case ErrorCode::kValidationParticipantDuplicate: return "validation_participant_duplicate";
    case ErrorCode::kValidationParticipantCountOutOfRange: return "validation_participant_count_out_of_range";
    case ErrorCode::kValidationEdgeUnset: return "validation_edge_unset";
    case ErrorCode::kValidationEdgeSelfLoop: return "validation_edge_self_loop";
    case ErrorCode::kValidationEdgeDuplicate: return "validation_edge_duplicate";
    case ErrorCode::kValidationEdgeEndpointNotParticipant: return "validation_edge_endpoint_not_participant";
    case ErrorCode::kValidationEdgeCountOutOfRange: return "validation_edge_count_out_of_range";
    case ErrorCode::kValidationPhaseOutOfRange: return "validation_phase_out_of_range";
    case ErrorCode::kValidationStepOutOfRange: return "validation_step_out_of_range";
    case ErrorCode::kValidationClassUnknownRejected: return "validation_class_unknown_rejected";
    case ErrorCode::kValidationMetadataLimit: return "validation_metadata_limit";
    case ErrorCode::kValidationLabelTooLong: return "validation_label_too_long";
    case ErrorCode::kValidationGenerationUnset: return "validation_generation_unset";
    case ErrorCode::kValidationAttemptUnset: return "validation_attempt_unset";
    case ErrorCode::kValidationRateInvalid: return "validation_rate_invalid";
    case ErrorCode::kValidationPolicyMissing: return "validation_policy_missing";
    case ErrorCode::kValidationTopologyMissing: return "validation_topology_missing";
    case ErrorCode::kValidationNodeUnknown: return "validation_node_unknown";
    case ErrorCode::kValidationCapacityNegative: return "validation_capacity_negative";
    case ErrorCode::kValidationReasonChainTooLong: return "validation_reason_chain_too_long";
    case ErrorCode::kStateCollectiveUnknown: return "state_collective_unknown";
    case ErrorCode::kStateCollectiveRetired: return "state_collective_retired";
    case ErrorCode::kStateCollectiveCancelled: return "state_collective_cancelled";
    case ErrorCode::kStateGenerationRegression: return "state_generation_regression";
    case ErrorCode::kStateAttemptRegression: return "state_attempt_regression";
    case ErrorCode::kStateDuplicateRegistration: return "state_duplicate_registration";
    case ErrorCode::kStateNotLive: return "state_not_live";
    case ErrorCode::kStateShuttingDown: return "state_shutting_down";
    case ErrorCode::kStateCapacityExceeded: return "state_capacity_exceeded";
    case ErrorCode::kStatePhaseRegression: return "state_phase_regression";
    case ErrorCode::kStatePlanMissing: return "state_plan_missing";
    case ErrorCode::kPersistenceOpenFailed: return "persistence_open_failed";
    case ErrorCode::kPersistenceReadFailed: return "persistence_read_failed";
    case ErrorCode::kPersistenceWriteFailed: return "persistence_write_failed";
    case ErrorCode::kPersistenceRenameFailed: return "persistence_rename_failed";
    case ErrorCode::kPersistenceTooLarge: return "persistence_too_large";
    case ErrorCode::kPersistenceCorrupt: return "persistence_corrupt";
    case ErrorCode::kPersistenceVersionUnsupported: return "persistence_version_unsupported";
    case ErrorCode::kPersistenceIntegrityMismatch: return "persistence_integrity_mismatch";
    case ErrorCode::kPersistenceTruncated: return "persistence_truncated";
    case ErrorCode::kPersistenceImpossibleState: return "persistence_impossible_state";
    case ErrorCode::kPersistenceDirectoryUnavailable: return "persistence_directory_unavailable";
    case ErrorCode::kTransportSocketFailed: return "transport_socket_failed";
    case ErrorCode::kTransportBindFailed: return "transport_bind_failed";
    case ErrorCode::kTransportListenFailed: return "transport_listen_failed";
    case ErrorCode::kTransportConnectFailed: return "transport_connect_failed";
    case ErrorCode::kTransportSendFailed: return "transport_send_failed";
    case ErrorCode::kTransportReceiveFailed: return "transport_receive_failed";
    case ErrorCode::kTransportClosed: return "transport_closed";
    case ErrorCode::kTransportTimeout: return "transport_timeout";
    case ErrorCode::kTransportAddressInvalid: return "transport_address_invalid";
    case ErrorCode::kTransportNotStarted: return "transport_not_started";
    case ErrorCode::kTransportSessionLimit: return "transport_session_limit";
    case ErrorCode::kAdmissionRateLimited: return "admission_rate_limited";
    case ErrorCode::kAdmissionBucketCapacity: return "admission_bucket_capacity";
    case ErrorCode::kAdmissionBucketUnset: return "admission_bucket_unset";
    case ErrorCode::kAdmissionClockRegression: return "admission_clock_regression";
    case ErrorCode::kCompletionStaleAttempt: return "completion_stale_attempt";
    case ErrorCode::kCompletionStaleGeneration: return "completion_stale_generation";
    case ErrorCode::kCompletionRetired: return "completion_retired";
    case ErrorCode::kCompletionCancelled: return "completion_cancelled";
    case ErrorCode::kCompletionUnknownCollective: return "completion_unknown_collective";
    case ErrorCode::kCompletionDuplicate: return "completion_duplicate";
    case ErrorCode::kInternalInvariant: return "internal_invariant";
    case ErrorCode::kInternalUnsupported: return "internal_unsupported";
  }
  return kUnknown;
}

bool is_transport_error(ErrorCode code) noexcept {
  const auto raw = static_cast<std::uint16_t>(code);
  return raw >= 100 && raw < 200;
}

bool is_decode_error(ErrorCode code) noexcept {
  const auto raw = static_cast<std::uint16_t>(code);
  return raw >= 300 && raw < 400;
}

bool is_persistence_error(ErrorCode code) noexcept {
  const auto raw = static_cast<std::uint16_t>(code);
  return raw >= 600 && raw < 700;
}

bool ReasonChain::add(std::string code, std::string detail, Identity subject) {
  if (reasons_.size() >= limits::kReasonChainMax) return false;
  if (detail.size() > limits::kReasonDetailBytesMax) {
    detail.resize(limits::kReasonDetailBytesMax);
  }
  if (code.empty()) code = "unspecified";
  reasons_.push_back(Reason(std::move(code), std::move(detail), subject));
  return true;
}

bool ReasonChain::add(const Reason& reason) { return add(reason.code, reason.detail, reason.subject); }

bool ReasonChain::contains(std::string_view code) const noexcept {
  for (const Reason& reason : reasons_) {
    if (reason.code == code) return true;
  }
  return false;
}

std::string ReasonChain::first_code() const {
  if (reasons_.empty()) return std::string();
  return reasons_.front().code;
}

std::string ReasonChain::to_string() const {
  std::string out;
  for (std::size_t index = 0; index < reasons_.size(); ++index) {
    if (index != 0) out += " -> ";
    out += reasons_[index].code;
    out += '(';
    out += reasons_[index].detail;
    if (reasons_[index].subject.is_some()) {
      out += " subject=";
      out += reasons_[index].subject.to_string();
    }
    out += ')';
  }
  return out;
}

std::string Status::to_string() const {
  std::string out(ctf::to_string(code_));
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace ctf
