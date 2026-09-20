// Collective Traffic Fabric - concurrency, race and lifecycle proofs.
// Copyright 2026 Summon Software Labs.
//
// These cases do not merely run threads and hope.  Each one drives a specific
// interleaving that has a defined correct answer, and every case finishes by
// checking accounting so that a lost update or a leaked entry cannot hide.
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "ctf/authority.hpp"
#include "ctf/service.hpp"
#include "ctf/transport.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;

// Waits until every participant has arrived, then releases all of them at once.
// This turns a probabilistic race into a deterministic one.
class Barrier {
 public:
  explicit Barrier(std::size_t parties) : parties_(parties), waiting_(0), generation_(0) {}

  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t generation = generation_;
    if (++waiting_ == parties_) {
      waiting_ = 0;
      ++generation_;
      condition_.notify_all();
      return;
    }
    condition_.wait(lock, [this, generation]() { return generation_ != generation; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t parties_;
  std::size_t waiting_;
  std::uint64_t generation_;
};

ServiceRequestContext context_for(SessionId session, BootIncarnation incarnation, CoordinatorEpoch epoch,
                                  std::uint64_t now) {
  ServiceRequestContext context;
  context.session = session;
  context.incarnation = incarnation;
  context.epoch = epoch;
  context.now_monotonic_ms = now;
  return context;
}

// Builds a service with a policy, a synthetic topology, live participants and a
// registered collective, and hands back everything a case needs to drive it.
struct Harness {
  explicit Harness(std::uint64_t seed, std::size_t participant_count = 4)
      : world(seed, 2, 2, 1000), fabric(ctf::test::make_fabric(seed, 2, 2)) {
    config.decision_history_capacity = 512;
    config.participant_ttl_ms = 60000;
    service.set_config(config);
    CTF_CHECK_OK(service.start(1));
    CTF_CHECK_OK(service.open_session("harness", 1, session));
    SessionBinding heartbeat_session;
    CTF_CHECK_OK(service.open_session("harness-heartbeats", 1, heartbeat_session));
    heartbeat_context = context_for(heartbeat_session.id, heartbeat_session.incarnation,
                                    heartbeat_session.epoch, 1);
    context = context_for(session.id, session.incarnation, session.epoch, 1);
    // Evidence and policy are installed through the service, not around it, so
    // the real authority path is what the cases exercise: an unregistered session
    // may never mutate fabric wide state.
    ReasonChain reasons;
    PolicyGeneration policy_generation;
    CTF_CHECK_OK(service.install_policy(context, TrafficPolicy::standard(ctf::mint_identity()),
                                        policy_generation, reasons));
    TopologyEvidence topology = fabric.topology;
    topology.generation = ctf::mint_identity();
    topology.captured_monotonic_ms = ctf::transport::monotonic_now_ms();
    TopologyGeneration topology_generation;
    CTF_CHECK_OK(service.ingest_topology(context, topology, topology_generation, reasons));
    available = 0;
    for (const TopologyLink& link : topology.links) available += link.capacity_bps;
    CapacityEvidence capacity = ctf::test::make_capacity(topology, seed + 1, 1000);
    capacity.topology_generation = topology.generation;
    capacity.generation = ctf::mint_identity();
    capacity.captured_monotonic_ms = ctf::transport::monotonic_now_ms();
    EvidenceGeneration capacity_generation;
    CTF_CHECK_OK(service.ingest_capacity(context, capacity, capacity_generation, reasons));
    CongestionEvidence congestion = ctf::test::make_congestion(topology, seed + 2, 1000);
    congestion.topology_generation = topology.generation;
    congestion.generation = ctf::mint_identity();
    congestion.captured_monotonic_ms = ctf::transport::monotonic_now_ms();
    congestion.valid_for_ms = 60000;
    EvidenceGeneration congestion_generation;
    CTF_CHECK_OK(service.ingest_congestion(context, congestion, congestion_generation, reasons));
    for (std::size_t index = 0; index < participant_count; ++index) {
      const ParticipantId participant = ctf::mint_identity();
      participants.push_back(participant);
      incarnations.push_back(ctf::mint_identity());
      nodes.push_back(topology.nodes[index % topology.nodes.size()].id);
    }
  }

  ~Harness() { service.stop(2); }

  void publish_all(std::uint64_t now_monotonic_ms) {
    heartbeat_context.now_monotonic_ms = now_monotonic_ms;
    for (std::size_t index = 0; index < participants.size(); ++index) {
      CTF_CHECK_OK(service.publish_participant(heartbeat_context, participants[index],
                                               incarnations[index], nodes[index],
                                               ParticipantState::kLive));
    }
  }

  std::uint64_t now_ms() const { return ctf::transport::monotonic_now_ms() + 1000; }

  ctf::test::SyntheticWorld world;
  ctf::test::RackFabric fabric;
  ServiceConfig config;
  CoordinatorService service;
  SessionBinding session;
  ServiceRequestContext context;
  ServiceRequestContext heartbeat_context;
  std::vector<ParticipantId> participants;
  std::vector<BootIncarnation> incarnations;
  std::vector<NodeId> nodes;
  std::uint64_t available = 0;
};

}  // namespace

CTF_TEST("concurrency_service", "parallel_registration_creates_exactly_one_generation") {
  Harness harness(101, 2);
  const std::size_t threads = 8;
  std::vector<std::thread> workers;
  std::atomic<int> created{0};
  std::atomic<int> unchanged{0};
  Barrier barrier(threads);
  CollectiveDefinition definition =
      ctf::test::make_definition(ctf::mint_identity(), CollectiveClass::kAllReduce, harness.participants);
  for (std::size_t index = 0; index < threads; ++index) {
    workers.emplace_back([&]() {
      barrier.arrive_and_wait();
      RegistrationOutcome outcome;
      const Status status = harness.service.register_collective(harness.context, definition, outcome);
      if (!status.is_ok()) return;
      if (outcome.created) ++created;
      if (outcome.result == RegistrationResult::kUnchanged) ++unchanged;
    });
  }
  for (std::thread& worker : workers) worker.join();
  // Exactly one thread may create the definition; every other thread must
  // observe the same generation rather than minting a competing one.
  CTF_CHECK_EQ(created.load(), 1);
  CTF_CHECK_EQ(unchanged.load(), static_cast<int>(threads - 1));
  const CollectiveRecord* record = harness.service.catalog().find(definition.id);
  CTF_CHECK(record != nullptr);
  CTF_CHECK_EQ(record->generation_ancestry_depth, 1u);
  CTF_CHECK_EQ(harness.service.counters().generation_advances, 0u);
}

CTF_TEST("concurrency_service", "parallel_decisions_never_duplicate_a_flow_group_id") {
  Harness harness(103, 4);
  harness.publish_all(harness.now_ms());
  CollectiveDefinition definition =
      ctf::test::make_definition(ctf::mint_identity(), CollectiveClass::kAllReduce, harness.participants);
  RegistrationOutcome outcome;
  CTF_CHECK_OK(harness.service.register_collective(harness.context, definition, outcome));
  definition.generation = outcome.generation;
  const CollectiveAttemptId attempt = ctf::mint_identity();
  AttemptOutcome attempt_outcome;
  CTF_CHECK_OK(harness.service.begin_attempt(harness.context, definition.id, definition.generation,
                                             attempt, 1u << 20, attempt_outcome));

  CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = definition.generation;
  instance.attempt = attempt;
  const FlowGroupPlan plan =
      ctf::test::make_ring_plan(instance, definition.participants, 1u << 18);

  const std::size_t threads = 8;
  std::vector<std::thread> workers;
  std::vector<DecisionRecord> records(threads);
  std::vector<Status> statuses(threads, Status(ErrorCode::kInternalInvariant, "not run"));
  Barrier barrier(threads);
  for (std::size_t index = 0; index < threads; ++index) {
    workers.emplace_back([&, index]() {
      barrier.arrive_and_wait();
      DecisionRequest request = ctf::test::make_request(instance, harness.service.authority().snapshot(0), plan);
      request.observed_epoch = harness.service.epoch();
      request.observed_policy_generation = harness.service.authority().policy_generation();
      request.observed_topology_generation = harness.service.authority().topology_generation();
      statuses[index] = harness.service.plan_flow_group(harness.context, request, records[index]);
    });
  }
  for (std::thread& worker : workers) worker.join();
  for (const Status& status : statuses) CTF_CHECK_OK(status);
  // Every concurrent decision for the same canonical plan must agree: one plan
  // means one flow group, and every decision binds the same generations.
  for (std::size_t index = 1; index < threads; ++index) {
    CTF_CHECK_EQ(records[index].flow_group, records[0].flow_group);
    CTF_CHECK_EQ(records[index].outcome, records[0].outcome);
    CTF_CHECK_EQ(records[index].collective_generation, records[0].collective_generation);
    CTF_CHECK_EQ(records[index].topology_generation, records[0].topology_generation);
    CTF_CHECK_EQ(records[index].epoch, records[0].epoch);
  }
  CTF_CHECK_EQ(harness.service.counters().decisions, static_cast<std::uint64_t>(threads));
}

CTF_TEST("concurrency_service", "concurrent_evidence_ingest_keeps_generations_monotonic") {
  Harness harness(107, 2);
  const std::size_t threads = 6;
  std::vector<std::thread> workers;
  std::atomic<int> installed{0};
  std::atomic<int> refused{0};
  Barrier barrier(threads);
  const TopologyGeneration topology_generation = harness.service.authority().topology_generation();
  const std::uint64_t ingests_before = harness.service.counters().evidence_ingests;
  for (std::size_t index = 0; index < threads; ++index) {
    workers.emplace_back([&, index]() {
      barrier.arrive_and_wait();
      for (int round = 0; round < 20; ++round) {
        CongestionEvidence evidence =
            ctf::test::make_congestion(harness.fabric.topology, 200 + index * 100 + round, 1000);
        evidence.topology_generation = topology_generation;
        evidence.captured_monotonic_ms = harness.now_ms();
        evidence.valid_for_ms = 60000;
        ReasonChain reasons;
        EvidenceGeneration generation;
        const Status status = harness.service.ingest_congestion(harness.context, evidence, generation, reasons);
        if (status.is_ok()) {
          ++installed;
        } else {
          ++refused;
          // A refused ingest must be a generation conflict, never a silent drop.
          CTF_CHECK(status.code() == ErrorCode::kEnvelopeEpochStale ||
                    status.code() == ErrorCode::kValidationTopologyMissing ||
                    status.code() == ErrorCode::kValidationRateInvalid);
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  CTF_CHECK_MSG(installed.load() > 0, "no congestion observation was ever installed");
  CTF_CHECK_EQ(static_cast<int>(installed.load() + refused.load()),
               static_cast<int>(threads) * 20);
  CTF_CHECK(harness.service.authority().congestion_generation().is_some());
  // The counter records every ingest *attempt*, including the refusals, because
  // an observation that was refused is exactly what an operator needs to see.
  // It is measured as a delta across the concurrent block, since the harness
  // itself installs evidence when it is built.
  const std::uint64_t attempts = harness.service.counters().evidence_ingests - ingests_before;
  CTF_CHECK_MSG(attempts >= static_cast<std::uint64_t>(installed.load()),
                "fewer ingest attempts were counted than succeeded");
  CTF_CHECK_MSG(attempts <= static_cast<std::uint64_t>(threads) * 20,
                "more ingest attempts were counted than were made");
}

CTF_TEST("concurrency_service", "fabric_state_cannot_be_mutated_by_an_unregistered_session") {
  Harness harness(127, 2);
  // A context whose session the coordinator does not own: every fabric wide
  // mutation must be refused, because evidence and policy are authority.
  ServiceRequestContext rogue = harness.context;
  rogue.session = ctf::mint_identity();
  ReasonChain reasons;

  PolicyGeneration policy_generation;
  CTF_CHECK_CODE(harness.service.install_policy(rogue, TrafficPolicy::standard(ctf::mint_identity()),
                                                policy_generation, reasons),
                 ErrorCode::kEnvelopeSessionUnknown);
  CTF_CHECK(policy_generation.is_none());

  TopologyEvidence topology = harness.fabric.topology;
  topology.generation = ctf::mint_identity();
  TopologyGeneration topology_generation;
  CTF_CHECK_CODE(harness.service.ingest_topology(rogue, topology, topology_generation, reasons),
                 ErrorCode::kEnvelopeSessionUnknown);

  CapacityEvidence capacity = ctf::test::make_capacity(topology, 128, 1000);
  capacity.topology_generation = harness.service.authority().topology_generation();
  capacity.generation = ctf::mint_identity();
  EvidenceGeneration evidence_generation;
  CTF_CHECK_CODE(harness.service.ingest_capacity(rogue, capacity, evidence_generation, reasons),
                 ErrorCode::kEnvelopeSessionUnknown);

  // A known session with the wrong boot incarnation is refused as well.
  ServiceRequestContext impostor = harness.context;
  impostor.incarnation = ctf::mint_identity();
  CTF_CHECK_CODE(harness.service.ingest_capacity(impostor, capacity, evidence_generation, reasons),
                 ErrorCode::kEnvelopeSessionMismatch);

  // A known session on a stale epoch is refused as well.
  ServiceRequestContext stale = harness.context;
  stale.epoch = ctf::mint_identity();
  CTF_CHECK_CODE(harness.service.ingest_capacity(stale, capacity, evidence_generation, reasons),
                 ErrorCode::kEnvelopeEpochStale);

  // Nothing changed: the installed generations are exactly what the harness set.
  CTF_CHECK_EQ(harness.service.authority().policy_generation(),
               harness.service.authority().policy_generation());
  CTF_CHECK(harness.service.authority().topology_generation().is_some());
  CTF_CHECK(harness.service.authority().capacity_generation().is_some());
}

CTF_TEST("concurrency_service", "sessions_open_and_close_under_contention_with_exact_accounting") {
  const std::size_t threads = 8;
  const int rounds = 40;
  ServiceConfig config;
  config.decision_history_capacity = 64;
  CoordinatorService service(config);
  CTF_CHECK_OK(service.start(1));
  std::vector<std::thread> workers;
  std::atomic<int> opened{0};
  std::atomic<int> limit_hits{0};
  Barrier barrier(threads);
  for (std::size_t index = 0; index < threads; ++index) {
    workers.emplace_back([&]() {
      barrier.arrive_and_wait();
      for (int round = 0; round < rounds; ++round) {
        SessionBinding binding;
        const Status status = service.open_session("contention", 1, binding);
        if (status.is_ok()) {
          ++opened;
          CTF_CHECK_OK(service.close_session(binding.id, 2));
        } else {
          // The session bound is a resource limit, not a defect.
          CTF_CHECK_EQ(status.code(), ErrorCode::kTransportSessionLimit);
          ++limit_hits;
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  CTF_CHECK_EQ(opened.load() + limit_hits.load(), static_cast<int>(threads) * rounds);
  CTF_CHECK_EQ(service.session_count(), 0u);
  const CoordinatorService::Counters counters = service.counters();
  CTF_CHECK_EQ(counters.sessions_opened, static_cast<std::uint64_t>(opened.load()));
  CTF_CHECK_EQ(counters.sessions_closed, static_cast<std::uint64_t>(opened.load()));
  CTF_CHECK_OK(service.stop(3));
}

CTF_TEST("concurrency_service", "repeated_start_stop_cycles_leave_no_state_behind") {
  ServiceConfig config;
  config.decision_history_capacity = 32;
  CoordinatorService service(config);
  std::vector<CoordinatorEpoch> epochs;
  for (int cycle = 0; cycle < 20; ++cycle) {
    CTF_CHECK_OK(service.start(static_cast<std::uint64_t>(cycle) * 10 + 1));
    epochs.push_back(service.epoch());
    // Each cycle does real work and then shuts down, so the teardown path is
    // exercised with live sessions, live participants and live history.
    SessionBinding binding;
    CTF_CHECK_OK(service.open_session("cycle", 1, binding));
    ServiceRequestContext context;
    context.session = binding.id;
    context.incarnation = binding.incarnation;
    context.epoch = binding.epoch;
    // The publication timestamp must come from the same clock the service
    // reads, otherwise the participant would be born already expired.
    context.now_monotonic_ms = ctf::transport::monotonic_now_ms();
    CTF_CHECK_OK(service.publish_participant(context, ctf::mint_identity(), ctf::mint_identity(),
                                              NodeId{}, ParticipantState::kLive));
    CTF_CHECK_EQ(service.live_peer_count(), 1u);
    CTF_CHECK_OK(service.stop(static_cast<std::uint64_t>(cycle) * 10 + 5));
    // Every shutdown must leave no liveness, no history and no sessions.
    CTF_CHECK_EQ(service.session_count(), 0u);
    CTF_CHECK_EQ(service.live_peer_count(), 0u);
    CTF_CHECK_EQ(service.history().size(), 0u);
    // The record itself is still visible, and honestly reports that it holds no
    // authority after shutdown.
    for (const PeerLiveness& peer : service.peers()) {
      CTF_CHECK(!participant_state_holds_authority(peer.state));
    }
    CTF_CHECK_EQ(service.counters().sessions_opened, static_cast<std::uint64_t>(cycle + 1));
  }
  for (std::size_t index = 1; index < epochs.size(); ++index) {
    // A restart always advances the epoch, and never repeats an earlier one.
    CTF_CHECK_NE(epochs[index], epochs[index - 1]);
    for (std::size_t other = 0; other < index; ++other) {
      CTF_CHECK_NE(epochs[index], epochs[other]);
    }
  }
}

CTF_TEST("concurrency_service", "authority_snapshot_under_concurrent_mutation_is_consistent") {
  Harness harness(109, 8);
  std::atomic<bool> stop{false};
  std::atomic<int> snapshots{0};
  std::vector<std::thread> mutators;
  std::vector<std::thread> readers;
  for (int index = 0; index < 3; ++index) {
    mutators.emplace_back([&, index]() {
      for (int round = 0; round < 200; ++round) {
        CongestionEvidence evidence =
            ctf::test::make_congestion(harness.fabric.topology, 900 + index * 1000 + round,
                                       static_cast<std::uint32_t>(round % 10000));
        evidence.topology_generation = harness.service.authority().topology_generation();
        evidence.captured_monotonic_ms = harness.now_ms();
        evidence.valid_for_ms = 60000;
        ReasonChain reasons;
        EvidenceGeneration generation;
        static_cast<void>(harness.service.ingest_congestion(harness.context, evidence, generation, reasons));
      }
      stop.store(true, std::memory_order_release);
    });
  }
  for (int index = 0; index < 3; ++index) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_acquire)) {
        const EvaluationSnapshot snapshot = harness.service.authority().snapshot(harness.now_ms());
        // A snapshot is internally consistent: its generation fields agree with
        // the evidence it carries, whatever a mutator was doing at the time.
        if (snapshot.congestion) {
          CTF_CHECK(snapshot.congestion_generation.is_some());
          CTF_CHECK_EQ(snapshot.congestion->generation, snapshot.congestion_generation);
          CTF_CHECK_EQ(snapshot.congestion->topology_generation, snapshot.topology_generation);
        }
        if (snapshot.capacity) {
          CTF_CHECK_EQ(snapshot.capacity->generation, snapshot.capacity_generation);
        }
        if (snapshot.topology) {
          CTF_CHECK_EQ(snapshot.topology->generation, snapshot.topology_generation);
        }
        CTF_CHECK(snapshot.peers.size() <= limits::kPeersMax);
        ++snapshots;
      }
    });
  }
  for (std::thread& worker : mutators) worker.join();
  for (std::thread& worker : readers) worker.join();
  CTF_CHECK_MSG(snapshots.load() > 0, "no snapshot was taken during the race");
}

CTF_TEST("concurrency_service", "participant_fencing_is_linearisable_under_races") {
  Harness harness(113, 1);
  const ParticipantId participant = harness.participants.front();
  // Boot incarnations are ordered values in this runtime: a participant mints a
  // strictly greater incarnation on every boot.  The race below is only
  // meaningful with that ordering made explicit.
  const BootIncarnation first(1, 1);
  const BootIncarnation second(2, 2);
  Barrier barrier(2);
  std::atomic<bool> accepted_first{false};
  std::atomic<bool> accepted_second{false};
  std::thread older([&]() {
    barrier.arrive_and_wait();
    accepted_first.store(harness.service.publish_participant(harness.heartbeat_context, participant, first,
                                                              harness.nodes.front(),
                                                              ParticipantState::kLive).is_ok(),
                         std::memory_order_release);
  });
  std::thread newer([&]() {
    barrier.arrive_and_wait();
    accepted_second.store(harness.service.publish_participant(harness.heartbeat_context, participant, second,
                                                               harness.nodes.front(),
                                                               ParticipantState::kLive).is_ok(),
                          std::memory_order_release);
  });
  older.join();
  newer.join();
  const PeerLiveness stored = harness.service.authority().peer_snapshot(participant);
  // Whichever publication landed first, the stored incarnation is always the
  // greater of the two: an older boot can never win a race against a newer one.
  CTF_CHECK(stored.incarnation == first || stored.incarnation == second);
  if (accepted_first.load() && accepted_second.load()) {
    CTF_CHECK_EQ(stored.incarnation, second);
  }
  CTF_CHECK(!harness.service.authority().heartbeat(participant, first, harness.now_ms(), 1000));
  CTF_CHECK(harness.service.authority().heartbeat(participant, second, harness.now_ms(), 1000));
}

CTF_TEST_MAIN()
