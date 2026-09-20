// Collective Traffic Fabric - decision records and deterministic explanations.
// Copyright 2026 Summon Software Labs.
#include "ctf/decision.hpp"

#include <algorithm>

namespace ctf {
namespace {

std::uint64_t mix64(std::uint64_t state, std::uint64_t value) noexcept {
  state ^= value + 0x9E3779B97F4A7C15ull + (state << 6) + (state >> 2);
  state *= 0xFF51AFD7ED558CCDull;
  state ^= state >> 33;
  return state;
}

std::uint64_t mix_identity(std::uint64_t state, Identity value) noexcept {
  return mix64(mix64(state, value.high()), value.low());
}

std::string decimal(std::uint64_t value) {
  if (value == 0) return "0";
  std::string digits;
  while (value != 0) {
    digits.insert(digits.begin(), static_cast<char>('0' + (value % 10)));
    value /= 10;
  }
  return digits;
}

}  // namespace

std::string_view to_string(Outcome value) noexcept {
  switch (value) {
    case Outcome::kAdmitted: return "ADMITTED";
    case Outcome::kDeferred: return "DEFERRED";
    case Outcome::kRateLimited: return "RATE_LIMITED";
    case Outcome::kIsolatedClass: return "ISOLATED_CLASS";
    case Outcome::kRequiresReplan: return "REQUIRES_REPLAN";
    case Outcome::kRejectedStale: return "REJECTED_STALE";
    case Outcome::kRejectedCapacity: return "REJECTED_CAPACITY";
    case Outcome::kRejectedPolicy: return "REJECTED_POLICY";
    case Outcome::kUnknownSemantics: return "UNKNOWN_SEMANTICS";
  }
  return "REJECTED_STALE";
}

bool outcome_is_rejection(Outcome value) noexcept {
  switch (value) {
    case Outcome::kRejectedStale:
    case Outcome::kRejectedCapacity:
    case Outcome::kRejectedPolicy:
    case Outcome::kUnknownSemantics:
      return true;
    default:
      return false;
  }
}

bool outcome_grants_authority(Outcome value) noexcept {
  // Authority is granted only by an outright admission or by an admission into
  // an isolated class.  Everything else withholds it, including DEFERRED and
  // RATE_LIMITED, which are decisions about timing rather than permission.
  return value == Outcome::kAdmitted || value == Outcome::kIsolatedClass;
}

bool outcome_requires_replan(Outcome value) noexcept {
  return value == Outcome::kRequiresReplan || value == Outcome::kDeferred ||
         value == Outcome::kRateLimited;
}

std::string_view to_string(AuthorityAxis value) noexcept {
  switch (value) {
    case AuthorityAxis::kNone: return "none";
    case AuthorityAxis::kCoordinatorEpoch: return "coordinator_epoch";
    case AuthorityAxis::kTopologyGeneration: return "topology_generation";
    case AuthorityAxis::kPolicyGeneration: return "policy_generation";
    case AuthorityAxis::kCollectiveGeneration: return "collective_generation";
    case AuthorityAxis::kAttemptGeneration: return "attempt_generation";
    case AuthorityAxis::kParticipantLiveness: return "participant_liveness";
    case AuthorityAxis::kCapacityEvidence: return "capacity_evidence";
    case AuthorityAxis::kCongestionEvidence: return "congestion_evidence";
    case AuthorityAxis::kLifecycle: return "lifecycle";
    case AuthorityAxis::kSemantics: return "semantics";
    case AuthorityAxis::kPlan: return "plan";
  }
  return "none";
}

std::uint64_t DecisionRecord::fingerprint() const noexcept {
  std::uint64_t state = 0x452821E638D01377ull;
  state = mix_identity(state, instance.id);
  state = mix_identity(state, instance.generation);
  state = mix_identity(state, instance.attempt);
  state = mix64(state, static_cast<std::uint64_t>(collective_class));
  state = mix64(state, static_cast<std::uint64_t>(algorithm_hint));
  state = mix64(state, static_cast<std::uint64_t>(outcome));
  state = mix64(state, static_cast<std::uint64_t>(axis));
  state = mix64(state, static_cast<std::uint64_t>(code));
  state = mix_identity(state, flow_group);
  state = mix64(state, static_cast<std::uint64_t>(traffic_class));
  state = mix64(state, priority);
  state = mix64(state, static_cast<std::uint64_t>(isolation));
  state = mix_identity(state, collective_generation);
  state = mix_identity(state, topology_generation);
  state = mix_identity(state, policy_generation);
  state = mix_identity(state, capacity_generation);
  state = mix_identity(state, congestion_generation);
  state = mix_identity(state, epoch);
  state = mix64(state, static_cast<std::uint64_t>(pacing.mode));
  state = mix64(state, pacing.admitted_bandwidth_bps);
  state = mix64(state, pacing.ceiling_bandwidth_bps);
  state = mix64(state, pacing.burst_bytes);
  state = mix64(state, pacing.utilization_bps);
  state = mix64(state, pacing.release_after_monotonic_ms);
  state = mix64(state, participants.size());
  for (const ParticipantId& participant : participants) state = mix_identity(state, participant);
  state = mix64(state, static_cast<std::uint64_t>(sync.kind));
  state = mix64(state, static_cast<std::uint64_t>(sync.basis));
  state = mix64(state, sync.barrier_member_count);
  state = mix64(state, static_cast<std::uint64_t>(congestion_evidence_used));
  state = mix64(state, static_cast<std::uint64_t>(capacity_evidence_used));
  state = mix64(state, static_cast<std::uint64_t>(all_members_live));
  state = mix64(state, phase_index.value());
  state = mix64(state, step_index.value());
  state = mix64(state, group.id.high());
  state = mix64(state, group.id.low());
  state = mix64(state, group.edges.size());
  state = mix64(state, group.total_logical_bytes);
  for (const Reason& reason : reasons.reasons()) {
    for (char character : reason.code) state = mix64(state, static_cast<std::uint64_t>(character));
    state = mix64(state, 0x5EEDull);
  }
  return state;
}

std::string DecisionRecord::summary() const {
  std::string out;
  out += std::string(to_string(outcome));
  out += " collective=";
  out += instance.id.to_string();
  out += " gen=";
  out += instance.generation.to_string();
  out += " attempt=";
  out += instance.attempt.to_string();
  out += " class=";
  out += std::string(to_string(collective_class));
  out += " traffic=";
  out += std::string(to_string(traffic_class));
  out += " axis=";
  out += std::string(to_string(axis));
  out += " code=";
  out += std::string(to_string(code));
  return out;
}

std::string DecisionRecord::explain() const {
  std::string out;
  out += "outcome: ";
  out += std::string(to_string(outcome));
  out += "\n";
  out += "authority_axis: ";
  out += std::string(to_string(axis));
  out += "\nerror_code: ";
  out += std::string(to_string(code));
  out += "\ncollective: ";
  out += instance.id.to_string();
  out += "\ncollective_generation: ";
  out += instance.generation.to_string();
  out += "\nattempt: ";
  out += instance.attempt.to_string();
  out += "\ncollective_class: ";
  out += std::string(to_string(collective_class));
  out += "\nalgorithm_hint: ";
  out += std::string(to_string(algorithm_hint));
  out += "\nparticipants: ";
  out += decimal(participants.size());
  out += "\ntraffic_class: ";
  out += std::string(to_string(traffic_class));
  out += "\npriority: ";
  out += decimal(priority);
  out += "\nisolation: ";
  out += std::string(to_string(isolation));
  out += "\nflow_group: ";
  out += flow_group.is_some() ? flow_group.to_string() : std::string("none");
  out += "\nedges: ";
  out += decimal(group.edges.size());
  out += "\ntotal_logical_bytes: ";
  out += decimal(group.total_logical_bytes);
  out += "\npacing_mode: ";
  out += std::string(to_string(pacing.mode));
  out += "\nadmitted_bandwidth_bps: ";
  out += decimal(pacing.admitted_bandwidth_bps);
  out += "\nceiling_bandwidth_bps: ";
  out += decimal(pacing.ceiling_bandwidth_bps);
  out += "\nburst_bytes: ";
  out += decimal(pacing.burst_bytes);
  out += "\nutilization_bps: ";
  out += decimal(pacing.utilization_bps);
  out += "\nrelease_after_monotonic_ms: ";
  out += decimal(pacing.release_after_monotonic_ms);
  out += "\nsync_kind: ";
  out += std::string(to_string(sync.kind));
  out += "\nsync_basis: ";
  out += std::string(to_string(sync.basis));
  out += "\nbindings:";
  out += "\n  collective_generation=";
  out += collective_generation.to_string();
  out += "\n  topology_generation=";
  out += topology_generation.to_string();
  out += "\n  policy_generation=";
  out += policy_generation.to_string();
  out += "\n  capacity_generation=";
  out += capacity_generation.is_some() ? capacity_generation.to_string() : std::string("none");
  out += "\n  congestion_generation=";
  out += congestion_generation.is_some() ? congestion_generation.to_string() : std::string("none");
  out += "\n  coordinator_epoch=";
  out += epoch.to_string();
  out += "\nevidence_used: capacity=";
  out += capacity_evidence_used ? "yes" : "no";
  out += " congestion=";
  out += congestion_evidence_used ? "yes" : "no";
  out += " all_members_live=";
  out += all_members_live ? "yes" : "no";
  out += "\ndecided_monotonic_ms: ";
  out += decimal(decided_monotonic_ms);
  out += "\ndecision_sequence: ";
  out += decimal(decision_sequence.value());
  out += "\nfingerprint: ";
  out += decimal(fingerprint());
  out += "\nreasons:";
  if (reasons.empty()) {
    out += " none";
  }
  for (const Reason& reason : reasons.reasons()) {
    out += "\n  - ";
    out += reason.code;
    out += ": ";
    out += reason.detail;
    if (reason.subject.is_some()) {
      out += " [";
      out += reason.subject.to_string();
      out += "]";
    }
  }
  out += "\n";
  return out;
}

Status validate_decision_request(const DecisionRequest& request) {
  if (request.instance.id.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "request has no collective identity");
  }
  if (request.instance.generation.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "request has no collective generation");
  }
  if (request.instance.attempt.is_none()) {
    return Status(ErrorCode::kValidationAttemptUnset, "request has no attempt identity");
  }
  if (request.has_flow_group_plan) {
    if (!request.has_flow_group_plan) {
      return Status(ErrorCode::kStatePlanMissing, "request has no plan");
    }
    if (request.plan.instance.id != request.instance.id ||
        request.plan.instance.generation != request.instance.generation ||
        request.plan.instance.attempt != request.instance.attempt) {
      return Status(ErrorCode::kEnvelopeIdentityMismatch,
                    "plan instance does not match the request instance");
    }
    if (request.plan.edges.size() > limits::kEdgesMaxPerFlowGroup) {
      return Status(ErrorCode::kValidationEdgeCountOutOfRange, "plan exceeds the edge bound");
    }
  }
  if (request.has_requested_class &&
      static_cast<std::uint8_t>(request.requested_class) > static_cast<std::uint8_t>(TrafficClass::kProbe)) {
    return Status(ErrorCode::kDecodeInvalidEnum, "requested traffic class is not a defined class");
  }
  return Status::ok();
}

}  // namespace ctf
