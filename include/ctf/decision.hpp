// Collective Traffic Fabric - admission outcomes, bound decisions and
// deterministic explanations.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_DECISION_HPP
#define CTF_DECISION_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ctf/collective.hpp"
#include "ctf/error.hpp"
#include "ctf/flow_group.hpp"
#include "ctf/identity.hpp"
#include "ctf/snapshot.hpp"
#include "ctf/sync.hpp"
#include "ctf/traffic.hpp"

namespace ctf {

// Every terminal outcome a flow-group decision can carry.  The set is closed
// and no other value is produced.
enum class Outcome : std::uint8_t {
  kAdmitted = 0,
  kDeferred = 1,
  kRateLimited = 2,
  kIsolatedClass = 3,
  kRequiresReplan = 4,
  kRejectedStale = 5,
  kRejectedCapacity = 6,
  kRejectedPolicy = 7,
  kUnknownSemantics = 8,
};

[[nodiscard]] std::string_view to_string(Outcome value) noexcept;
[[nodiscard]] bool outcome_is_rejection(Outcome value) noexcept;
[[nodiscard]] bool outcome_grants_authority(Outcome value) noexcept;
[[nodiscard]] bool outcome_requires_replan(Outcome value) noexcept;

// Which authority axis defeated a request.  Populated only for rejections so
// that operators can alert per axis.
enum class AuthorityAxis : std::uint8_t {
  kNone = 0,
  kCoordinatorEpoch = 1,
  kTopologyGeneration = 2,
  kPolicyGeneration = 3,
  kCollectiveGeneration = 4,
  kAttemptGeneration = 5,
  kParticipantLiveness = 6,
  kCapacityEvidence = 7,
  kCongestionEvidence = 8,
  kLifecycle = 9,
  kSemantics = 10,
  kPlan = 11,
};

[[nodiscard]] std::string_view to_string(AuthorityAxis value) noexcept;

// ---------------------------------------------------------------------------
// Request.  Supplied by a planner or coordinator; every externally supplied
// field is treated as untrusted until validated.
// ---------------------------------------------------------------------------
struct DecisionRequest {
  SessionId session{};
  CollectiveInstance instance{};
  // Generations the requester believes are current.  A mismatch is evidence
  // about the requester, not about the fabric, and is always reported.
  TopologyGeneration observed_topology_generation{};
  PolicyGeneration observed_policy_generation{};
  CoordinatorEpoch observed_epoch{};
  EvidenceGeneration observed_capacity_generation{};
  EvidenceGeneration observed_congestion_generation{};

  bool has_flow_group_plan = false;
  FlowGroupPlan plan{};
  // Optional override requested by a member.  Honored only when policy allows
  // it; an override may never strengthen an unknown collective.
  bool has_requested_class = false;
  TrafficClass requested_class = TrafficClass::kUnclassified;
  std::uint64_t requested_transfer_bytes = 0;
};

// ---------------------------------------------------------------------------
// Decision record.  A decision is legal only for the exact generation set it
// bound; anything else is a stale decision and must be re-authorized.
// ---------------------------------------------------------------------------
struct DecisionRecord {
  CollectiveInstance instance{};
  CollectiveClass collective_class = CollectiveClass::kUnknown;
  AlgorithmHint algorithm_hint = AlgorithmHint::kUnspecified;
  Outcome outcome = Outcome::kRejectedStale;
  AuthorityAxis axis = AuthorityAxis::kNone;
  ErrorCode code = ErrorCode::kOk;

  FlowGroupId flow_group{};
  bool flow_group_constructed = false;
  FlowGroup group{};
  std::vector<ParticipantId> participants;  // canonical ascending

  TrafficClass traffic_class = TrafficClass::kUnclassified;
  std::uint64_t priority = 0;
  IsolationMode isolation = IsolationMode::kShared;
  PacingIntent pacing{};
  SyncSensitivity sync{};

  // The binding.  A decision that is not bound to all of these is invalid.
  CollectiveGeneration collective_generation{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  EvidenceGeneration capacity_generation{};
  EvidenceGeneration congestion_generation{};
  CoordinatorEpoch epoch{};

  bool congestion_evidence_used = false;
  bool capacity_evidence_used = false;
  bool all_members_live = false;

  Sequence phase_index{};
  Sequence step_index{};

  ReasonChain reasons{};
  std::uint64_t decided_monotonic_ms = 0;
  Sequence decision_sequence{};

  // Deterministic 64 bit content digest over every bound field.  Reproducible
  // across processes for identical inputs.
  [[nodiscard]] std::uint64_t fingerprint() const noexcept;
  // Canonical single line summary used in tests and inspection output.
  [[nodiscard]] std::string summary() const;
  // Multi line deterministic explanation of the bindings and the reason chain.
  [[nodiscard]] std::string explain() const;
};

// ---------------------------------------------------------------------------
// Validation of untrusted requests.  Performed before any evidence is read.
// ---------------------------------------------------------------------------
[[nodiscard]] Status validate_decision_request(const DecisionRequest& request);

}  // namespace ctf

#endif  // CTF_DECISION_HPP
