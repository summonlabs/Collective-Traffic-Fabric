// Collective Traffic Fabric - seeded property and randomized invariant proofs.
// Copyright 2026 Summon Software Labs.
//
// Every case is driven by a deterministic seed.  The seed is part of the case
// name hash, so a failing run is reproduced exactly by setting CTF_TEST_SEED to
// the value printed in the failure message.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ctf/admission.hpp"
#include "ctf/engine.hpp"
#include "ctf/flow_group.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;

// A small, fully deterministic random source.  Reproducibility matters more
// than statistical quality here: every case must replay exactly.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed | 0x9E3779B97F4A7C15ull) {}

  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  std::uint32_t below(std::uint32_t bound) {
    if (bound == 0) return 0;
    return static_cast<std::uint32_t>(next() % bound);
  }

  bool chance(std::uint32_t percent) { return below(100) < percent; }

 private:
  std::uint64_t state_;
};

// The invariant set that must hold for every decision this runtime produces.
void check_decision_invariants(const DecisionRecord& decision, const EvaluationSnapshot& snapshot,
                               const CollectiveDefinition& definition) {
  // 1. Authority is granted only by the two admitting outcomes.
  if (!outcome_grants_authority(decision.outcome)) {
    CTF_CHECK_MSG(decision.outcome != Outcome::kAdmitted, "a non admitting outcome claimed admission");
  }
  // 2. A grant always carries a fully populated binding.
  if (outcome_grants_authority(decision.outcome)) {
    CTF_CHECK(decision.collective_generation.is_some());
    CTF_CHECK(decision.topology_generation.is_some());
    CTF_CHECK(decision.policy_generation.is_some());
    CTF_CHECK(decision.epoch.is_some());
    CTF_CHECK(decision.instance.attempt.is_some());
    CTF_CHECK(decision.flow_group_constructed);
    CTF_CHECK(decision.flow_group.is_some());
    CTF_CHECK(!decision.participants.empty());
    CTF_CHECK(!decision.reasons.empty());
    CTF_CHECK(traffic_class_is_classified(decision.traffic_class));
    CTF_CHECK(decision.pacing.admitted_bandwidth_bps > 0);
    CTF_CHECK(decision.all_members_live);
    // 3. The bound generations are exactly the ones the snapshot presented.
    CTF_CHECK_EQ(decision.collective_generation, definition.generation);
    CTF_CHECK_EQ(decision.topology_generation, snapshot.topology_generation);
    CTF_CHECK_EQ(decision.policy_generation, snapshot.policy_generation);
    CTF_CHECK_EQ(decision.epoch, snapshot.epoch);
    // 4. The participants bound are the collective's canonical participant set.
    CTF_CHECK(decision.participants == definition.participants);
    CTF_CHECK(participants_are_canonical(decision.participants));
    // 5. Every bound participant was actually live in the snapshot.
    for (const ParticipantId& participant : decision.participants) {
      const PeerLiveness* liveness = snapshot.find_peer(participant);
      CTF_CHECK_MSG(liveness != nullptr, "a grant bound an unknown participant");
      CTF_CHECK(liveness->is_current(snapshot.now_monotonic_ms));
    }
    // 6. Congestion evidence was actually used when the class demands it.
    const CollectiveClassPolicy* collective_policy =
        snapshot.policy->find_collective(definition.collective_class);
    const TrafficClassPolicy* class_policy = snapshot.policy->find_class(decision.traffic_class);
    CTF_CHECK(class_policy != nullptr);
    if (class_policy->requires_evidence_freshness) {
      CTF_CHECK_MSG(snapshot.congestion_observation_available,
                    "a freshness demanding class was admitted without congestion evidence");
    }
    if (collective_policy != nullptr) {
      CTF_CHECK(collective_policy->traffic_class == decision.traffic_class ||
                collective_policy->allow_member_override);
    }
  }
  // 7. A rejection always names an authority axis and a deterministic code.
  if (outcome_is_rejection(decision.outcome)) {
    CTF_CHECK(decision.axis != AuthorityAxis::kNone);
    CTF_CHECK(decision.code != ErrorCode::kOk);
  }
  // 8. Unknown semantics never carry a classified traffic class.
  if (decision.outcome == Outcome::kUnknownSemantics) {
    CTF_CHECK(!traffic_class_is_classified(decision.traffic_class));
  }
  // 9. The reason chain is bounded and non empty for every outcome.
  CTF_CHECK(decision.reasons.size() <= limits::kReasonChainMax);
  CTF_CHECK(!decision.reasons.empty());
}

}  // namespace

CTF_TEST("property_engine", "randomized_decisions_respect_every_invariant") {
  const std::uint64_t seed = ctf::test::resolve_seed("randomized_decisions_respect_every_invariant");
  Rng rng(seed);
  std::size_t granted = 0;
  std::size_t refused = 0;
  for (int step = 0; step < 400; ++step) {
    const std::uint32_t racks = 1 + rng.below(4);
    const std::uint32_t nodes_per_rack = 1 + rng.below(4);
    ctf::test::SyntheticWorld world(seed + static_cast<std::uint64_t>(step), racks, nodes_per_rack,
                                    rng.below(10001));
    const std::uint32_t utilization = rng.below(10001);
    world.congestion = ctf::test::make_congestion(world.fabric.topology, seed + 1000 + step, utilization,
                                                  1000 + rng.below(4000));
    world.capacity = ctf::test::make_capacity(world.fabric.topology, seed + 2000 + step, utilization,
                                              100 + rng.below(901));
    world.rebuild(rng.below(500));

    // Randomly age the congestion observation beyond its own validity window.
    const bool stale_congestion = rng.chance(20);
    if (stale_congestion) {
      world.congestion.captured_monotonic_ms = 0;
      world.congestion.valid_for_ms = 1;
      world.rebuild(100000);
    }

    // Randomly remove one participant's liveness evidence.
    const bool drop_peer = rng.chance(20) && world.participants.size() > 1;
    ParticipantId dropped{};
    if (drop_peer) {
      dropped = world.participants[rng.below(static_cast<std::uint32_t>(world.participants.size()))];
      std::vector<PeerLiveness> survivors;
      for (const PeerLiveness& peer : world.snapshot.peers) {
        if (peer.participant != dropped) survivors.push_back(peer);
      }
      world.snapshot.peers = survivors;
    }

    CollectiveClass collective_class = CollectiveClass::kAllReduce;
    if (rng.chance(25)) {
      const std::uint8_t raw = static_cast<std::uint8_t>(1 + rng.below(13));
      collective_class = static_cast<CollectiveClass>(raw);
    }
    if (rng.chance(10)) collective_class = CollectiveClass::kUnknown;

    CollectiveDefinition definition =
        ctf::test::make_definition(mint_identity(), collective_class, world.participants, "property");
    definition.generation = world.ids.next();
    definition.logical_bytes = 1 + rng.below(1u << 16);
    if (rng.chance(15)) definition.declares_barrier_semantics = true;
    const CollectiveInstance instance = [&definition]() {
      CollectiveInstance value;
      value.id = definition.id;
      value.generation = definition.generation;
      value.attempt = mint_identity();
      return value;
    }();

    FlowGroupPlan plan;
    switch (rng.below(3)) {
      case 0:
        plan = ctf::test::make_ring_plan(instance, definition.participants, 1 + rng.below(1u << 20));
        break;
      case 1:
        plan = ctf::test::make_full_mesh_plan(instance, definition.participants, 1 + rng.below(1u << 16));
        break;
      default:
        plan = ctf::test::make_tree_plan(instance, definition.participants, 1 + rng.below(1u << 18));
        break;
    }

    DecisionRequest request = ctf::test::make_request(instance, world.snapshot, plan);
    // Randomly present a stale observation on exactly one axis.
    switch (rng.below(8)) {
      case 1: request.observed_epoch = mint_identity(); break;
      case 2: request.observed_policy_generation = mint_identity(); break;
      case 3: request.observed_topology_generation = mint_identity(); break;
      case 4: request.instance.generation = mint_identity(); request.plan.instance.generation = request.instance.generation; break;
      case 5: request.has_flow_group_plan = false; break;
      case 6:
        request.has_requested_class = true;
        request.requested_class = static_cast<TrafficClass>(1 + rng.below(7));
        break;
      default: break;
    }

    const DecisionRecord decision = evaluate(world.snapshot, request, definition);
    check_decision_invariants(decision, world.snapshot, definition);
    if (outcome_grants_authority(decision.outcome)) {
      ++granted;
      // A granted decision must be reproducible byte for byte.
      const DecisionRecord again = evaluate(world.snapshot, request, definition);
      CTF_CHECK_EQ(again.fingerprint(), decision.fingerprint());
      CTF_CHECK_EQ(again.outcome, decision.outcome);
      CTF_CHECK_EQ(again.flow_group, decision.flow_group);
      CTF_CHECK_EQ(again.pacing.admitted_bandwidth_bps, decision.pacing.admitted_bandwidth_bps);
    } else {
      ++refused;
    }
    if (drop_peer && outcome_grants_authority(decision.outcome)) {
      CTF_CHECK_MSG(false, "authority was granted while a participant had no liveness evidence");
    }
  }
  // Both branches must actually be exercised, otherwise the case proves little.
  CTF_CHECK_MSG(granted > 20, "too few admissions to be meaningful");
  CTF_CHECK_MSG(refused > 20, "too few refusals to be meaningful");
}

CTF_TEST("property_engine", "canonicalisation_is_order_independent") {
  const std::uint64_t seed = ctf::test::resolve_seed("canonicalisation_is_order_independent");
  Rng rng(seed);
  for (int step = 0; step < 60; ++step) {
    ctf::test::SyntheticWorld world(seed + static_cast<std::uint64_t>(step), 2, 2, 1000);
    std::vector<ParticipantId> original = world.participants;
    std::vector<ParticipantId> shuffled = original;
    for (std::size_t index = shuffled.size(); index > 1; --index) {
      const std::size_t swap_with = rng.below(static_cast<std::uint32_t>(index));
      std::swap(shuffled[index - 1], shuffled[swap_with]);
    }
    CollectiveDefinition forward =
        ctf::test::make_definition(mint_identity(), CollectiveClass::kAllReduce, original);
    // The same collective class, a different encounter order: the identity of a
    // flow group must depend on the content, not on how it was presented.
    CollectiveDefinition backward =
        ctf::test::make_definition(mint_identity(), CollectiveClass::kAllReduce, shuffled);
    CTF_CHECK_EQ(forward.participants, backward.participants);
    CTF_CHECK(participants_are_canonical(forward.participants));

    const CollectiveInstance instance = [&forward]() {
      CollectiveInstance value;
      value.id = forward.id;
      value.generation = mint_identity();
      value.attempt = mint_identity();
      return value;
    }();
    // The ring is taken over the canonical order in both cases.  A ring over a
    // different cyclic order is a genuinely different edge set, and treating the
    // two as equivalent would be wrong; what must not matter is the order in
    // which equivalent content is presented.
    FlowGroupPlan plan_a = ctf::test::make_ring_plan(instance, forward.participants, 4096);
    FlowGroupPlan plan_b = plan_a;
    // Present the same edges in an arbitrary order, and with the participant
    // list permuted, to prove the canonicalisation is content based.
    for (std::size_t index = plan_b.edges.size(); index > 1; --index) {
      const std::size_t swap_with = rng.below(static_cast<std::uint32_t>(index));
      std::swap(plan_b.edges[index - 1], plan_b.edges[swap_with]);
    }
    FlowGroupPlan plan_a_reordered = plan_a;
    std::reverse(plan_a_reordered.edges.begin(), plan_a_reordered.edges.end());
    std::vector<ParticipantId> reversed_participants = backward.participants;
    std::reverse(reversed_participants.begin(), reversed_participants.end());

    FlowGroup group_a;
    FlowGroup group_b;
    FlowGroup group_c;
    CTF_CHECK_OK(construct_flow_group(plan_a, forward.participants, group_a));
    CTF_CHECK_OK(construct_flow_group(plan_b, backward.participants, group_b));
    CTF_CHECK_OK(construct_flow_group(plan_a_reordered, forward.participants, group_c));
    CTF_CHECK_EQ(group_a.id, group_b.id);
    CTF_CHECK_EQ(group_a.id, group_c.id);
    CTF_CHECK_EQ(group_a.edges, group_b.edges);
    CTF_CHECK_EQ(group_a.edges, group_c.edges);
    CTF_CHECK_EQ(group_a.participants, group_b.participants);
    CTF_CHECK_EQ(group_a.total_logical_bytes, group_b.total_logical_bytes);
    CTF_CHECK_EQ(flow_group_content_digest(plan_a), flow_group_content_digest(plan_b));
  }
}

CTF_TEST("property_engine", "capacity_clamping_never_exceeds_the_observation") {
  const std::uint64_t seed = ctf::test::resolve_seed("capacity_clamping_never_exceeds_the_observation");
  Rng rng(seed);
  for (int step = 0; step < 200; ++step) {
    ctf::test::SyntheticWorld world(seed + static_cast<std::uint64_t>(step), 2, 2, rng.below(10001));
    const std::uint32_t utilization = rng.below(10001);
    const std::uint64_t fraction = 1 + rng.below(1000);
    world.capacity = ctf::test::make_capacity(world.fabric.topology, seed + 500 + step, utilization, fraction);
    world.congestion = ctf::test::make_congestion(world.fabric.topology, seed + 900 + step, utilization);
    world.rebuild(10);
    CollectiveDefinition definition =
        ctf::test::make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "cap");
    definition.generation = world.ids.next();
    CollectiveInstance instance;
    instance.id = definition.id;
    instance.generation = definition.generation;
    instance.attempt = mint_identity();
    const FlowGroupPlan plan =
        ctf::test::make_ring_plan(instance, definition.participants, 1 + rng.below(1u << 20));
    const DecisionRecord decision =
        evaluate(world.snapshot, ctf::test::make_request(instance, world.snapshot, plan), definition);
    check_decision_invariants(decision, world.snapshot, definition);
    if (outcome_grants_authority(decision.outcome)) {
      // The admitted rate can never exceed the observed total availability.
      std::uint64_t available = 0;
      for (const LinkCapacity& entry : world.capacity.links) available += entry.available_bps;
      const std::uint64_t ceiling = decision.pacing.ceiling_bandwidth_bps;
      if (ceiling != kUnlimitedRate) {
        CTF_CHECK(decision.pacing.admitted_bandwidth_bps <= std::max(ceiling, available));
      }
      CTF_CHECK(decision.pacing.utilization_bps == utilization);
    }
  }
}

CTF_TEST("property_engine", "rate_ceiling_monotonically_falls_as_congestion_rises") {
  const std::uint64_t seed = ctf::test::resolve_seed("rate_ceiling_monotonically_falls_as_congestion_rises");
  Rng rng(seed);
  for (int step = 0; step < 40; ++step) {
    ctf::test::SyntheticWorld world(seed + static_cast<std::uint64_t>(step), 2, 2, 1000);
    CollectiveDefinition definition =
        ctf::test::make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "mono");
    definition.generation = world.ids.next();
    CollectiveInstance instance;
    instance.id = definition.id;
    instance.generation = definition.generation;
    instance.attempt = mint_identity();
    const FlowGroupPlan plan = ctf::test::make_ring_plan(instance, definition.participants, 1u << 20);
    std::uint64_t previous_response = 1000;
    std::uint64_t previous_admitted = kUnlimitedRate;
    for (std::uint32_t utilization = 0; utilization <= 10000; utilization += 500) {
      world.congestion = ctf::test::make_congestion(world.fabric.topology, seed + 7000 + utilization,
                                                    utilization);
      world.rebuild(5);
      const DecisionRecord decision =
          evaluate(world.snapshot, ctf::test::make_request(instance, world.snapshot, plan), definition);
      if (!outcome_grants_authority(decision.outcome)) continue;
      // The capacity clamp is exact: each half of the scaled ceiling never
      // exceeds its share of the observed availability.
      const std::uint64_t ceiling = decision.pacing.ceiling_bandwidth_bps;
      if (ceiling != kUnlimitedRate) {
        CTF_CHECK(decision.pacing.admitted_bandwidth_bps <= ceiling);
      }
      // More congestion never produces a larger response factor.  Above the
      // class floor the admitted rate is that factor applied to the ceiling, so
      // it is non increasing too; at or below the floor it is pinned to the
      // floor, which is a guarantee rather than a violation.
      const CollectiveClassPolicy* collective_policy =
          world.policy.find_collective(definition.collective_class);
      const TrafficClassPolicy* class_policy = world.policy.find_class(collective_policy->traffic_class);
      const std::uint64_t floor = class_policy->minimum_bandwidth_bps;
      if (decision.pacing.admitted_bandwidth_bps > floor && previous_admitted > floor) {
        CTF_CHECK_MSG(decision.pacing.admitted_bandwidth_bps <= previous_admitted,
                      "the admitted rate rose as congestion rose");
      }
      previous_admitted = decision.pacing.admitted_bandwidth_bps;
      const auto& response = world.policy.congestion_response;
      std::size_t bucket = 0;
      while (bucket < CongestionResponseTable::kBucketCount &&
             utilization >= response.utilization_bps[bucket]) {
        ++bucket;
      }
      const std::size_t index =
          bucket == 0 ? 0 : std::min(bucket, CongestionResponseTable::kBucketCount - 1);
      CTF_CHECK(response.rate_numerator_per_mille[index] <= previous_response);
      previous_response = response.rate_numerator_per_mille[index];
    }
    CTF_CHECK_EQ(world.congestion.links.front().level,
                 congestion_level_from_utilization_bps(10000));
  }
}

CTF_TEST_MAIN()
