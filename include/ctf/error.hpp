// Collective Traffic Fabric - deterministic error codes and reason chains.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_ERROR_HPP
#define CTF_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ctf/identity.hpp"
#include "ctf/limits.hpp"

namespace ctf {

// Every failure surface in this runtime reports a code from this enumeration.
// Codes are stable, machine readable, and never replaced by a bare boolean.
enum class ErrorCode : std::uint16_t {
  kOk = 0,

  // transport / framing
  kFrameTruncatedHeader = 100,
  kFrameTruncatedPayload = 101,
  kFrameMagicMismatch = 102,
  kFrameVersionUnsupported = 103,
  kFrameLengthOutOfRange = 104,
  kFrameIntegrityMismatch = 105,
  kFrameTrailingGarbage = 106,
  kFrameKindUnknown = 107,
  kFrameFlagsInvalid = 108,
  kFrameLengthArithmeticOverflow = 109,

  // envelope / authority binding
  kEnvelopeSessionUnknown = 200,
  kEnvelopeSessionMismatch = 201,
  kEnvelopeIncarnationStale = 202,
  kEnvelopeEpochStale = 203,
  kEnvelopeEpochUnknown = 204,
  kEnvelopeIdentityMismatch = 205,
  kEnvelopeProvenanceRejected = 206,
  kEnvelopeReplayed = 207,
  kEnvelopeSequenceRegression = 208,

  // decode
  kDecodeTruncated = 300,
  kDecodeTrailingBytes = 301,
  kDecodeInvalidTag = 302,
  kDecodeInvalidEnum = 303,
  kDecodeInvalidBoolean = 304,
  kDecodeInvalidString = 305,
  kDecodeLengthOutOfRange = 306,
  kDecodeCountOutOfRange = 307,
  kDecodeMessageTooLarge = 308,
  kDecodeDuplicateEntry = 309,
  kDecodeNonCanonical = 310,
  kDecodeArithmeticOverflow = 311,
  kDecodeEmptyRequiredField = 312,

  // domain validation
  kValidationParticipantUnset = 400,
  kValidationParticipantDuplicate = 401,
  kValidationParticipantCountOutOfRange = 402,
  kValidationEdgeUnset = 403,
  kValidationEdgeSelfLoop = 404,
  kValidationEdgeDuplicate = 405,
  kValidationEdgeEndpointNotParticipant = 406,
  kValidationEdgeCountOutOfRange = 407,
  kValidationPhaseOutOfRange = 408,
  kValidationStepOutOfRange = 409,
  kValidationClassUnknownRejected = 410,
  kValidationMetadataLimit = 411,
  kValidationLabelTooLong = 412,
  kValidationGenerationUnset = 413,
  kValidationAttemptUnset = 414,
  kValidationRateInvalid = 415,
  kValidationPolicyMissing = 416,
  kValidationTopologyMissing = 417,
  kValidationNodeUnknown = 418,
  kValidationCapacityNegative = 419,
  kValidationReasonChainTooLong = 420,

  // state machine / lifecycle
  kStateCollectiveUnknown = 500,
  kStateCollectiveRetired = 501,
  kStateCollectiveCancelled = 502,
  kStateGenerationRegression = 503,
  kStateAttemptRegression = 504,
  kStateDuplicateRegistration = 505,
  kStateNotLive = 506,
  kStateShuttingDown = 507,
  kStateCapacityExceeded = 508,
  kStatePhaseRegression = 509,
  kStatePlanMissing = 510,

  // persistence
  kPersistenceOpenFailed = 600,
  kPersistenceReadFailed = 601,
  kPersistenceWriteFailed = 602,
  kPersistenceRenameFailed = 603,
  kPersistenceTooLarge = 604,
  kPersistenceCorrupt = 605,
  kPersistenceVersionUnsupported = 606,
  kPersistenceIntegrityMismatch = 607,
  kPersistenceTruncated = 608,
  kPersistenceImpossibleState = 609,
  kPersistenceDirectoryUnavailable = 610,

  // transport runtime
  kTransportSocketFailed = 700,
  kTransportBindFailed = 701,
  kTransportListenFailed = 702,
  kTransportConnectFailed = 703,
  kTransportSendFailed = 704,
  kTransportReceiveFailed = 705,
  kTransportClosed = 706,
  kTransportTimeout = 707,
  kTransportAddressInvalid = 708,
  kTransportNotStarted = 709,
  kTransportSessionLimit = 710,

  // admission control
  kAdmissionRateLimited = 800,
  kAdmissionBucketCapacity = 801,
  kAdmissionBucketUnset = 802,
  kAdmissionClockRegression = 803,

  // cancellation / retirement / stale completion
  kCompletionStaleAttempt = 900,
  kCompletionStaleGeneration = 901,
  kCompletionRetired = 902,
  kCompletionCancelled = 903,
  kCompletionUnknownCollective = 904,
  kCompletionDuplicate = 905,

  // internal
  kInternalInvariant = 1000,
  kInternalUnsupported = 1001,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;
[[nodiscard]] bool is_transport_error(ErrorCode code) noexcept;
[[nodiscard]] bool is_decode_error(ErrorCode code) noexcept;
[[nodiscard]] bool is_persistence_error(ErrorCode code) noexcept;

// A single machine readable step in an explanation.  Codes are stable strings
// (snake_case) so that operators can alert on them.
struct Reason {
  std::string code;
  std::string detail;
  Identity subject{};  // the identity the reason is about, when applicable

  Reason() = default;
  Reason(std::string code_in, std::string detail_in, Identity subject_in = {})
      : code(std::move(code_in)), detail(std::move(detail_in)), subject(subject_in) {}
};

// Ordered evidence: the first entry is the outermost rule that produced the
// outcome, later entries are the supporting or defeating facts beneath it.
class ReasonChain {
 public:
  ReasonChain() = default;

  // Appends a reason.  Returns false (and appends nothing) when the chain is at
  // its hard bound, so that an adversarial peer cannot grow it without limit.
  bool add(std::string code, std::string detail, Identity subject = {});
  bool add(const Reason& reason);

  [[nodiscard]] bool empty() const noexcept { return reasons_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return reasons_.size(); }
  [[nodiscard]] const Reason& operator[](std::size_t index) const { return reasons_[index]; }
  [[nodiscard]] const std::vector<Reason>& reasons() const noexcept { return reasons_; }
  [[nodiscard]] bool contains(std::string_view code) const noexcept;
  [[nodiscard]] std::string first_code() const;
  // Deterministic single line rendering: "code(detail) -> code(detail)".
  [[nodiscard]] std::string to_string() const;
  void clear() noexcept { reasons_.clear(); }

 private:
  std::vector<Reason> reasons_;
};

class Status {
 public:
  Status() = default;
  Status(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  [[nodiscard]] static Status ok() { return Status(); }
  [[nodiscard]] bool is_ok() const noexcept { return code_ == ErrorCode::kOk; }
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] std::string to_string() const;

 private:
  ErrorCode code_ = ErrorCode::kOk;
  std::string message_;
};

}  // namespace ctf

#endif  // CTF_ERROR_HPP
