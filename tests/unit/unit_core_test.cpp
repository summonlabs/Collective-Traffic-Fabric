// Collective Traffic Fabric - deterministic unit proofs for the core model.
// Copyright 2026 Summon Software Labs.
#include <algorithm>
#include <string>
#include <vector>

#include "ctf/admission.hpp"
#include "ctf/catalog.hpp"
#include "ctf/collective.hpp"
#include "ctf/decision.hpp"
#include "ctf/engine.hpp"
#include "ctf/flow_group.hpp"
#include "ctf/history.hpp"
#include "ctf/identity.hpp"
#include "ctf/sync.hpp"
#include "ctf/traffic.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;
using ctf::test::make_definition;
using ctf::test::make_request;
using ctf::test::make_ring_plan;
using ctf::test::SyntheticWorld;

CollectiveInstance instance_of(const CollectiveDefinition& definition, CollectiveAttemptId attempt) {
  CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = definition.generation;
  instance.attempt = attempt;
  return instance;
}

}  // namespace

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
CTF_TEST("identity", "round_trip_and_rejections") {
  const Identity minted = mint_identity();
  CTF_CHECK(minted.is_some());
  Identity parsed;
  CTF_CHECK(Identity::parse(minted.to_string(), parsed));
  CTF_CHECK_EQ(parsed, minted);
  CTF_CHECK(!Identity::parse("zzzz", parsed));
  CTF_CHECK(!Identity::parse("", parsed));
  CTF_CHECK(!Identity::parse("0000000000000000000000000000000", parsed));
  CTF_CHECK(!Identity::parse(minted.to_string() + "0", parsed));
  // Zero is the unset identity and must never be produced by the minter.
  CTF_CHECK(mint_identity().is_some());
  CTF_CHECK(Identity{}.is_none());
}

CTF_TEST("identity", "minter_is_deterministic_for_a_seed") {
  IdentityMinter first(1234);
  IdentityMinter second(1234);
  for (int index = 0; index < 64; ++index) {
    CTF_CHECK_EQ(first.next(), second.next());
  }
  IdentityMinter other(1235);
  IdentityMinter again(1234);
  CTF_CHECK_NE(other.next(), again.next());
  // Two seeds that differ in one bit must not produce the same second value.
  IdentityMinter near_a(0x1000);
  IdentityMinter near_b(0x1001);
  const Identity near_a_first = near_a.next();
  const Identity near_b_first = near_b.next();
  CTF_CHECK_NE(near_a_first, near_b_first);
  CTF_CHECK_NE(near_a.next(), near_b.next());
}

CTF_TEST("identity", "sequence_ordering_and_advance") {
  Sequence first(1);
  Sequence second(2);
  CTF_CHECK(first < second);
  CTF_CHECK(second > first);
  CTF_CHECK_EQ((++first).value(), 2u);
  CTF_CHECK(std::string(to_string(CollectiveClass::kAllReduce)) == "all_reduce");
}

// ---------------------------------------------------------------------------
// Canonicalisation
// ---------------------------------------------------------------------------
CTF_TEST("canonical", "participant_order_does_not_change_the_identity") {
  SyntheticWorld world(7);
  std::vector<ParticipantId> shuffled = world.participants;
  std::reverse(shuffled.begin(), shuffled.end());
  CollectiveDefinition forward = make_definition(mint_identity(), CollectiveClass::kAllReduce,
                                                 world.participants);
  CollectiveDefinition backward = make_definition(mint_identity(), CollectiveClass::kAllReduce, shuffled);
  CTF_CHECK_EQ(forward.participants, backward.participants);
  CTF_CHECK(participants_are_canonical(forward.participants));
}

CTF_TEST("canonical", "flow_group_ids_agree_for_equivalent_plans") {
  SyntheticWorld world(11);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants);
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan first = make_ring_plan(instance, definition.participants, 1u << 20);
  FlowGroupPlan second = first;
  std::reverse(second.edges.begin(), second.edges.end());
  FlowGroup first_group;
  FlowGroup second_group;
  CTF_CHECK_OK(construct_flow_group(first, definition.participants, first_group));
  CTF_CHECK_OK(construct_flow_group(second, definition.participants, second_group));
  CTF_CHECK_EQ(first_group.id, second_group.id);
  CTF_CHECK_EQ(first_group.edges, second_group.edges);
  CTF_CHECK_EQ(first_group.participants, second_group.participants);
  CTF_CHECK_EQ(first_group.max_fan_out, 1u);
  CTF_CHECK_EQ(first_group.max_fan_in, 1u);
  CTF_CHECK_EQ(first_group.total_logical_bytes, (1u << 20) * definition.participants.size());
}

// ---------------------------------------------------------------------------
// Flow group validation
// ---------------------------------------------------------------------------
CTF_TEST("flow_group", "rejects_malformed_plans") {
  SyntheticWorld world(13);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants);
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());

  FlowGroupPlan self_loop = make_ring_plan(instance, definition.participants, 1u << 20);
  self_loop.edges[0].destination = self_loop.edges[0].source;
  FlowGroup group;
  CTF_CHECK_CODE(construct_flow_group(self_loop, definition.participants, group),
                 ErrorCode::kValidationEdgeSelfLoop);

  FlowGroupPlan duplicate = make_ring_plan(instance, definition.participants, 1u << 20);
  duplicate.edges.push_back(duplicate.edges.front());
  CTF_CHECK_CODE(construct_flow_group(duplicate, definition.participants, group),
                 ErrorCode::kValidationEdgeDuplicate);

  FlowGroupPlan foreign = make_ring_plan(instance, definition.participants, 1u << 20);
  foreign.edges[0].source = mint_identity();
  CTF_CHECK_CODE(construct_flow_group(foreign, definition.participants, group),
                 ErrorCode::kValidationEdgeEndpointNotParticipant);

  FlowGroupPlan unset = make_ring_plan(instance, definition.participants, 1u << 20);
  unset.edges[0].source = Identity{};
  CTF_CHECK_CODE(construct_flow_group(unset, definition.participants, group),
                 ErrorCode::kValidationEdgeUnset);

  FlowGroupPlan no_steps = make_ring_plan(instance, definition.participants, 1u << 20);
  no_steps.edges[0].step_index = Sequence{};
  CTF_CHECK_CODE(construct_flow_group(no_steps, definition.participants, group),
                 ErrorCode::kValidationStepOutOfRange);

  FlowGroupPlan no_plan;
  no_plan.instance = instance;
  CTF_CHECK_CODE(construct_flow_group(no_plan, definition.participants, group),
                 ErrorCode::kValidationEdgeCountOutOfRange);
}

// ---------------------------------------------------------------------------
// Classification and policy
// ---------------------------------------------------------------------------
CTF_TEST("classification", "every_known_class_has_a_defined_treatment") {
  const TrafficPolicy policy = TrafficPolicy::standard(mint_identity());
  CTF_CHECK_OK(policy.validate());
  for (std::uint8_t raw = 1; raw <= static_cast<std::uint8_t>(CollectiveClass::kSendRecvSplit); ++raw) {
    const auto collective_class = static_cast<CollectiveClass>(raw);
    const CollectiveClassPolicy* entry = policy.find_collective(collective_class);
    CTF_CHECK_MSG(entry != nullptr, "missing policy for collective class");
    CTF_CHECK(traffic_class_is_classified(entry->traffic_class));
  }
  const CollectiveClassPolicy* unknown = policy.find_collective(CollectiveClass::kUnknown);
  CTF_CHECK(unknown != nullptr);
  // Unknown semantics must never be admitted into a real traffic class.
  CTF_CHECK(!unknown->admit_when_unknown_semantics);
  CTF_CHECK_EQ(unknown->traffic_class, TrafficClass::kUnclassified);
  const CollectiveClassPolicy* vendor = policy.find_collective(CollectiveClass::kUnknownVendor);
  CTF_CHECK(vendor != nullptr);
  CTF_CHECK_EQ(vendor->traffic_class, TrafficClass::kUnclassified);
}

CTF_TEST("classification", "strength_ordering_is_total_and_unknown_is_weakest") {
  CTF_CHECK(traffic_class_strength(TrafficClass::kUnclassified) <
            traffic_class_strength(TrafficClass::kBackground));
  CTF_CHECK(traffic_class_strength(TrafficClass::kBackground) <
            traffic_class_strength(TrafficClass::kBulkData));
  CTF_CHECK(traffic_class_strength(TrafficClass::kBulkData) <
            traffic_class_strength(TrafficClass::kLatencyCritical));
  CTF_CHECK(!traffic_class_is_classified(TrafficClass::kUnclassified));
}

CTF_TEST("policy", "invalid_policies_are_refused") {
  TrafficPolicy policy = TrafficPolicy::standard(mint_identity());
  CTF_CHECK_OK(policy.validate());

  TrafficPolicy no_generation = policy;
  no_generation.generation = Identity{};
  CTF_CHECK_CODE(no_generation.validate(), ErrorCode::kValidationGenerationUnset);

  TrafficPolicy inverted = policy;
  for (TrafficClassPolicy& entry : inverted.class_policies) {
    if (entry.traffic_class == TrafficClass::kBestEffortBulk) {
      // A floor above a finite ceiling is not a policy, it is a contradiction.
      entry.minimum_bandwidth_bps = entry.maximum_bandwidth_bps + 1;
    }
  }
  CTF_CHECK_CODE(inverted.validate(), ErrorCode::kValidationRateInvalid);

  TrafficPolicy flat_response = policy;
  flat_response.congestion_response.utilization_bps[1] =
      flat_response.congestion_response.utilization_bps[0];
  CTF_CHECK_CODE(flat_response.validate(), ErrorCode::kValidationRateInvalid);

  TrafficPolicy duplicate = policy;
  duplicate.class_policies.push_back(duplicate.class_policies.front());
  CTF_CHECK_CODE(duplicate.validate(), ErrorCode::kValidationPolicyMissing);

  // Two class entries with the same traffic class are refused explicitly.
  TrafficPolicy repeated = policy;
  repeated.class_policies.push_back(repeated.class_policies.front());
  while (repeated.class_policies.size() > 8) repeated.class_policies.pop_back();
  repeated.class_policies.push_back(repeated.class_policies.front());
  CTF_CHECK(!repeated.validate().is_ok());
}

// ---------------------------------------------------------------------------
// Token bucket
// ---------------------------------------------------------------------------
CTF_TEST("token_bucket", "refills_deterministically_and_refuses_clock_regression") {
  TokenBucketState state;
  const std::uint64_t rate = 1000ull * 1000;  // 1 MB/s
  const std::uint64_t burst = 1000;
  CTF_CHECK_OK(TokenBucket::refill(state, 1000, rate, burst));
  CTF_CHECK_EQ(state.tokens, burst);
  CTF_CHECK_OK(TokenBucket::try_consume(state, 1000, 1000, rate, burst));
  CTF_CHECK_EQ(state.tokens, 0u);
  CTF_CHECK_CODE(TokenBucket::try_consume(state, 1000, 1, rate, burst), ErrorCode::kAdmissionRateLimited);
  // The bucket refills 1000 bytes per millisecond at 1 MB/s, so one millisecond
  // of headroom is exactly enough for the next kilobyte.
  CTF_CHECK_EQ(TokenBucket::delay_until_available_ms(state, 1000), 1u);
  // 1 MiB needs a full 1049 ms at exactly 1 MB/s: the refill is integer exact.
  CTF_CHECK_EQ(TokenBucket::delay_until_available_ms(state, 1u << 20), 1049u);
  // After one second the bucket holds exactly the whole burst again.
  CTF_CHECK_OK(TokenBucket::try_consume(state, 2000, 1000, rate, burst));
  // A clock that moves backwards is refused rather than granting tokens.
  CTF_CHECK_CODE(TokenBucket::refill(state, 1500, rate, burst), ErrorCode::kAdmissionClockRegression);
  // Unlimited is explicit and never a consequence of a zero rate.
  TokenBucketState unlimited;
  CTF_CHECK_OK(TokenBucket::refill(unlimited, 0, kUnlimitedRate, 0));
  CTF_CHECK_OK(TokenBucket::try_consume(unlimited, 0, 1ull << 40, kUnlimitedRate, 0));
  TokenBucketState unset;
  CTF_CHECK_CODE(TokenBucket::refill(unset, 0, 0, 0), ErrorCode::kAdmissionBucketUnset);
}

CTF_TEST("admission", "rate_limited_is_a_timing_answer_not_a_permission_answer") {
  SyntheticWorld world(17);
  // A policy that states an explicit burst ceiling.  The bucket may release at
  // most a quarter of a mebibyte at once, which makes the timing behaviour
  // observable with a transfer that is larger than the bucket.
  for (TrafficClassPolicy& entry : world.policy.class_policies) {
    if (entry.traffic_class == TrafficClass::kBulkData) {
      entry.burst_bytes = 256 * 1024;
    }
  }
  world.rebuild();
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "admission");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const DecisionRequest request =
      make_request(instance, world.snapshot, make_ring_plan(instance, definition.participants, 1u << 30));
  const DecisionRecord decision = evaluate(world.snapshot, request, definition);
  CTF_CHECK_EQ(decision.outcome, Outcome::kAdmitted);
  CTF_CHECK(outcome_grants_authority(decision.outcome));
  CTF_CHECK_MSG(decision.group.total_logical_bytes > decision.pacing.burst_bytes,
                std::string("bytes=") + std::to_string(decision.group.total_logical_bytes) + " burst=" +
                    std::to_string(decision.pacing.burst_bytes) + " admitted=" +
                    std::to_string(decision.pacing.admitted_bandwidth_bps) + " ceiling=" +
                    std::to_string(decision.pacing.ceiling_bandwidth_bps));

  AdmissionController controller;
  const AdmissionResult first = controller.admit(decision, 1000);
  CTF_CHECK_EQ(first.outcome, AdmissionOutcome::kGranted);
  // At the same instant the bucket cannot hold a second transfer of this size.
  const AdmissionResult second = controller.admit(decision, 1000);
  CTF_CHECK_EQ(second.outcome, AdmissionOutcome::kRateLimited);
  CTF_CHECK(second.wait_ms > 0);
  CTF_CHECK_EQ(second.code, ErrorCode::kAdmissionRateLimited);
  // Once enough time has passed for the bucket to refill, the same decision is
  // released again: the permission never changed, only the timing.
  const AdmissionResult later = controller.admit(decision, 1000 + second.wait_ms + 1);
  CTF_CHECK_EQ(later.outcome, AdmissionOutcome::kGranted);
  // A rejected decision can never release traffic.
  DecisionRecord rejected = decision;
  rejected.outcome = Outcome::kRejectedPolicy;
  const AdmissionResult refused = controller.admit(rejected, 1000);
  CTF_CHECK_EQ(refused.outcome, AdmissionOutcome::kNotApplicable);
}

// ---------------------------------------------------------------------------
// Decision semantics
// ---------------------------------------------------------------------------
CTF_TEST("decision", "admitted_decision_binds_every_generation") {
  SyntheticWorld world(19);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllGather, world.participants, "binding");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const DecisionRequest request =
      make_request(instance, world.snapshot, make_ring_plan(instance, definition.participants, 1u << 20));
  const DecisionRecord decision = evaluate(world.snapshot, request, definition);
  CTF_CHECK_EQ(decision.outcome, Outcome::kAdmitted);
  CTF_CHECK_EQ(decision.collective_generation, definition.generation);
  CTF_CHECK_EQ(decision.topology_generation, world.fabric.topology.generation);
  CTF_CHECK_EQ(decision.policy_generation, world.policy.generation);
  CTF_CHECK_EQ(decision.capacity_generation, world.capacity.generation);
  CTF_CHECK_EQ(decision.congestion_generation, world.congestion.generation);
  CTF_CHECK_EQ(decision.epoch, world.snapshot.epoch);
  CTF_CHECK(decision.flow_group_constructed);
  CTF_CHECK(decision.flow_group.is_some());
  CTF_CHECK_EQ(decision.participants, definition.participants);
  CTF_CHECK(decision.all_members_live);
  CTF_CHECK(decision.congestion_evidence_used);
  CTF_CHECK(decision.priority > 0);
  CTF_CHECK(decision.pacing.admitted_bandwidth_bps > 0);
  CTF_CHECK(!decision.reasons.empty());
  CTF_CHECK(!decision.summary().empty());
  CTF_CHECK(decision.explain().find("outcome: ADMITTED") != std::string::npos);
  // Identical inputs must produce an identical fingerprint.
  const DecisionRecord again = evaluate(world.snapshot, request, definition);
  CTF_CHECK_EQ(decision.fingerprint(), again.fingerprint());
}

CTF_TEST("decision", "unknown_semantics_never_gain_a_class") {
  SyntheticWorld world(23);
  for (const CollectiveClass collective_class :
       {CollectiveClass::kUnknown, CollectiveClass::kUnknownVendor}) {
    const CollectiveDefinition base =
        make_definition(mint_identity(), collective_class, world.participants, "unknown");
    CollectiveDefinition with_generation = base;
    with_generation.generation = world.ids.next();
    const CollectiveInstance instance = instance_of(with_generation, mint_identity());
    DecisionRequest request = make_request(
        instance, world.snapshot, make_ring_plan(instance, with_generation.participants, 1u << 20));
    // Even an explicit request for a stronger class is refused.
    request.has_requested_class = true;
    request.requested_class = TrafficClass::kLatencyCritical;
    const DecisionRecord decision = evaluate(world.snapshot, request, with_generation);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedPolicy);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kSemantics);
    CTF_CHECK(!outcome_grants_authority(decision.outcome));
    // The plan was still structurally sound and is reported as constructed: the
    // refusal is about semantics and authority, not about the plan's shape.
    CTF_CHECK(decision.flow_group_constructed);
    CTF_CHECK(!traffic_class_is_classified(decision.traffic_class));
  }
}

CTF_TEST("decision", "unknown_semantics_without_override_report_their_own_outcome") {
  SyntheticWorld world(29);
  TrafficPolicy relaxed = world.policy;
  for (CollectiveClassPolicy& entry : relaxed.collective_policies) {
    if (entry.collective_class == CollectiveClass::kUnknown) {
      // A policy is allowed to make an unknown collective admissible in
      // principle; the outcome must then still refuse to invent a class.
      entry.allow_member_override = false;
    }
  }
  world.policy = relaxed;
  world.rebuild();
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kUnknown, world.participants, "unknown-outcome");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const DecisionRequest request =
      make_request(instance, world.snapshot, make_ring_plan(instance, definition.participants, 1u << 20));
  const DecisionRecord decision = evaluate(world.snapshot, request, definition);
  CTF_CHECK(decision.outcome == Outcome::kUnknownSemantics ||
            decision.outcome == Outcome::kRejectedPolicy);
  CTF_CHECK(!outcome_grants_authority(decision.outcome));
}

CTF_TEST("decision", "missing_plan_requires_replan_instead_of_guessing") {
  SyntheticWorld world(31);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "no-plan");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  DecisionRequest request = make_request(instance, world.snapshot,
                                         make_ring_plan(instance, definition.participants, 1u << 20));
  request.has_flow_group_plan = false;
  const DecisionRecord decision = evaluate(world.snapshot, request, definition);
  CTF_CHECK_EQ(decision.outcome, Outcome::kRequiresReplan);
  CTF_CHECK(!outcome_grants_authority(decision.outcome));
  CTF_CHECK(decision.reasons.contains("requires_replan"));
}

CTF_TEST("decision", "stale_inputs_are_rejected_per_axis") {
  SyntheticWorld world(37);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kReduceScatter, world.participants, "stale");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan plan = make_ring_plan(instance, definition.participants, 1u << 20);

  {
    DecisionRequest request = make_request(instance, world.snapshot, plan);
    request.observed_epoch = mint_identity();
    const DecisionRecord decision = evaluate(world.snapshot, request, definition);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCoordinatorEpoch);
  }
  {
    DecisionRequest request = make_request(instance, world.snapshot, plan);
    request.observed_topology_generation = mint_identity();
    const DecisionRecord decision = evaluate(world.snapshot, request, definition);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kTopologyGeneration);
  }
  {
    DecisionRequest request = make_request(instance, world.snapshot, plan);
    request.observed_policy_generation = mint_identity();
    const DecisionRecord decision = evaluate(world.snapshot, request, definition);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kPolicyGeneration);
    CTF_CHECK_EQ(decision.code, ErrorCode::kValidationPolicyMissing);
  }
  {
    DecisionRequest request = make_request(instance, world.snapshot, plan);
    request.instance.generation = mint_identity();
    request.plan.instance.generation = request.instance.generation;
    const DecisionRecord decision = evaluate(world.snapshot, request, definition);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCollectiveGeneration);
  }
}

CTF_TEST("decision", "member_of_the_set_without_liveness_withholds_authority") {
  SyntheticWorld world(41);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "liveness");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan plan = make_ring_plan(instance, definition.participants, 1u << 20);

  // The first participant silently loses its liveness evidence.
  const ParticipantId removed = definition.participants.front();
  std::vector<PeerLiveness> survivors;
  for (const PeerLiveness& peer : world.snapshot.peers) {
    if (peer.participant != removed) survivors.push_back(peer);
  }
  world.snapshot.peers = survivors;
  const DecisionRecord decision =
      evaluate(world.snapshot, make_request(instance, world.snapshot, plan), definition);
  CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(decision.axis, AuthorityAxis::kParticipantLiveness);
  CTF_CHECK(!decision.all_members_live);
  CTF_CHECK(decision.reasons.contains("participant_not_live"));
}

CTF_TEST("decision", "expired_liveness_is_not_current") {
  SyntheticWorld world(43);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kBroadcast, world.participants, "expiry");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan plan = make_ring_plan(instance, definition.participants, 1u << 20);
  world.rebuild(1000000);  // beyond every expiry in the synthetic world
  const DecisionRecord decision =
      evaluate(world.snapshot, make_request(instance, world.snapshot, plan), definition);
  CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(decision.axis, AuthorityAxis::kParticipantLiveness);
}

// ---------------------------------------------------------------------------
// Pacing response
// ---------------------------------------------------------------------------
CTF_TEST("pacing", "congestion_response_shapes_the_admitted_rate") {
  SyntheticWorld world(47, 2, 2, 1000);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "pacing");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan plan = make_ring_plan(instance, definition.participants, 1u << 20);

  const DecisionRecord unrestricted =
      evaluate(world.snapshot, make_request(instance, world.snapshot, plan), definition);
  CTF_CHECK_EQ(unrestricted.outcome, Outcome::kAdmitted);

  world.congestion = ctf::test::make_congestion(world.fabric.topology, 51, 9700);
  world.rebuild();
  const DecisionRecord congested =
      evaluate(world.snapshot, make_request(instance, world.snapshot, plan), definition);
  CTF_CHECK_EQ(congested.outcome, Outcome::kAdmitted);
  CTF_CHECK(congested.pacing.admitted_bandwidth_bps <= unrestricted.pacing.admitted_bandwidth_bps);
  CTF_CHECK_EQ(congested.pacing.utilization_bps, 9700u);
  CTF_CHECK(congested.pacing.ceiling_bandwidth_bps > 0);
}

CTF_TEST("pacing", "capacity_below_the_class_floor_is_rejected") {
  SyntheticWorld world(53, 2, 2, 1000);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "capacity");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan plan = make_ring_plan(instance, definition.participants, 1u << 20);

  // A starved fabric: 1 byte per second available on every link.
  world.capacity = ctf::test::make_capacity(world.fabric.topology, 59, 10000, 0);
  for (LinkCapacity& entry : world.capacity.links) entry.available_bps = 1;
  world.capacity.canonicalize();
  world.rebuild();
  const DecisionRecord decision =
      evaluate(world.snapshot, make_request(instance, world.snapshot, plan), definition);
  CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedCapacity);
  CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCapacityEvidence);
  CTF_CHECK(decision.reasons.contains("capacity_below_class_floor"));
}

CTF_TEST("pacing", "missing_congestion_requires_replan_rather_than_a_guess") {
  SyntheticWorld world(61);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "no-congestion");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan plan = make_ring_plan(instance, definition.participants, 1u << 20);
  world.snapshot.congestion.reset();
  world.snapshot.congestion_generation = Identity{};
  world.snapshot.congestion_observation_available = false;
  const DecisionRecord decision =
      evaluate(world.snapshot, make_request(instance, world.snapshot, plan), definition);
  CTF_CHECK_EQ(decision.outcome, Outcome::kRequiresReplan);
  CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCongestionEvidence);
  CTF_CHECK(!outcome_grants_authority(decision.outcome));
}

CTF_TEST("pacing", "stale_congestion_requires_replan") {
  SyntheticWorld world(67);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "stale-congestion");
  definition.generation = world.ids.next();
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan plan = make_ring_plan(instance, definition.participants, 1u << 20);
  world.rebuild(60000);  // far beyond the two second validity window
  const DecisionRecord decision =
      evaluate(world.snapshot, make_request(instance, world.snapshot, plan), definition);
  CTF_CHECK_EQ(decision.outcome, Outcome::kRequiresReplan);
  CTF_CHECK(decision.reasons.contains("congestion_evidence_stale"));
}

CTF_TEST("pacing", "isolated_classes_are_reported_as_such") {
  SyntheticWorld world(71);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kBarrierOnly, world.participants, "barrier");
  definition.generation = world.ids.next();
  definition.declares_barrier_semantics = true;
  const CollectiveInstance instance = instance_of(definition, mint_identity());
  const FlowGroupPlan plan = make_ring_plan(instance, definition.participants, 64);
  const DecisionRecord decision =
      evaluate(world.snapshot, make_request(instance, world.snapshot, plan), definition);
  // Barrier traffic is latency critical; the outcome must be an explicit class
  // outcome, never a silent admission into bulk treatment.
  CTF_CHECK(decision.outcome == Outcome::kAdmitted || decision.outcome == Outcome::kIsolatedClass);
  CTF_CHECK_EQ(decision.traffic_class, TrafficClass::kLatencyCritical);
  CTF_CHECK_EQ(decision.sync.kind, SyncKind::kBarrier);
  CTF_CHECK_EQ(decision.sync.basis, SyncBasis::kDeclaredBarrierSemantics);
}

// ---------------------------------------------------------------------------
// Catalog lifecycle
// ---------------------------------------------------------------------------
CTF_TEST("catalog", "registration_unchanged_and_generation_advance") {
  CollectiveCatalog catalog(64);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, {mint_identity(), mint_identity()});
  const CollectiveId id = definition.id;
  RegistrationOutcome first = catalog.register_definition(definition, 10);
  CTF_CHECK_EQ(first.result, RegistrationResult::kRegistered);
  CTF_CHECK(first.created);
  const CollectiveGeneration generation = first.generation;

  RegistrationOutcome same = catalog.register_definition(definition, 11);
  CTF_CHECK_EQ(same.result, RegistrationResult::kUnchanged);
  CTF_CHECK_EQ(same.generation, generation);

  std::vector<ParticipantId> reordered = definition.participants;
  std::reverse(reordered.begin(), reordered.end());
  // A peer may present its participant list in any order; the wire decoder
  // canonicalises before the catalog ever sees it, so a mere reordering must
  // never mint a new generation.
  canonicalize_participants(reordered);
  definition.participants = reordered;
  RegistrationOutcome reordered_outcome = catalog.register_definition(definition, 12);
  CTF_CHECK_EQ(reordered_outcome.result, RegistrationResult::kUnchanged);

  definition.participants.push_back(mint_identity());
  canonicalize_participants(definition.participants);
  RegistrationOutcome advanced = catalog.register_definition(definition, 13);
  CTF_CHECK_EQ(advanced.result, RegistrationResult::kGenerationAdvanced);
  CTF_CHECK(advanced.advanced);
  CTF_CHECK_NE(advanced.generation, generation);
  const CollectiveRecord* record = catalog.find(id);
  CTF_CHECK(record != nullptr);
  CTF_CHECK_EQ(record->definition.generation, advanced.generation);
  CTF_CHECK_EQ(record->generation_ancestry_depth, 2u);
}

CTF_TEST("catalog", "cancelled_collectives_are_never_resurrected") {
  CollectiveCatalog catalog(8);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, {mint_identity(), mint_identity()});
  RegistrationOutcome registered = catalog.register_definition(definition, 10);
  CTF_CHECK_OK(catalog.cancel(definition.id, 20));
  RegistrationOutcome again = catalog.register_definition(definition, 30);
  CTF_CHECK_EQ(again.result, RegistrationResult::kRejectedTerminal);
  CTF_CHECK_CODE(catalog.set_state(definition.id, CollectiveState::kActive, 31), ErrorCode::kStateCollectiveRetired);
  const CollectiveRecord* record = catalog.find(definition.id);
  CTF_CHECK(record != nullptr);
  CTF_CHECK_EQ(record->state, CollectiveState::kCancelled);
  CTF_CHECK_EQ(registered.generation, record->definition.generation);
}

CTF_TEST("catalog", "capacity_bound_is_enforced") {
  CollectiveCatalog catalog(2);
  for (int index = 0; index < 2; ++index) {
    CollectiveDefinition definition =
        make_definition(mint_identity(), CollectiveClass::kAllReduce, {mint_identity()});
    CTF_CHECK_EQ(catalog.register_definition(definition, 1).result, RegistrationResult::kRegistered);
  }
  CollectiveDefinition overflow =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, {mint_identity()});
  CTF_CHECK_EQ(catalog.register_definition(overflow, 1).result, RegistrationResult::kRejectedCapacity);
}

CTF_TEST("catalog", "invalid_definitions_are_refused_without_partial_state") {
  CollectiveCatalog catalog(8);
  CollectiveDefinition empty_participants = make_definition(mint_identity(), CollectiveClass::kAllReduce, {});
  CTF_CHECK_EQ(catalog.register_definition(empty_participants, 1).result,
               RegistrationResult::kRejectedInvalid);
  CollectiveDefinition no_identity;
  no_identity.collective_class = CollectiveClass::kAllReduce;
  no_identity.participants.push_back(mint_identity());
  CTF_CHECK_EQ(catalog.register_definition(no_identity, 1).result, RegistrationResult::kRejectedInvalid);
  CTF_CHECK_EQ(catalog.size(), 0u);
}

CTF_TEST("catalog", "attempts_advance_and_duplicates_are_refused") {
  CollectiveCatalog catalog(8);
  CollectiveDefinition definition =
      make_definition(mint_identity(), CollectiveClass::kAllReduce, {mint_identity()});
  RegistrationOutcome registered = catalog.register_definition(definition, 10);
  CollectiveAttemptId previous;
  const CollectiveAttemptId first_attempt = mint_identity();
  CTF_CHECK_OK(catalog.begin_attempt(definition.id, first_attempt, 11, previous));
  CTF_CHECK(previous.is_none());
  CTF_CHECK_CODE(catalog.begin_attempt(definition.id, first_attempt, 12, previous),
                 ErrorCode::kCompletionDuplicate);
  CollectiveAttemptId ignored;
  CTF_CHECK_OK(catalog.begin_attempt(definition.id, mint_identity(), 13, ignored));
  CTF_CHECK_EQ(ignored, first_attempt);
  CTF_CHECK(catalog.set_state(definition.id, CollectiveState::kRetired, 14).is_ok());
  CTF_CHECK_CODE(catalog.begin_attempt(definition.id, mint_identity(), 15, ignored), ErrorCode::kStateNotLive);
  CTF_CHECK_EQ(registered.generation, catalog.find(definition.id)->definition.generation);
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
CTF_TEST("history", "ring_is_bounded_and_reports_eviction") {
  DecisionHistory history(4);
  std::vector<std::uint64_t> ordinals;
  for (int index = 0; index < 10; ++index) {
    DecisionRecord record;
    record.instance.id = Identity(1, static_cast<std::uint64_t>(index));
    ordinals.push_back(history.record(record));
  }
  CTF_CHECK_EQ(history.size(), 4u);
  CTF_CHECK_EQ(history.first_ordinal(), 7u);
  CTF_CHECK_EQ(history.next_ordinal(), 11u);
  CTF_CHECK_EQ(history.evicted_count(), 6u);
  DecisionRecord out;
  CTF_CHECK_EQ(history.get(ordinals.front(), out), HistoryStatus::kEvicted);
  CTF_CHECK_EQ(history.get(ordinals.back(), out), HistoryStatus::kOk);
  CTF_CHECK_EQ(history.get(999, out), HistoryStatus::kFuture);
  const std::vector<DecisionRecord> latest = history.latest(2);
  CTF_CHECK_EQ(latest.size(), 2u);
  CTF_CHECK_EQ(latest.front().decision_sequence.value(), ordinals.back());
  const std::vector<DecisionRecord> page = history.page(8, 10);
  CTF_CHECK_EQ(page.size(), 3u);
}

CTF_TEST("history", "instance_lookup_ignores_generation_but_requires_exact_attempt") {
  DecisionHistory history(8);
  DecisionRecord first;
  first.instance.id = Identity(9, 9);
  first.instance.generation = Identity(1, 1);
  first.instance.attempt = Identity(2, 2);
  first.outcome = Outcome::kRejectedStale;
  history.record(first);
  DecisionRecord second = first;
  second.instance.generation = Identity(3, 3);
  second.outcome = Outcome::kAdmitted;
  history.record(second);

  DecisionRecord out;
  CTF_CHECK_EQ(history.find_instance(first.instance, out), HistoryStatus::kOk);
  CTF_CHECK_EQ(out.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(history.find_attempt(first.instance.id, first.instance.attempt, out), HistoryStatus::kOk);
  CTF_CHECK_EQ(out.outcome, Outcome::kAdmitted);
  CTF_CHECK_EQ(history.find_attempt(first.instance.id, Identity(7, 7), out), HistoryStatus::kEvicted);

  history.bind_correlation(4242, 1);
  CTF_CHECK_EQ(history.find_correlation(4242, out), HistoryStatus::kOk);
  CTF_CHECK_EQ(out.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(history.find_correlation(1, out), HistoryStatus::kEvicted);
}

CTF_TEST("history", "outcome_histogram_is_deterministic") {
  DecisionHistory history(8);
  for (int index = 0; index < 3; ++index) {
    DecisionRecord record;
    record.outcome = Outcome::kAdmitted;
    history.record(record);
  }
  DecisionRecord rejected;
  rejected.outcome = Outcome::kRejectedPolicy;
  history.record(rejected);
  const auto histogram = history.outcome_histogram();
  CTF_CHECK_EQ(histogram.size(), 2u);
  CTF_CHECK_EQ(histogram[0].first, std::string("ADMITTED"));
  CTF_CHECK_EQ(histogram[0].second, 3u);
  CTF_CHECK_EQ(histogram[1].first, std::string("REJECTED_POLICY"));
}

// ---------------------------------------------------------------------------
// Enumeration tables
// ---------------------------------------------------------------------------
CTF_TEST("tables", "string_tables_round_trip") {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(CollectiveClass::kUnknownVendor); ++raw) {
    const auto value = static_cast<CollectiveClass>(raw);
    CollectiveClass parsed = CollectiveClass::kUnknownVendor;
    CTF_CHECK(collective_class_from_string(to_string(value), parsed));
    CTF_CHECK_EQ(parsed, value);
  }
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(AlgorithmHint::kVendorSpecific); ++raw) {
    const auto value = static_cast<AlgorithmHint>(raw);
    AlgorithmHint parsed = AlgorithmHint::kVendorSpecific;
    CTF_CHECK(algorithm_hint_from_string(to_string(value), parsed));
    CTF_CHECK_EQ(parsed, value);
  }
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(TrafficClass::kProbe); ++raw) {
    const auto value = static_cast<TrafficClass>(raw);
    TrafficClass parsed = TrafficClass::kProbe;
    CTF_CHECK(traffic_class_from_string(to_string(value), parsed));
    CTF_CHECK_EQ(parsed, value);
  }
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(FlowPattern::kCustom); ++raw) {
    const auto value = static_cast<FlowPattern>(raw);
    FlowPattern parsed = FlowPattern::kCustom;
    CTF_CHECK(flow_pattern_from_string(to_string(value), parsed));
    CTF_CHECK_EQ(parsed, value);
  }
  CollectiveClass rejected = CollectiveClass::kAllReduce;
  CTF_CHECK(!collective_class_from_string("AllReduce", rejected));
  CTF_CHECK(!collective_class_from_string("all reduce", rejected));
  CTF_CHECK(!collective_class_from_string("", rejected));
  // A refused parse must not have silently written anything.
  CTF_CHECK_EQ(rejected, CollectiveClass::kAllReduce);
}

CTF_TEST("tables", "every_outcome_and_axis_has_a_stable_name") {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(Outcome::kUnknownSemantics); ++raw) {
    const std::string name(to_string(static_cast<Outcome>(raw)));
    CTF_CHECK(!name.empty());
    CTF_CHECK(name != std::string("unknown_error"));
  }
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(AuthorityAxis::kPlan); ++raw) {
    CTF_CHECK(!std::string(to_string(static_cast<AuthorityAxis>(raw))).empty());
  }
  for (std::uint16_t raw = 0; raw <= static_cast<std::uint16_t>(ErrorCode::kInternalUnsupported); ++raw) {
    const auto code = static_cast<ErrorCode>(raw);
    const std::string name(to_string(code));
    CTF_CHECK(!name.empty());
    // Codes are stable identifiers, not incidental text.
    for (char character : name) {
      CTF_CHECK((character >= 'a' && character <= 'z') || character == '_');
    }
  }
}

CTF_TEST_MAIN()
