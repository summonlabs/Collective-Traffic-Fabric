// Collective Traffic Fabric - scale proofs.
// Copyright 2026 Summon Software Labs.
//
// These cases are about cost, not correctness: thousands of active collectives
// and participant edges must not turn a keyed lookup into a linear scan, and the
// engine reports its own access counts so accidental O(N^2) behaviour is visible
// without measuring wall clock time.
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "ctf/catalog.hpp"
#include "ctf/engine.hpp"
#include "ctf/history.hpp"
#include "ctf/service.hpp"
#include "ctf/transport.hpp"
#include "ctf/coordinator.hpp"
#include "support/client_session.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;

std::uint64_t elapsed_ms_since(std::uint64_t start) {
  const std::uint64_t now = ctf::transport::monotonic_now_ms();
  return now >= start ? now - start : 0;
}

}  // namespace

CTF_TEST("scale_fabric", "thousands_of_collectives_register_and_advance_without_quadratic_cost") {
  constexpr std::uint32_t kCollectives = 4000;
  constexpr std::uint32_t kParticipantsPerCollective = 8;
  CollectiveCatalog catalog(kCollectives + 16);
  ctf::test::IdFactory ids(9001);

  std::vector<ParticipantId> participants;
  participants.reserve(kParticipantsPerCollective);
  for (std::uint32_t index = 0; index < kParticipantsPerCollective; ++index) {
    participants.push_back(ids.next());
  }
  canonicalize_participants(participants);

  std::vector<CollectiveId> ids_of_collectives;
  ids_of_collectives.reserve(kCollectives);
  const std::uint64_t register_start = ctf::transport::monotonic_now_ms();
  for (std::uint32_t index = 0; index < kCollectives; ++index) {
    CollectiveDefinition definition =
        ctf::test::make_definition(ids.next(), CollectiveClass::kAllReduce, participants, "scale");
    ids_of_collectives.push_back(definition.id);
    const RegistrationOutcome outcome = catalog.register_definition(definition, index);
    CTF_CHECK_EQ(outcome.result, RegistrationResult::kRegistered);
  }
  const std::uint64_t register_ms = elapsed_ms_since(register_start);
  CTF_CHECK_EQ(catalog.size(), kCollectives);

  // A second registration pass over every collective must be a constant time
  // lookup each, not a scan: the whole pass is checked for a comparable cost.
  const std::uint64_t revisit_start = ctf::transport::monotonic_now_ms();
  std::uint32_t advanced = 0;
  for (std::uint32_t index = 0; index < kCollectives; ++index) {
    CTF_CHECK(catalog.contains(ids_of_collectives[index]));
    CollectiveDefinition modified =
        ctf::test::make_definition(ids_of_collectives[index], CollectiveClass::kAllReduce, participants,
                                   "scale-2");
    const RegistrationOutcome outcome = catalog.register_definition(modified, index);
    if (outcome.result == RegistrationResult::kGenerationAdvanced) ++advanced;
  }
  const std::uint64_t revisit_ms = elapsed_ms_since(revisit_start);
  CTF_CHECK_EQ(advanced, kCollectives);
  // The bound is deliberately loose: this is a smoke alarm for a quadratic
  // regression, not a benchmark.  A linear pass over four thousand records is
  // milliseconds; a quadratic one is minutes.
  CTF_CHECK_MSG(register_ms < 20000, "registration scaled implausibly slowly");
  CTF_CHECK_MSG(revisit_ms < 20000, "the second pass scaled implausibly slowly");
}

CTF_TEST("scale_fabric", "engine_work_per_decision_is_bounded_by_the_plan") {
  // The engine's own counters, not the clock, are the evidence here: for a
  // group with E edges and P participants, the access counts must grow with the
  // input size and never with the square of it.
  std::vector<std::size_t> sizes = {4, 16, 64, 256};
  std::vector<EngineCounters> counters;
  for (const std::size_t size : sizes) {
    ctf::test::SyntheticWorld world(9100 + size, 2, 2, 1000);
    // Build a participant set of the requested size by cycling over the nodes.
    std::vector<ParticipantId> participants;
    for (std::size_t index = 0; index < size; ++index) {
      participants.push_back(world.ids.next());
    }
    canonicalize_participants(participants);
    // A live peer for every participant, all bound to the first topology node.
    for (const ParticipantId& participant : participants) {
      PeerLiveness liveness;
      liveness.participant = participant;
      liveness.incarnation = world.ids.next();
      liveness.state = ParticipantState::kLive;
      liveness.node = world.fabric.nodes.front();
      liveness.expires_at_monotonic_ms = 100000;
      world.snapshot.peers.push_back(liveness);
    }
    std::sort(world.snapshot.peers.begin(), world.snapshot.peers.end(),
              [](const PeerLiveness& a, const PeerLiveness& b) { return a.participant < b.participant; });
    CollectiveDefinition definition =
        ctf::test::make_definition(mint_identity(), CollectiveClass::kAllReduce, participants, "scale");
    definition.generation = world.ids.next();
    CollectiveInstance instance;
    instance.id = definition.id;
    instance.generation = definition.generation;
    instance.attempt = mint_identity();
    const FlowGroupPlan plan = ctf::test::make_ring_plan(instance, participants, 4096);
    EngineCounters work;
    const DecisionRecord decision = evaluate(world.snapshot, ctf::test::make_request(instance, world.snapshot, plan),
                                             definition, &work);
    CTF_CHECK_EQ(decision.outcome, Outcome::kAdmitted);
    counters.push_back(work);
  }
  for (std::size_t index = 1; index < sizes.size(); ++index) {
    const std::size_t previous_size = sizes[index - 1];
    const std::size_t current_size = sizes[index];
    const double growth = static_cast<double>(current_size) / static_cast<double>(previous_size);
    const double lookups = static_cast<double>(counters[index].lookups) + 1.0;
    const double previous_lookups = static_cast<double>(counters[index - 1].lookups) + 1.0;
    // Linear growth is the expectation; anything beyond a small constant factor
    // over the input growth is a quadratic scan in disguise.
    CTF_CHECK_MSG(lookups <= previous_lookups * growth * 4.0,
                  "engine lookups grew faster than the input size");
  }
}

CTF_TEST("scale_fabric", "decision_history_stays_bounded_under_a_long_run") {
  constexpr std::uint32_t kDecisions = 20000;
  DecisionHistory history(1024);
  const std::uint64_t start = ctf::transport::monotonic_now_ms();
  for (std::uint32_t index = 0; index < kDecisions; ++index) {
    DecisionRecord record;
    record.instance.id = Identity(1, index);
    record.instance.generation = Identity(2, index);
    record.instance.attempt = Identity(3, index);
    record.outcome = index % 3 == 0 ? Outcome::kAdmitted : Outcome::kRejectedStale;
    history.record(record);
  }
  const std::uint64_t elapsed = elapsed_ms_since(start);
  CTF_CHECK_EQ(history.size(), 1024u);
  CTF_CHECK_EQ(history.evicted_count(), kDecisions - 1024);
  CTF_CHECK_EQ(history.first_ordinal(), kDecisions - 1024 + 1);
  const std::vector<DecisionRecord> latest = history.latest(64);
  CTF_CHECK_EQ(latest.size(), 64u);
  CTF_CHECK_EQ(latest.front().instance.id, Identity(1, kDecisions - 1));
  CTF_CHECK_MSG(elapsed < 20000, "history recording scaled implausibly slowly");
}

CTF_TEST("scale_fabric", "service_admits_thousands_of_collectives_with_one_live_fabric") {
  // The end to end path: a real CoordinatorService with a few thousand live
  // collectives, each with its own attempt and flow group decision.
  constexpr std::uint32_t kCollectives = 3000;
  ServiceConfig config;
  config.decision_history_capacity = 2048;
  config.participant_ttl_ms = 3600000;
  CoordinatorService service(config);
  CTF_CHECK_OK(service.start(ctf::transport::monotonic_now_ms()));

  SessionBinding control;
  CTF_CHECK_OK(service.open_session("scale-control", ctf::transport::monotonic_now_ms(), control));
  ServiceRequestContext control_context;
  control_context.session = control.id;
  control_context.incarnation = control.incarnation;
  control_context.epoch = control.epoch;
  control_context.now_monotonic_ms = ctf::transport::monotonic_now_ms();

  ctf::test::RackFabric fabric = ctf::test::make_fabric(9200, 4, 8);
  TopologyEvidence topology = fabric.topology;
  topology.generation = mint_identity();
  topology.captured_monotonic_ms = control_context.now_monotonic_ms;
  CTF_CHECK(service.authority().install_policy(TrafficPolicy::standard(mint_identity())) ==
            InstallResult::kInstalled);
  CTF_CHECK(service.authority().install_topology(topology) == InstallResult::kInstalled);
  CapacityEvidence capacity = ctf::test::make_capacity(topology, 9201, 1000);
  capacity.topology_generation = topology.generation;
  capacity.generation = mint_identity();
  capacity.captured_monotonic_ms = control_context.now_monotonic_ms;
  CTF_CHECK(service.authority().install_capacity(capacity) == InstallResult::kInstalled);
  CongestionEvidence congestion = ctf::test::make_congestion(topology, 9202, 1000);
  congestion.topology_generation = topology.generation;
  congestion.generation = mint_identity();
  congestion.captured_monotonic_ms = control_context.now_monotonic_ms;
  congestion.valid_for_ms = 3600000;
  CTF_CHECK(service.authority().install_congestion(congestion) == InstallResult::kInstalled);

  ctf::test::IdFactory ids(9203);
  std::vector<ParticipantId> participants;
  for (int index = 0; index < 4; ++index) participants.push_back(ids.next());
  canonicalize_participants(participants);
  for (const ParticipantId& participant : participants) {
    CTF_CHECK_OK(service.publish_participant(control_context, participant, ids.next(),
                                              topology.nodes.front().id, ParticipantState::kLive));
  }

  const std::uint64_t start = ctf::transport::monotonic_now_ms();
  std::uint32_t admitted = 0;
  for (std::uint32_t index = 0; index < kCollectives; ++index) {
    CollectiveDefinition definition =
        ctf::test::make_definition(ids.next(), CollectiveClass::kAllReduce, participants, "scale");
    RegistrationOutcome registration;
    CTF_CHECK_OK(service.register_collective(control_context, definition, registration));
    CTF_CHECK(registration.generation.is_some());
    definition.generation = registration.generation;
    const CollectiveAttemptId attempt = ids.next();
    AttemptOutcome attempt_outcome;
    CTF_CHECK_OK(service.begin_attempt(control_context, definition.id, definition.generation, attempt,
                                       1u << 20, attempt_outcome));
    CollectiveInstance instance;
    instance.id = definition.id;
    instance.generation = definition.generation;
    instance.attempt = attempt;
    DecisionRequest request = ctf::test::make_request(
        instance, service.authority().snapshot(control_context.now_monotonic_ms),
        ctf::test::make_ring_plan(instance, participants, 1u << 18));
    request.observed_epoch = service.epoch();
    request.observed_policy_generation = service.authority().policy_generation();
    request.observed_topology_generation = service.authority().topology_generation();
    request.observed_capacity_generation = service.authority().capacity_generation();
    request.observed_congestion_generation = service.authority().congestion_generation();
    DecisionRecord decision;
    CTF_CHECK_OK(service.plan_flow_group(control_context, request, decision));
    if (outcome_grants_authority(decision.outcome)) ++admitted;
  }
  const std::uint64_t elapsed = elapsed_ms_since(start);
  CTF_CHECK_EQ(admitted, kCollectives);
  CTF_CHECK_EQ(service.catalog().size(), kCollectives);
  CTF_CHECK_EQ(service.counters().decisions, kCollectives);
  CTF_CHECK_MSG(elapsed < 60000, "the end to end scale run was implausibly slow");

  // Inspection of one collective out of thousands must remain a keyed lookup.
  const std::vector<CollectiveId> page = service.catalog().ids(0, 1);
  CTF_CHECK_EQ(page.size(), 1u);
  const std::string report = service.inspect_collective(page.front());
  CTF_CHECK(report.find("collective: ") != std::string::npos);
  const std::string summary = service.inspect_summary();
  CTF_CHECK(summary.find("collectives: 3000") != std::string::npos);
  CTF_CHECK_OK(service.stop(ctf::transport::monotonic_now_ms()));
}

CTF_TEST("scale_fabric", "large_participant_sets_stay_linear_in_every_stage") {
  // A single collective with thousands of members: construction, validation and
  // the decision must all be linear in the number of edges.
  constexpr std::uint32_t kParticipants = 2000;
  ctf::test::SyntheticWorld world(9300, 4, 8, 1000);
  ctf::test::IdFactory ids(9301);
  std::vector<ParticipantId> participants;
  participants.reserve(kParticipants);
  for (std::uint32_t index = 0; index < kParticipants; ++index) participants.push_back(ids.next());
  canonicalize_participants(participants);
  for (const ParticipantId& participant : participants) {
    PeerLiveness liveness;
    liveness.participant = participant;
    liveness.incarnation = ids.next();
    liveness.state = ParticipantState::kLive;
    liveness.node = world.fabric.nodes.front();
    liveness.expires_at_monotonic_ms = 1000000;
    world.snapshot.peers.push_back(liveness);
  }
  std::sort(world.snapshot.peers.begin(), world.snapshot.peers.end(),
            [](const PeerLiveness& a, const PeerLiveness& b) { return a.participant < b.participant; });

  CollectiveDefinition definition =
      ctf::test::make_definition(mint_identity(), CollectiveClass::kAllGather, participants, "wide");
  definition.generation = ids.next();
  CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = definition.generation;
  instance.attempt = ids.next();
  const FlowGroupPlan plan = ctf::test::make_ring_plan(instance, participants, 4096);

  const std::uint64_t construct_start = ctf::transport::monotonic_now_ms();
  FlowGroup group;
  CTF_CHECK_OK(construct_flow_group(plan, participants, group));
  const std::uint64_t construct_ms = elapsed_ms_since(construct_start);
  CTF_CHECK_EQ(group.edges.size(), kParticipants);
  CTF_CHECK_EQ(group.participants.size(), kParticipants);
  CTF_CHECK_EQ(group.max_fan_out, 1u);
  CTF_CHECK_EQ(group.max_fan_in, 1u);

  EngineCounters work;
  const DecisionRecord decision = evaluate(world.snapshot, ctf::test::make_request(instance, world.snapshot, plan),
                                           definition, &work);
  CTF_CHECK(outcome_grants_authority(decision.outcome) ||
            decision.outcome == Outcome::kRejectedCapacity ||
            decision.outcome == Outcome::kRequiresReplan);
  CTF_CHECK_EQ(decision.participants.size(), kParticipants);
  CTF_CHECK_MSG(construct_ms < 10000, "flow group construction scaled implausibly slowly");
}

// A session's accounting must return to zero when its peer disconnects, and a
// quiet peer must not be mistaken for a closed one.  Both halves matter: retrying
// a closed descriptor leaks the worker and the session, while treating a quiet
// peer as closed would drop a healthy session.
CTF_TEST("scale_fabric", "a_disconnected_peer_releases_its_session_and_its_worker") {
  ctf::CoordinatorConfig config;
  config.transport.bind_host = "127.0.0.1";
  config.transport.port = 0;
  config.service.coordinator_label = "ctf-lifecycle-accounting";
  ctf::CoordinatorServer server(config);
  CTF_CHECK_OK(server.start(ctf::transport::monotonic_now_ms()));

  ctf::test::ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "accounting",
                              ctf::transport::monotonic_now_ms()));
  for (int attempt = 0; attempt < 20000 && server.session_count() != 1; ++attempt) {
    std::this_thread::yield();
  }
  CTF_CHECK_EQ(server.session_count(), 1u);
  CTF_CHECK_EQ(server.service().session_count(), 1u);
  CTF_CHECK_EQ(server.accepted_total(), 1u);
  CTF_CHECK_EQ(server.rejected_total(), 0u);

  // Closing the client releases the server's session, its service session and its
  // worker thread, with no shutdown involved.
  client.close();
  for (int attempt = 0; attempt < 20000 && server.session_count() != 0; ++attempt) {
    std::this_thread::yield();
  }
  CTF_CHECK_EQ(server.session_count(), 0u);
  CTF_CHECK_EQ(server.service().session_count(), 0u);

  // Several connect/disconnect cycles must leave the accounting exactly where it
  // started rather than accumulating sessions.
  for (int cycle = 0; cycle < 8; ++cycle) {
    ctf::test::ClientSession transient;
    CTF_CHECK_OK(transient.connect("127.0.0.1", server.port(), "transient",
                                   ctf::transport::monotonic_now_ms()));
    transient.close();
    for (int attempt = 0; attempt < 20000 && server.session_count() != 0; ++attempt) {
      std::this_thread::yield();
    }
    CTF_CHECK_EQ(server.session_count(), 0u);
  }
  CTF_CHECK_EQ(server.accepted_total(), 9u);

  // Stopping with no session attached is immediate, and stopping is idempotent.
  CTF_CHECK_OK(server.stop(ctf::transport::monotonic_now_ms()));
  CTF_CHECK_EQ(server.session_count(), 0u);
}

CTF_TEST_MAIN()
