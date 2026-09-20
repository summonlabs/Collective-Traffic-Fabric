// Collective Traffic Fabric - authority state machine proofs.
// Copyright 2026 Summon Software Labs.
//
// These cases are about currency rather than concurrency: which evidence is
// allowed to support a decision, what happens when it ages out, and what a
// restart may and may not bring back.
#include <cstdint>
#include <string>
#include <vector>

#include "ctf/authority.hpp"
#include "ctf/service.hpp"
#include "ctf/transport.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;

// A world wired into a real AuthorityState, so every case exercises the same
// install and fencing rules the coordinator uses.
struct AuthorityHarness {
  explicit AuthorityHarness(std::uint64_t seed)
      : fabric(ctf::test::make_fabric(seed, 2, 2)), ids(seed) {
    topology = fabric.topology;
    topology.generation = ids.next();
    topology.captured_monotonic_ms = 0;
    capacity = ctf::test::make_capacity(topology, seed + 1, 1000);
    capacity.topology_generation = topology.generation;
    capacity.generation = ids.next();
    capacity.captured_monotonic_ms = 0;
    congestion = ctf::test::make_congestion(topology, seed + 2, 1000);
    congestion.topology_generation = topology.generation;
    congestion.generation = ids.next();
    congestion.captured_monotonic_ms = 0;
    congestion.valid_for_ms = 1000;
    policy = TrafficPolicy::standard(ids.next());
  }

  void install_everything() {
    authority.begin_boot(CoordinatorEpoch{});
    CTF_CHECK_EQ(authority.install_policy(policy), InstallResult::kInstalled);
    CTF_CHECK_EQ(authority.install_topology(topology), InstallResult::kInstalled);
    CTF_CHECK_EQ(authority.install_capacity(capacity), InstallResult::kInstalled);
    CTF_CHECK_EQ(authority.install_congestion(congestion), InstallResult::kInstalled);
  }

  ctf::test::RackFabric fabric;
  ctf::test::IdFactory ids;
  TopologyEvidence topology;
  CapacityEvidence capacity;
  CongestionEvidence congestion;
  TrafficPolicy policy;
  AuthorityState authority;
};

}  // namespace

CTF_TEST("authority", "generations_only_advance_and_equal_generations_are_refused") {
  AuthorityHarness harness(201);
  harness.install_everything();
  const PolicyGeneration installed_policy = harness.authority.policy_generation();
  const TopologyGeneration installed_topology = harness.authority.topology_generation();

  // The same policy again is refused: an equal generation would make two
  // different policies indistinguishable to every stored decision.
  CTF_CHECK_EQ(harness.authority.install_policy(harness.policy), InstallResult::kRejectedSameGeneration);
  CTF_CHECK_EQ(harness.authority.topology_generation(), installed_topology);
  CTF_CHECK_EQ(harness.authority.policy_generation(), installed_policy);

  // A generation that was already superseded is refused as well.  Generations
  // are minted identities, so "older" means "already seen on this axis", not
  // "numerically smaller": the coordinator keeps a bounded record of what it has
  // accepted and refuses a replay of any of it.
  TopologyEvidence superseded = harness.topology;
  superseded.generation = harness.ids.next();
  CTF_CHECK_EQ(harness.authority.install_topology(superseded), InstallResult::kInstalled);
  CTF_CHECK_EQ(harness.authority.install_topology(harness.topology),
               InstallResult::kRejectedOlderGeneration);
  CTF_CHECK_EQ(harness.authority.topology_generation(), superseded.generation);

  // A newer policy is installed and re-stamps every later decision.
  TrafficPolicy newer = TrafficPolicy::standard(harness.ids.next());
  CTF_CHECK_EQ(harness.authority.install_policy(newer), InstallResult::kInstalled);
  CTF_CHECK_NE(harness.authority.policy_generation(), installed_policy);

  // A malformed policy is refused without touching the installed one.
  TrafficPolicy broken = newer;
  broken.class_policies.clear();
  CTF_CHECK_EQ(harness.authority.install_policy(broken), InstallResult::kRejectedInvalid);
  CTF_CHECK_EQ(harness.authority.policy_generation(), newer.generation);
}

CTF_TEST("authority", "topology_change_drops_observations_that_described_the_old_one") {
  AuthorityHarness harness(203);
  harness.install_everything();
  CTF_CHECK(harness.authority.capacity_available());
  CTF_CHECK(harness.authority.congestion_available());

  TopologyEvidence next = harness.topology;
  next.generation = harness.ids.next();
  next.captured_monotonic_ms = 100;
  CTF_CHECK_EQ(harness.authority.install_topology(next), InstallResult::kInstalled);
  // Observations that described the previous topology are dropped, not
  // reinterpreted against a topology they never saw.
  CTF_CHECK(!harness.authority.capacity_available());
  CTF_CHECK(!harness.authority.congestion_available());
  CTF_CHECK(harness.authority.capacity_generation().is_none());
  CTF_CHECK(harness.authority.congestion_generation().is_none());

  // Evidence stamped against the old topology is refused outright.
  CTF_CHECK_EQ(harness.authority.install_capacity(harness.capacity), InstallResult::kRejectedInvalid);
  CTF_CHECK_EQ(harness.authority.install_congestion(harness.congestion), InstallResult::kRejectedInvalid);

  CapacityEvidence fresh = ctf::test::make_capacity(next, 204, 1000);
  fresh.topology_generation = next.generation;
  fresh.generation = harness.ids.next();
  CTF_CHECK_EQ(harness.authority.install_capacity(fresh), InstallResult::kInstalled);
}

CTF_TEST("authority", "liveness_expires_and_a_refresh_is_required") {
  AuthorityHarness harness(207);
  harness.install_everything();
  const ParticipantId participant = harness.ids.next();
  const BootIncarnation incarnation = harness.ids.next();
  CTF_CHECK(harness.authority.heartbeat(participant, incarnation, 1000, 500));
  CTF_CHECK_EQ(harness.authority.live_peer_count(1200), 1u);
  // At exactly the expiry instant the participant no longer holds authority.
  CTF_CHECK_EQ(harness.authority.live_peer_count(1500), 0u);
  const std::vector<ParticipantId> fenced = harness.authority.expire_due(1500);
  CTF_CHECK_EQ(fenced.size(), 1u);
  CTF_CHECK_EQ(fenced.front(), participant);
  const PeerLiveness stored = harness.authority.peer_snapshot(participant);
  CTF_CHECK_EQ(stored.state, ParticipantState::kFenced);
  // The record is still visible and honestly reports that it is not live.
  CTF_CHECK(!stored.is_current(1500));
  // The incarnation that lapsed is fenced: republishing it cannot revive the
  // participant, because a boot that ended must not come back.
  CTF_CHECK(!harness.authority.heartbeat(participant, incarnation, 1550, 500));
  PeerLiveness revived = stored;
  revived.state = ParticipantState::kLive;
  revived.expires_at_monotonic_ms = 2000;
  CTF_CHECK_EQ(harness.authority.publish_liveness(revived),
               AuthorityState::PublishResult::kFencedStaleIncarnation);
  // A strictly greater incarnation, which is what a real reboot produces, is
  // accepted and restores authority.
  const BootIncarnation rebooted(incarnation.high() + 1, incarnation.low());
  CTF_CHECK(harness.authority.heartbeat(participant, rebooted, 1600, 500));
  CTF_CHECK_EQ(harness.authority.live_peer_count(1700), 1u);
}

CTF_TEST("authority", "an_older_boot_incarnation_is_fenced_permanently") {
  AuthorityHarness harness(211);
  harness.install_everything();
  const ParticipantId participant = harness.ids.next();
  const BootIncarnation older(1, 1);
  const BootIncarnation newer(2, 2);
  PeerLiveness liveness;
  liveness.participant = participant;
  liveness.incarnation = newer;
  liveness.state = ParticipantState::kLive;
  liveness.expires_at_monotonic_ms = 10000;
  CTF_CHECK_EQ(harness.authority.publish_liveness(liveness), AuthorityState::PublishResult::kAccepted);
  liveness.incarnation = older;
  CTF_CHECK_EQ(harness.authority.publish_liveness(liveness),
               AuthorityState::PublishResult::kFencedStaleIncarnation);
  CTF_CHECK(!harness.authority.heartbeat(participant, older, 2000, 500));
  CTF_CHECK(harness.authority.heartbeat(participant, newer, 2000, 500));
  // An unset identity is not a participant and is refused, never defaulted.
  liveness.participant = Identity{};
  CTF_CHECK_EQ(harness.authority.publish_liveness(liveness),
               AuthorityState::PublishResult::kRejectedInvalid);
}

CTF_TEST("authority", "session_teardown_expires_every_participant_it_owned") {
  AuthorityHarness harness(213);
  harness.install_everything();
  const SessionId session = harness.ids.next();
  for (int index = 0; index < 3; ++index) {
    PeerLiveness liveness;
    liveness.participant = harness.ids.next();
    liveness.incarnation = harness.ids.next();
    liveness.state = ParticipantState::kLive;
    liveness.bound_session = session;
    liveness.expires_at_monotonic_ms = 10000;
    CTF_CHECK_EQ(harness.authority.publish_liveness(liveness), AuthorityState::PublishResult::kAccepted);
  }
  CTF_CHECK_EQ(harness.authority.live_peer_count(100), 3u);
  CTF_CHECK_EQ(harness.authority.expire_session(session, 100), 3u);
  CTF_CHECK_EQ(harness.authority.live_peer_count(100), 0u);
  // Expiring an unknown session changes nothing.
  CTF_CHECK_EQ(harness.authority.expire_session(harness.ids.next(), 100), 0u);
}

CTF_TEST("authority", "durable_snapshot_carries_no_dynamic_state") {
  AuthorityHarness harness(217);
  harness.install_everything();
  const ParticipantId participant = harness.ids.next();
  CTF_CHECK(harness.authority.heartbeat(participant, harness.ids.next(), 1000, 500));
  const DurableAuthority durable = harness.authority.durable_snapshot();
  CTF_CHECK_EQ(durable.topology_generation, harness.authority.topology_generation());
  CTF_CHECK_EQ(durable.policy_generation, harness.authority.policy_generation());
  CTF_CHECK_EQ(durable.topology.nodes.size(), harness.topology.nodes.size());
  // The durable record has no field for liveness, capacity or congestion: the
  // type system is what prevents a restart from resurrecting them.
  PeerLiveness live;
  live.participant = Identity(9, 9);
  live.incarnation = Identity(9, 9);
  live.state = ParticipantState::kLive;
  live.expires_at_monotonic_ms = 100000;
  CTF_CHECK_EQ(harness.authority.publish_liveness(live), AuthorityState::PublishResult::kAccepted);
  CTF_CHECK(harness.authority.capacity_available());
  CTF_CHECK(harness.authority.congestion_available());
  const CoordinatorEpoch before_boot = harness.authority.epoch();
  harness.authority.begin_boot(durable.last_epoch);
  CTF_CHECK_NE(harness.authority.epoch(), before_boot);
  CTF_CHECK_OK(harness.authority.restore_durable(durable, 2000));
  CTF_CHECK_NE(harness.authority.epoch(), before_boot);
  CTF_CHECK_EQ(harness.authority.peer_count(), 0u);
  CTF_CHECK(!harness.authority.capacity_available());
  CTF_CHECK(!harness.authority.congestion_available());
  CTF_CHECK_EQ(harness.authority.topology_generation(), durable.topology_generation);
}

CTF_TEST("authority", "a_restart_epoch_is_never_reused") {
  AuthorityHarness harness(219);
  std::vector<CoordinatorEpoch> epochs;
  CoordinatorEpoch last;
  for (int cycle = 0; cycle < 12; ++cycle) {
    last = harness.authority.begin_boot(last);
    epochs.push_back(last);
  }
  for (std::size_t index = 1; index < epochs.size(); ++index) {
    CTF_CHECK_NE(epochs[index], epochs[index - 1]);
    for (std::size_t other = 0; other < index; ++other) {
      CTF_CHECK_NE(epochs[index], epochs[other]);
    }
  }
  CTF_CHECK(harness.authority.epoch_is_current(epochs.back()));
  CTF_CHECK(!harness.authority.epoch_is_current(epochs.front()));
  CTF_CHECK(!harness.authority.epoch_is_current(Identity{}));
}

CTF_TEST("authority", "the_authority_ladder_grants_nothing_until_every_rung_is_present") {
  ctf::test::SyntheticWorld world(223, 2, 2, 1000);
  CollectiveDefinition definition =
      ctf::test::make_definition(mint_identity(), CollectiveClass::kAllReduce, world.participants, "ladder");
  definition.generation = world.ids.next();
  CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = definition.generation;
  instance.attempt = mint_identity();
  const FlowGroupPlan plan =
      ctf::test::make_ring_plan(instance, definition.participants, 1u << 20);
  const DecisionRequest request = ctf::test::make_request(instance, world.snapshot, plan);

  // Rung 0: everything present, so the decision is granted.
  const DecisionRecord baseline = evaluate(world.snapshot, request, definition);
  CTF_CHECK_EQ(baseline.outcome, Outcome::kAdmitted);

  struct Rung {
    const char* name;
    Outcome outcome;
    AuthorityAxis axis;
  };
  std::vector<Rung> observed;

  auto evaluate_with = [&](const EvaluationSnapshot& snapshot, const DecisionRequest& candidate) {
    const DecisionRecord decision = evaluate(snapshot, candidate, definition);
    CTF_CHECK(!outcome_grants_authority(decision.outcome));
    observed.push_back(Rung{"", decision.outcome, decision.axis});
    return decision;
  };

  // Rung 1: no policy.  The request does not claim to have observed one, which
  // is the honest case: nothing is installed and the requester knows it.
  {
    EvaluationSnapshot snapshot = world.snapshot;
    snapshot.policy.reset();
    snapshot.policy_generation = PolicyGeneration{};
    snapshot.policy_available = false;
    DecisionRequest candidate = request;
    candidate.observed_policy_generation = PolicyGeneration{};
    const DecisionRecord decision = evaluate_with(snapshot, candidate);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedPolicy);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kPolicyGeneration);
  }
  // Rung 1b: the requester believes a policy is installed and it is not, which
  // is a stale observation rather than a policy decision.
  {
    EvaluationSnapshot snapshot = world.snapshot;
    snapshot.policy.reset();
    snapshot.policy_generation = PolicyGeneration{};
    snapshot.policy_available = false;
    const DecisionRecord decision = evaluate_with(snapshot, request);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kPolicyGeneration);
  }
  // Rung 2: no topology.
  {
    EvaluationSnapshot snapshot = world.snapshot;
    snapshot.topology.reset();
    snapshot.topology_generation = TopologyGeneration{};
    snapshot.topology_available = false;
    const DecisionRecord decision = evaluate_with(snapshot, request);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kTopologyGeneration);
  }
  // Rung 3: no congestion observation at all.
  {
    EvaluationSnapshot snapshot = world.snapshot;
    snapshot.congestion.reset();
    snapshot.congestion_generation = EvidenceGeneration{};
    snapshot.congestion_observation_available = false;
    const DecisionRecord decision = evaluate_with(snapshot, request);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRequiresReplan);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCongestionEvidence);
  }
  // Rung 4: capacity that cannot honor the class floor.
  {
    EvaluationSnapshot snapshot = world.snapshot;
    auto capacity = std::make_shared<CapacityEvidence>(world.capacity);
    for (LinkCapacity& entry : capacity->links) entry.available_bps = 1;
    capacity->canonicalize();
    snapshot.capacity = capacity;
    const DecisionRecord decision = evaluate_with(snapshot, request);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedCapacity);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCapacityEvidence);
  }
  // Rung 5: no plan.
  {
    DecisionRequest candidate = request;
    candidate.has_flow_group_plan = false;
    const DecisionRecord decision = evaluate_with(world.snapshot, candidate);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRequiresReplan);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kPlan);
  }
  // Rung 6: a participant without liveness evidence.
  {
    EvaluationSnapshot snapshot = world.snapshot;
    snapshot.peers.clear();
    const DecisionRecord decision = evaluate_with(snapshot, request);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kParticipantLiveness);
  }
  // Rung 7: a stale epoch.
  {
    DecisionRequest candidate = request;
    candidate.observed_epoch = mint_identity();
    const DecisionRecord decision = evaluate_with(world.snapshot, candidate);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCoordinatorEpoch);
  }
  // Rung 8: a stale collective generation.
  {
    DecisionRequest candidate = request;
    candidate.instance.generation = mint_identity();
    candidate.plan.instance.generation = candidate.instance.generation;
    const DecisionRecord decision = evaluate_with(world.snapshot, candidate);
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCollectiveGeneration);
  }
  // Rung 9: unknown semantics.
  {
    CollectiveDefinition unknown =
        ctf::test::make_definition(mint_identity(), CollectiveClass::kUnknown, world.participants, "unknown");
    unknown.generation = world.ids.next();
    CollectiveInstance unknown_instance;
    unknown_instance.id = unknown.id;
    unknown_instance.generation = unknown.generation;
    unknown_instance.attempt = mint_identity();
    DecisionRequest candidate = ctf::test::make_request(
        unknown_instance, world.snapshot,
        ctf::test::make_ring_plan(unknown_instance, unknown.participants, 1u << 20));
    const DecisionRecord decision = evaluate(world.snapshot, candidate, unknown);
    CTF_CHECK(!outcome_grants_authority(decision.outcome));
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kSemantics);
  }
  CTF_CHECK_EQ(observed.size(), 9u);
  // Every rung produced a distinct refusal: no two rungs collapse to the same
  // diagnosis, which is what makes the reason chain useful to an operator.
  for (std::size_t index = 0; index < observed.size(); ++index) {
    for (std::size_t other = index + 1; other < observed.size(); ++other) {
      const bool same = observed[index].outcome == observed[other].outcome &&
                        observed[index].axis == observed[other].axis;
      CTF_CHECK_MSG(!same, "two authority rungs produced the same outcome and axis");
    }
  }
}

CTF_TEST_MAIN()
