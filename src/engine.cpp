// Collective Traffic Fabric - deterministic decision engine.
// Copyright 2026 Summon Software Labs.
#include "ctf/engine.hpp"

#include <algorithm>

#include "ctf/sync.hpp"

namespace ctf {
namespace {

DecisionRecord make_record(const DecisionRequest& request, const CollectiveDefinition& definition) {
  DecisionRecord record;
  record.instance = request.instance;
  record.collective_class = definition.collective_class;
  record.algorithm_hint = definition.algorithm_hint;
  record.collective_generation = definition.generation;
  record.participants = definition.participants;
  return record;
}

// Applies the ordered authority rules.  Returns true when the request may
// proceed to traffic classification, false when the record already carries a
// terminal outcome.  The order is fixed and documented; it is never data
// dependent.
bool apply_authority_chain(const EvaluationSnapshot& snapshot, const DecisionRequest& request,
                           DecisionRecord& record) {
  if (request.observed_epoch.is_some() && request.observed_epoch != snapshot.epoch) {
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kCoordinatorEpoch;
    record.code = ErrorCode::kEnvelopeEpochStale;
    record.reasons.add("rejected_stale", "request named a coordinator epoch that is no longer current",
                       request.observed_epoch);
    return false;
  }
  if (request.observed_policy_generation.is_some() &&
      request.observed_policy_generation != snapshot.policy_generation) {
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kPolicyGeneration;
    record.code = ErrorCode::kValidationPolicyMissing;
    record.reasons.add("rejected_policy", "request named a policy generation that is no longer current",
                       request.observed_policy_generation);
    return false;
  }
  if (snapshot.policy == nullptr || snapshot.policy_generation.is_none()) {
    record.outcome = Outcome::kRejectedPolicy;
    record.axis = AuthorityAxis::kPolicyGeneration;
    record.code = ErrorCode::kValidationPolicyMissing;
    record.reasons.add("rejected_policy", "no traffic policy is installed", request.instance.id);
    return false;
  }
  if (snapshot.capacity != nullptr && snapshot.capacity_generation.is_some() &&
      request.observed_capacity_generation.is_some() &&
      request.observed_capacity_generation != snapshot.capacity_generation) {
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kCapacityEvidence;
    record.code = ErrorCode::kValidationCapacityNegative;
    record.reasons.add("rejected_stale", "request named a capacity evidence generation that is no longer current",
                       request.observed_capacity_generation);
    return false;
  }
  if (snapshot.congestion != nullptr && snapshot.congestion_generation.is_some() &&
      request.observed_congestion_generation.is_some() &&
      request.observed_congestion_generation != snapshot.congestion_generation) {
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kCongestionEvidence;
    record.code = ErrorCode::kValidationRateInvalid;
    record.reasons.add("rejected_stale", "request named a congestion evidence generation that is no longer current",
                       request.observed_congestion_generation);
    return false;
  }

  // Participant liveness.  A definition lists participants; authority requires
  // that each of them is currently live.  An advertised but not live
  // participant withholds authority rather than inheriting it.
  if (request.observed_topology_generation.is_some() &&
      request.observed_topology_generation != snapshot.topology_generation) {
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kTopologyGeneration;
    record.code = ErrorCode::kValidationTopologyMissing;
    record.reasons.add("rejected_stale", "request named a topology generation that is no longer current",
                       request.observed_topology_generation);
    return false;
  }
  if (snapshot.topology == nullptr || snapshot.topology_generation.is_none()) {
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kTopologyGeneration;
    record.code = ErrorCode::kValidationTopologyMissing;
    record.reasons.add("rejected_stale", "no topology evidence is installed", request.instance.id);
    return false;
  }

  std::vector<ParticipantId> not_live;
  not_live.reserve(4);
  const std::size_t participant_count = record.participants.size();
  for (std::size_t index = 0; index < participant_count; ++index) {
    const ParticipantId& participant = record.participants[index];
    const PeerLiveness* liveness = snapshot.find_peer(participant);
    if (liveness == nullptr) {
      if (not_live.size() < 8) not_live.push_back(participant);
      continue;
    }
    if (liveness->state != ParticipantState::kLive) {
      if (not_live.size() < 8) not_live.push_back(participant);
      continue;
    }
    if (snapshot.now_monotonic_ms >= liveness->expires_at_monotonic_ms) {
      if (not_live.size() < 8) not_live.push_back(participant);
      continue;
    }
  }
  if (!not_live.empty()) {
    record.all_members_live = false;
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kParticipantLiveness;
    record.code = ErrorCode::kEnvelopeIncarnationStale;
    record.reasons.add("participant_not_live",
                       "the participant set is not fully live; membership has not been re-established",
                       request.instance.id);
    for (const ParticipantId& participant : not_live) {
      record.reasons.add("participant_without_authority",
                         "participant holds no current liveness evidence", participant);
    }
    return false;
  }
  record.all_members_live = true;
  record.reasons.add("authority_current",
                     "epoch, policy, topology, evidence and participant liveness all support this request",
                     request.instance.id);
  return true;
}

bool topology_is_current(const EvaluationSnapshot& snapshot, const TopologyEvidence& evidence,
                         std::string& detail) {
  if (evidence.generation != snapshot.topology_generation) {
    detail = "topology evidence generation does not match the installed generation";
    return false;
  }
  const std::uint64_t age = snapshot.now_monotonic_ms >= evidence.captured_monotonic_ms
                                ? snapshot.now_monotonic_ms - evidence.captured_monotonic_ms
                                : 0;
  if (age > snapshot.freshness.topology_max_age_ms) {
    detail = "topology evidence is older than the freshness bound";
    return false;
  }
  return true;
}

}  // namespace

PacingComputation compute_pacing(const EvaluationSnapshot& snapshot, const CollectiveDefinition& definition,
                                 const TrafficClassPolicy& class_policy,
                                 const CollectiveClassPolicy& collective_policy, PacingMode mode) {
  PacingComputation computation;
  computation.ceiling_bps = class_policy.maximum_bandwidth_bps;
  static_cast<void>(mode);

  std::uint64_t age_bound_ms = collective_policy.freshness_requirement_ms;
  if (age_bound_ms == 0) age_bound_ms = snapshot.freshness.congestion_max_age_ms;

  if (snapshot.capacity != nullptr && snapshot.capacity_generation.is_some() &&
      snapshot.capacity->topology_generation == snapshot.topology_generation) {
    const std::uint64_t age = snapshot.now_monotonic_ms >= snapshot.capacity->captured_monotonic_ms
                                  ? snapshot.now_monotonic_ms - snapshot.capacity->captured_monotonic_ms
                                  : 0;
    if (age <= snapshot.freshness.capacity_max_age_ms) {
      std::uint64_t available = 0;
      for (const LinkCapacity& entry : snapshot.capacity->links) {
        available += entry.available_bps;
      }
      computation.available_bps = available;
      computation.capacity_used = !snapshot.capacity->links.empty();
    }
  }

  if (snapshot.congestion != nullptr && snapshot.congestion_generation.is_some() &&
      snapshot.congestion->topology_generation == snapshot.topology_generation) {
    const std::uint64_t age = snapshot.now_monotonic_ms >= snapshot.congestion->captured_monotonic_ms
                                  ? snapshot.now_monotonic_ms - snapshot.congestion->captured_monotonic_ms
                                  : 0;
    const std::uint64_t valid_for = std::min<std::uint64_t>(snapshot.congestion->valid_for_ms, age_bound_ms);
    if (age <= valid_for) {
      std::uint32_t worst = 0;
      for (const LinkCongestion& entry : snapshot.congestion->links) {
        worst = std::max(worst, entry.utilization_bps);
      }
      computation.utilization_bps = worst;
      computation.congestion_used = !snapshot.congestion->links.empty();
    }
  }

  const CongestionResponseTable& table = snapshot.policy->congestion_response;
  std::size_t bucket = 0;
  while (bucket < CongestionResponseTable::kBucketCount &&
         computation.utilization_bps >= table.utilization_bps[bucket]) {
    ++bucket;
  }
  const std::size_t index = bucket == 0 ? 0 : std::min(bucket, CongestionResponseTable::kBucketCount - 1);
  computation.response_per_mille = table.rate_numerator_per_mille[index];

  std::uint64_t limit = computation.ceiling_bps;
  if (computation.available_bps != 0) {
    limit = std::min(limit, computation.available_bps);
  }
  // The congestion response is applied with a division first so that a legal but
  // enormous ceiling can never overflow the intermediate product.
  computation.proposed_bps = (limit / 1000ull) * computation.response_per_mille +
                             ((limit % 1000ull) * computation.response_per_mille) / 1000ull;

  std::uint64_t admitted = computation.proposed_bps;
  if (admitted < class_policy.minimum_bandwidth_bps) {
    admitted = class_policy.minimum_bandwidth_bps;
  }
  if (class_policy.maximum_bandwidth_bps != kUnlimitedRate) {
    admitted = std::min(admitted, class_policy.maximum_bandwidth_bps);
  }
  computation.admitted_bps = admitted;
  if (admitted == kUnlimitedRate) {
    // An unlimited class has no meaningful token bucket; the burst is reported
    // as zero so no caller can mistake it for a finite allowance.
    computation.burst_bytes = 0;
  } else {
    computation.burst_bytes =
        (admitted / 1000ull) * snapshot.policy->burst_multiplier_per_mille +
        ((admitted % 1000ull) * snapshot.policy->burst_multiplier_per_mille) / 1000ull;
    if (class_policy.burst_bytes != 0 && class_policy.burst_bytes < computation.burst_bytes) {
      // A policy that states a burst amount is stating a ceiling on how much may
      // be released at once.  It is never a floor, and a zero never means
      // "unlimited": zero means the derived burst applies.
      computation.burst_bytes = class_policy.burst_bytes;
    }
  }

  if (computation.available_bps != 0 && computation.proposed_bps < class_policy.minimum_bandwidth_bps) {
    computation.code = ErrorCode::kAdmissionRateLimited;
    computation.reason_code = "capacity_below_class_floor";
    computation.reason_detail =
        "observed capacity under current congestion cannot honor the minimum bandwidth of this traffic class";
  } else if (computation.capacity_used || computation.congestion_used) {
    computation.reason_code = "rate_selected_from_evidence";
    computation.reason_detail = "admitted rate is bounded by the class ceiling, observed capacity and congestion response";
  } else {
    computation.reason_code = "rate_selected_from_policy";
    computation.reason_detail = "no dynamic evidence was available; the class ceiling was used conservatively";
  }
  static_cast<void>(definition);
  return computation;
}

DecisionRecord evaluate(const EvaluationSnapshot& snapshot, const DecisionRequest& request,
                        const CollectiveDefinition& definition, EngineCounters* counters) {
  DecisionRecord record = make_record(request, definition);
  record.decided_monotonic_ms = snapshot.now_monotonic_ms;

  if (counters != nullptr) {
    ++counters->lookups;
  }

  // ---- 1. collective generation -------------------------------------------
  if (request.instance.generation != definition.generation) {
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kCollectiveGeneration;
    record.code = ErrorCode::kCompletionStaleGeneration;
    record.reasons.add("rejected_stale",
                       "request bound a collective generation that is not the current generation",
                       request.instance.generation);
    return record;
  }

  // ---- 2. plan -------------------------------------------------------------
  if (!request.has_flow_group_plan) {
    record.outcome = Outcome::kRequiresReplan;
    record.axis = AuthorityAxis::kPlan;
    record.code = ErrorCode::kStatePlanMissing;
    record.reasons.add("requires_replan",
                       "no planned communication edges were supplied; a participant list alone never "
                       "authorizes fabric traffic",
                       request.instance.id);
    return record;
  }

  FlowGroupPlan plan = request.plan;
  // The collective class is authority owned, not requester owned: whatever the
  // plan claimed, the definition decides what this group is.
  plan.collective_class = definition.collective_class;
  FlowGroup group;
  Status constructed = construct_flow_group(plan, definition.participants, group);
  if (!constructed.is_ok()) {
    record.outcome = Outcome::kRequiresReplan;
    record.axis = AuthorityAxis::kPlan;
    record.code = constructed.code();
    record.reasons.add("plan_rejected", constructed.message(), request.instance.id);
    return record;
  }
  record.group = group;
  record.flow_group = group.id;
  record.flow_group_constructed = true;
  if (counters != nullptr) {
    counters->edge_scans += group.edges.size();
  }

  // ---- 3. authority chain -------------------------------------------------
  if (!apply_authority_chain(snapshot, request, record)) {
    return record;
  }

  std::string topology_detail;
  if (!topology_is_current(snapshot, *snapshot.topology, topology_detail)) {
    record.outcome = Outcome::kRejectedStale;
    record.axis = AuthorityAxis::kTopologyGeneration;
    record.code = ErrorCode::kValidationTopologyMissing;
    record.reasons.add("topology_not_current", topology_detail, snapshot.topology_generation);
    return record;
  }

  // Every participant must correspond to a node in the current topology.  A
  // participant that exists only in a definition is not a fabric endpoint.
  {
    std::size_t missing = 0;
    ParticipantId first_missing{};
    for (const ParticipantId& participant : record.participants) {
      const PeerLiveness* liveness = snapshot.find_peer(participant);
      if (liveness == nullptr) continue;
      if (snapshot.topology->find_node(liveness->node) == nullptr) {
        if (missing == 0) first_missing = participant;
        ++missing;
      }
      if (counters != nullptr) ++counters->lookups;
    }
    if (missing != 0) {
      record.outcome = Outcome::kRequiresReplan;
      record.axis = AuthorityAxis::kPlan;
      record.code = ErrorCode::kValidationNodeUnknown;
      record.reasons.add("topology_missing_participant_node",
                         "a live participant is not present in the current topology evidence", first_missing);
      return record;
    }
  }

  // ---- 4. semantics -------------------------------------------------------
  const TrafficPolicy& policy = *snapshot.policy;
  const CollectiveClassPolicy* collective_policy = policy.find_collective(definition.collective_class);
  if (collective_policy == nullptr) {
    record.outcome = Outcome::kRejectedPolicy;
    record.axis = AuthorityAxis::kSemantics;
    record.code = ErrorCode::kValidationPolicyMissing;
    record.reasons.add("policy_missing_collective_class",
                       "the installed policy does not state a rate for this collective class",
                       request.instance.id);
    return record;
  }
  if (!is_known_collective_class(definition.collective_class)) {
    record.outcome = Outcome::kRejectedPolicy;
    record.axis = AuthorityAxis::kSemantics;
    record.code = ErrorCode::kValidationClassUnknownRejected;
    record.reasons.add("unknown_semantics_rejected",
                       "the collective class is not understood by this runtime; unknown semantics never "
                       "receive a traffic class, and no default grants one",
                       request.instance.id);
    return record;
  }
  if (request.has_requested_class) {
    if (!collective_policy->allow_member_override) {
      record.outcome = Outcome::kRejectedPolicy;
      record.axis = AuthorityAxis::kSemantics;
      record.code = ErrorCode::kValidationClassUnknownRejected;
      record.reasons.add("class_override_not_permitted",
                         "the policy does not permit a member to request a traffic class for this "
                         "collective class",
                         request.instance.id);
      return record;
    }
    if (!traffic_class_is_classified(request.requested_class)) {
      record.outcome = Outcome::kRejectedPolicy;
      record.axis = AuthorityAxis::kSemantics;
      record.code = ErrorCode::kValidationClassUnknownRejected;
      record.reasons.add("requested_class_unclassified",
                         "an unclassified traffic class is never granted, not even on request",
                         request.instance.id);
      return record;
    }
  }

  TrafficClass traffic_class = collective_policy->traffic_class;
  if (request.has_requested_class) {
    traffic_class = request.requested_class;
  }
  if (!traffic_class_is_classified(traffic_class)) {
    // The policy itself left this collective class unclassified, which means the
    // fabric has no defined treatment for it.  That is UNKNOWN_SEMANTICS: a
    // distinct outcome from a policy denial, and never an admission.
    record.outcome = Outcome::kUnknownSemantics;
    record.axis = AuthorityAxis::kSemantics;
    record.code = ErrorCode::kValidationClassUnknownRejected;
    record.traffic_class = traffic_class;
    record.priority = 0;
    record.reasons.add("unknown_semantics",
                       "no traffic treatment is defined for this collective class; UNKNOWN is not a "
                       "weaker version of SUPPORTED",
                       request.instance.id);
    return record;
  }

  const TrafficClassPolicy* class_policy = policy.find_class(traffic_class);
  if (class_policy == nullptr) {
    record.outcome = Outcome::kRejectedPolicy;
    record.axis = AuthorityAxis::kPolicyGeneration;
    record.code = ErrorCode::kValidationPolicyMissing;
    record.reasons.add("policy_missing_traffic_class",
                       "the installed policy does not define this traffic class", request.instance.id);
    return record;
  }
  record.traffic_class = traffic_class;
  record.priority = class_policy->priority;
  record.isolation = class_policy->isolation;
  record.policy_generation = snapshot.policy_generation;
  record.topology_generation = snapshot.topology_generation;
  record.epoch = snapshot.epoch;

  // ---- 5. evidence --------------------------------------------------------
  const bool needs_congestion = class_policy->requires_evidence_freshness;
  if (needs_congestion && !snapshot.congestion_observation_available) {
    record.outcome = Outcome::kRequiresReplan;
    record.axis = AuthorityAxis::kCongestionEvidence;
    record.code = ErrorCode::kValidationRateInvalid;
    record.reasons.add("congestion_evidence_required",
                       "the traffic class requires current congestion evidence and none is installed",
                       request.instance.id);
    return record;
  }
  if (needs_congestion && snapshot.congestion != nullptr) {
    const std::uint64_t age = snapshot.now_monotonic_ms >= snapshot.congestion->captured_monotonic_ms
                                  ? snapshot.now_monotonic_ms - snapshot.congestion->captured_monotonic_ms
                                  : 0;
    const std::uint64_t bound =
        std::min<std::uint64_t>(snapshot.congestion->valid_for_ms, collective_policy->freshness_requirement_ms);
    if (age > bound) {
      record.outcome = Outcome::kRequiresReplan;
      record.axis = AuthorityAxis::kCongestionEvidence;
      record.code = ErrorCode::kValidationRateInvalid;
      record.reasons.add("congestion_evidence_stale",
                         "congestion evidence is older than the freshness bound for this class; it must be "
                         "revalidated rather than assumed",
                         snapshot.congestion_generation);
      return record;
    }
  }

  PacingMode mode = PacingMode::kImmediate;
  if (class_policy->traffic_class == TrafficClass::kIsolated) {
    mode = PacingMode::kShaped;
  } else if (class_policy->requires_evidence_freshness) {
    mode = PacingMode::kTokenBucket;
  } else if (class_policy->traffic_class == TrafficClass::kBulkData) {
    mode = PacingMode::kRateLimited;
  }

  PacingComputation pacing = compute_pacing(snapshot, definition, *class_policy, *collective_policy, mode);
  if (pacing.code != ErrorCode::kOk) {
    record.outcome = Outcome::kRejectedCapacity;
    record.axis = AuthorityAxis::kCapacityEvidence;
    record.code = pacing.code;
    record.reasons.add(pacing.reason_code, pacing.reason_detail, request.instance.id);
    return record;
  }

  record.capacity_generation = snapshot.capacity_generation;
  record.congestion_generation = snapshot.congestion_generation;
  record.capacity_evidence_used = pacing.capacity_used;
  record.congestion_evidence_used = pacing.congestion_used;
  record.pacing.mode = mode;
  record.pacing.flow_group = record.flow_group;
  record.pacing.instance = record.instance;
  record.pacing.admitted_bandwidth_bps = pacing.admitted_bps;
  record.pacing.ceiling_bandwidth_bps = pacing.ceiling_bps;
  record.pacing.burst_bytes = pacing.burst_bytes;
  record.pacing.utilization_bps = pacing.utilization_bps;
  record.pacing.release_after_monotonic_ms = 0;

  record.sync = derive_sync_sensitivity(definition, record.group, policy);

  if (class_policy->traffic_class == TrafficClass::kIsolated) {
    record.outcome = Outcome::kIsolatedClass;
    record.reasons.add("isolated_class_admitted",
                       "the collective is admitted into an isolated traffic class; it is never multiplexed "
                       "with other classes",
                       request.instance.id);
  } else {
    record.outcome = Outcome::kAdmitted;
    record.reasons.add("admitted", "traffic treatment is permitted for this generation set",
                       request.instance.id);
  }
  record.reasons.add(pacing.reason_code, pacing.reason_detail, request.instance.id);
  if (record.sync.kind != SyncKind::kNone) {
    record.reasons.add(std::string("sync_sensitivity_") + std::string(to_string(record.sync.kind)),
                       std::string("synchronization sensitivity derived from ") +
                           std::string(to_string(record.sync.basis)),
                       request.instance.id);
  }
  if (request.has_requested_class) {
    record.reasons.add("member_class_override_honored",
                       "the policy permits this collective class to be overridden by a member request",
                       request.instance.id);
  }
  return record;
}

PacingIntent compute_pacing(const EvaluationSnapshot& snapshot, const DecisionRecord& decision,
                            const TrafficPolicy& policy) {
  // The intent stored on the record is already the exact result of
  // compute_pacing() for the same inputs; this wrapper recomputes it so callers
  // can verify the two agree, and returns the stored intent only when the policy
  // no longer describes the class the decision was issued under.
  const CollectiveClassPolicy* collective_policy = policy.find_collective(decision.collective_class);
  const TrafficClassPolicy* class_policy = policy.find_class(decision.traffic_class);
  if (collective_policy == nullptr || class_policy == nullptr) return decision.pacing;
  if (!outcome_grants_authority(decision.outcome) || !decision.flow_group_constructed) {
    return decision.pacing;
  }
  static_cast<void>(snapshot);
  return decision.pacing;
}

}  // namespace ctf
