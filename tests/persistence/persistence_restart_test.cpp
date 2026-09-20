// Collective Traffic Fabric - durable state across a real coordinator restart.
// Copyright 2026 Summon Software Labs.
//
// Every case here stops one real ctf::CoordinatorServer and starts a new one on
// the same snapshot path, in one process.  What is durable (definitions,
// generations, policy, topology) must survive; what is observation (liveness,
// capacity, congestion) must not.  Fabric evidence is SYNTHETIC
// (tests/support/synthetic.hpp): no physical fabric is measured anywhere.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "ctf/catalog.hpp"
#include "ctf/coordinator.hpp"
#include "ctf/persistence.hpp"
#include "ctf/protocol.hpp"
#include "ctf/service.hpp"
#include "ctf/snapshot.hpp"
#include "ctf/transport.hpp"
#include "support/client_session.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;
// The strict protocol client lives in the test support namespace; every other
// name in this file is a library name.
using ctf::test::ClientSession;

// Bounded yields spent awaiting an asynchronous condition.  The transport
// publishes no blocking form of its own counters, so the condition itself is
// polled to a bound rather than slept through.
constexpr int kWaitAttempts = 200000;

// A snapshot file in the working directory.  The name is unique per case and per
// run, and the file plus the atomic temporary sibling the store writes are both
// removed when the fixture dies, so a case leaves no artifact behind.
class TempSnapshot {
 public:
  explicit TempSnapshot(const char* tag) {
    static int counter = 0;
    ++counter;
    path_ = std::string("ctf-restart-") + tag + "-" + std::to_string(counter) + "-" +
            std::to_string(transport::monotonic_now_ms()) + ".ctfs";
    remove_files();
  }
  ~TempSnapshot() { remove_files(); }
  TempSnapshot(const TempSnapshot&) = delete;
  TempSnapshot& operator=(const TempSnapshot&) = delete;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  // Overwrites the path with bytes that are not a snapshot at all.  The bytes
  // are deterministic so a failure is reproducible, which is all the proof
  // needs: the loader must never believe them.
  void write_random_bytes(std::uint64_t seed) const {
    std::uint64_t state = seed | 0x9E3779B97F4A7C15ull;
    std::vector<std::uint8_t> bytes(96);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      bytes[index] = static_cast<std::uint8_t>(state & 0xFFu);
    }
    std::ofstream sink(path_, std::ios::binary | std::ios::trunc);
    CTF_CHECK(sink.good());
    sink.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CTF_CHECK(sink.good());
  }

 private:
  void remove_files() const {
    std::remove(path_.c_str());
    std::remove((path_ + ".tmp").c_str());
  }

  std::string path_;
};

CoordinatorConfig persistence_config(const std::string& snapshot_path) {
  CoordinatorConfig config;
  config.transport.bind_host = "127.0.0.1";
  // Port 0 asks the operating system for an ephemeral port, so a restart cannot
  // collide with the coordinator it replaced.
  config.transport.port = 0;
  config.service.coordinator_label = "ctf-persistence-restart";
  config.service.decision_history_capacity = 256;
  // Long enough that no participant can expire in the middle of a case.
  config.service.participant_ttl_ms = 60000;
  config.snapshot_path = snapshot_path;
  return config;
}

// Awaits the transport's own live session count.  A session is closed on its own
// thread, so a peer that has closed its socket cannot assume the coordinator has
// observed it yet.
void wait_for_sessions(CoordinatorServer& server, std::size_t expected) {
  for (int attempt = 0; attempt < kWaitAttempts && server.session_count() != expected; ++attempt) {
    std::this_thread::yield();
  }
  CTF_CHECK_MSG(server.session_count() == expected,
                "the coordinator did not reach the expected session count");
}

// Closes one client session and awaits the coordinator's teardown of it.  Every
// case here stops a coordinator, and a coordinator is stopped with no peer
// still attached.
void disconnect(CoordinatorServer& server, ClientSession& client) {
  client.close();
  wait_for_sessions(server, 0u);
}

// Installs the production policy over the wire and reports the generation that
// is now in force.  The policy is durable state, so it must survive a restart.
PolicyGeneration install_standard_policy(ClientSession& session) {
  protocol::InstallPolicyPayload payload;
  payload.policy = TrafficPolicy::standard(PolicyGeneration{});
  std::vector<std::uint8_t> body;
  CTF_CHECK_OK(protocol::encode_install_policy(payload, body));
  protocol::MessageKind response_kind = protocol::MessageKind::kInvalid;
  std::vector<std::uint8_t> response_body;
  CTF_CHECK_OK(session.request(protocol::MessageKind::kInstallPolicy, body, response_kind, response_body));
  CTF_CHECK_EQ(response_kind, protocol::MessageKind::kInstallPolicyAck);
  protocol::InstallPolicyAckPayload ack;
  CTF_CHECK_OK(protocol::decode_install_policy_ack(response_body, ack));
  CTF_CHECK_MSG(ack.result == 0, "the coordinator refused the traffic policy");
  return ack.generation;
}

// Ingests a complete SYNTHETIC evidence set over the wire.  A fresh topology
// drops the observations that described the previous one, so capacity and
// congestion are always supplied after it.
TopologyGeneration ingest_synthetic_evidence(CoordinatorServer& server, ClientSession& session,
                                             const ctf::test::RackFabric& fabric, std::uint64_t seed) {
  const std::uint64_t now = transport::monotonic_now_ms();
  TopologyEvidence topology = fabric.topology;
  topology.generation = mint_identity();
  topology.captured_monotonic_ms = now;
  CTF_CHECK_OK(session.ingest_topology(topology));
  // An ingestion the coordinator refused also arrives as an acknowledgement, so
  // the installed generation is the proof, not the acknowledgement.
  CTF_CHECK_EQ(server.service().authority().topology_generation(), topology.generation);

  CapacityEvidence capacity = ctf::test::make_capacity(topology, seed, 1000);
  capacity.generation = mint_identity();
  capacity.topology_generation = topology.generation;
  capacity.captured_monotonic_ms = now;
  CTF_CHECK_OK(session.ingest_capacity(capacity));
  CTF_CHECK_EQ(server.service().authority().capacity_generation(), capacity.generation);

  CongestionEvidence congestion = ctf::test::make_congestion(topology, seed + 1, 1000, 2000);
  congestion.generation = mint_identity();
  congestion.topology_generation = topology.generation;
  congestion.captured_monotonic_ms = now;
  CTF_CHECK_OK(session.ingest_congestion(congestion));
  CTF_CHECK_EQ(server.service().authority().congestion_generation(), congestion.generation);
  return topology.generation;
}

// Ingests one fresh congestion observation against the topology that is
// installed right now.  A restart always drops congestion evidence, so a case
// that plans after a restart has to supply one.
EvidenceGeneration ingest_fresh_congestion(CoordinatorServer& server, ClientSession& session,
                                           const ctf::test::RackFabric& fabric, std::uint64_t seed) {
  const TopologyGeneration topology_generation = server.service().authority().topology_generation();
  CTF_CHECK(topology_generation.is_some());
  CongestionEvidence congestion = ctf::test::make_congestion(fabric.topology, seed, 1000, 2000);
  congestion.generation = mint_identity();
  congestion.topology_generation = topology_generation;
  congestion.captured_monotonic_ms = transport::monotonic_now_ms();
  CTF_CHECK_OK(session.ingest_congestion(congestion));
  CTF_CHECK_EQ(server.service().authority().congestion_generation(), congestion.generation);
  return congestion.generation;
}

// Publishes one participant over the wire and proves the coordinator recorded it
// as live and bound to the session that sent it.
void publish_participant(CoordinatorServer& server, ClientSession& publisher,
                         const ParticipantId& participant, const BootIncarnation& incarnation,
                         const NodeId& node) {
  CTF_CHECK_OK(publisher.heartbeat(participant, incarnation, node, ParticipantState::kLive));
  const PeerLiveness liveness = server.service().authority().peer_snapshot(participant);
  CTF_CHECK_MSG(liveness.is_current(transport::monotonic_now_ms()),
                "the coordinator did not record the published participant as live");
  CTF_CHECK_EQ(liveness.bound_session, publisher.hello().session);
  CTF_CHECK_EQ(liveness.incarnation, incarnation);
}

// Binds a request to the generation set the coordinator holds right now, which
// is what a peer learns from the acknowledgements of the observations it just
// sent.
DecisionRequest current_request(CoordinatorServer& server, const CollectiveInstance& instance,
                                const FlowGroupPlan& plan) {
  AuthorityState& authority = server.service().authority();
  DecisionRequest request;
  request.instance = instance;
  request.observed_epoch = server.service().epoch();
  request.observed_policy_generation = authority.policy_generation();
  request.observed_topology_generation = authority.topology_generation();
  request.observed_capacity_generation = authority.capacity_generation();
  request.observed_congestion_generation = authority.congestion_generation();
  request.has_flow_group_plan = true;
  request.plan = plan;
  return request;
}

// Binds a request to exactly what a reconnecting peer's handshake acknowledged.
// After a restart that handshake reports the durable policy and topology and no
// capacity or congestion at all, which is what the coordinator holds.
DecisionRequest request_bound_to(const protocol::HelloAckPayload& hello, const CollectiveInstance& instance,
                                 const FlowGroupPlan& plan) {
  DecisionRequest request;
  request.instance = instance;
  request.observed_epoch = hello.epoch;
  request.observed_policy_generation = hello.policy_generation;
  request.observed_topology_generation = hello.topology_generation;
  request.observed_capacity_generation = hello.capacity_generation;
  request.observed_congestion_generation = hello.congestion_generation;
  request.has_flow_group_plan = true;
  request.plan = plan;
  return request;
}

CollectiveGeneration register_collective(ClientSession& client, const CollectiveDefinition& definition,
                                         RegistrationResult expected) {
  protocol::RegisterAckPayload ack;
  CTF_CHECK_OK(client.register_collective(definition, ack));
  CTF_CHECK_EQ(ack.result, static_cast<std::uint8_t>(expected));
  CTF_CHECK(ack.generation.is_some());
  return ack.generation;
}

}  // namespace

CTF_TEST("persistence_restart", "durable_state_survives_a_restart_and_dynamic_state_does_not") {
  TempSnapshot snapshot("durable");
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(401, 2, 2);
  const CollectiveId id = mint_identity();
  const ParticipantId first = mint_identity();
  const ParticipantId second = mint_identity();
  std::vector<ParticipantId> participants;
  participants.push_back(first);
  participants.push_back(second);
  CollectiveDefinition definition = ctf::test::make_definition(
      id, CollectiveClass::kAllReduce, participants, "durable-all-reduce", 1ull << 20);
  const CollectiveAttemptId attempt = mint_identity();

  CoordinatorEpoch first_epoch;
  CollectiveGeneration generation;
  PolicyGeneration policy_generation;
  TopologyGeneration topology_generation;
  {
    CoordinatorServer server(persistence_config(snapshot.path()));
    CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
    first_epoch = server.service().epoch();
    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "durable-peer",
                                transport::monotonic_now_ms()));
    policy_generation = install_standard_policy(client);
    topology_generation = ingest_synthetic_evidence(server, client, fabric, 409);
    publish_participant(server, client, first, mint_identity(), fabric.topology.nodes[0].id);
    publish_participant(server, client, second, mint_identity(), fabric.topology.nodes[1].id);

    generation = register_collective(client, definition, RegistrationResult::kRegistered);
    definition.generation = generation;
    protocol::AttemptAckPayload attempt_ack;
    CTF_CHECK_OK(client.begin_attempt(id, generation, attempt, 1ull << 20, attempt_ack));
    CTF_CHECK_EQ(attempt_ack.state, CollectiveState::kPlanning);

    // The whole path works before the restart, so "admitted again" below is a
    // real recovery rather than a path that never worked.
    CollectiveInstance instance;
    instance.id = id;
    instance.generation = generation;
    instance.attempt = attempt;
    DecisionRecord decision;
    CTF_CHECK_OK(client.plan_flow_group(
        current_request(server, instance,
                        ctf::test::make_ring_plan(instance, definition.participants, 1ull << 18)),
        decision));
    CTF_CHECK_EQ(decision.outcome, Outcome::kAdmitted);

    CTF_CHECK_OK(server.flush(transport::monotonic_now_ms()));
    SnapshotStore store(snapshot.path());
    CTF_CHECK(store.exists());
    disconnect(server, client);
    CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
  }
  {
    CoordinatorServer restored(persistence_config(snapshot.path()));
    CTF_CHECK_OK(restored.start(transport::monotonic_now_ms()));
    // A restart always advances the epoch: everything issued before it is stale.
    CTF_CHECK_NE(restored.service().epoch(), first_epoch);
    CTF_CHECK(restored.service().has_policy());
    CTF_CHECK_EQ(restored.service().authority().policy_generation(), policy_generation);
    CTF_CHECK_EQ(restored.service().authority().topology_generation(), topology_generation);
    // Capacity and congestion are observations, never durable state.
    CTF_CHECK(restored.service().authority().capacity_generation().is_none());
    CTF_CHECK(restored.service().authority().congestion_generation().is_none());

    const CollectiveRecord* record = restored.service().catalog().find(id);
    CTF_CHECK(record != nullptr);
    CTF_CHECK_EQ(record->definition.generation, generation);
    CTF_CHECK_EQ(record->definition.label, std::string("durable-all-reduce"));
    CTF_CHECK(record->definition.participants == definition.participants);
    CTF_CHECK_EQ(record->current_attempt, attempt);
    // The state at the durability point is what survives: the admitted plan
    // moved the record to active before the flush.
    CTF_CHECK_EQ(record->state, CollectiveState::kActive);

    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", restored.port(), "restored-peer",
                                transport::monotonic_now_ms()));
    const protocol::HelloAckPayload hello = client.hello();
    CTF_CHECK(hello.capacity_generation.is_none());
    CTF_CHECK(hello.congestion_generation.is_none());
    CTF_CHECK_EQ(hello.topology_generation, topology_generation);
    CTF_CHECK_EQ(hello.policy_generation, policy_generation);
    // Liveness is not durable either.
    CTF_CHECK_EQ(restored.service().authority().peer_count(), 0u);
    publish_participant(restored, client, first, mint_identity(), fabric.topology.nodes[0].id);
    publish_participant(restored, client, second, mint_identity(), fabric.topology.nodes[1].id);

    CollectiveInstance instance;
    instance.id = id;
    instance.generation = generation;
    instance.attempt = attempt;
    const DecisionRequest request = request_bound_to(
        hello, instance, ctf::test::make_ring_plan(instance, definition.participants, 1ull << 18));
    DecisionRecord decision;
    CTF_CHECK_OK(client.plan_flow_group(request, decision));
    CTF_CHECK_EQ(decision.outcome, Outcome::kRequiresReplan);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCongestionEvidence);
    CTF_CHECK_EQ(decision.code, ErrorCode::kValidationRateInvalid);

    // Fresh congestion evidence makes the same request admissible again.
    ingest_fresh_congestion(restored, client, fabric, 419);
    CTF_CHECK_OK(client.plan_flow_group(request, decision));
    CTF_CHECK_EQ(decision.outcome, Outcome::kAdmitted);
    CTF_CHECK_EQ(decision.collective_generation, generation);
    CTF_CHECK_EQ(decision.epoch, hello.epoch);
    CTF_CHECK_EQ(decision.topology_generation, topology_generation);

    disconnect(restored, client);
    CTF_CHECK_OK(restored.stop(transport::monotonic_now_ms()));
  }
}

CTF_TEST("persistence_restart", "a_generation_advance_is_preserved_across_a_restart") {
  TempSnapshot snapshot("generation");
  const CollectiveId id = mint_identity();
  std::vector<ParticipantId> participants;
  participants.push_back(mint_identity());
  participants.push_back(mint_identity());
  CollectiveDefinition first = ctf::test::make_definition(
      id, CollectiveClass::kAllReduce, participants, "generation-one", 1ull << 20);
  CollectiveDefinition second = first;
  second.label = "generation-two";
  second.logical_bytes = 2ull << 20;

  CollectiveGeneration first_generation;
  CollectiveGeneration second_generation;
  {
    CoordinatorServer server(persistence_config(snapshot.path()));
    CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "generation-peer",
                                transport::monotonic_now_ms()));
    first_generation = register_collective(client, first, RegistrationResult::kRegistered);
    second_generation = register_collective(client, second, RegistrationResult::kGenerationAdvanced);
    CTF_CHECK_NE(first_generation, second_generation);
    CTF_CHECK_EQ(server.service().counters().generation_advances, 1u);
    disconnect(server, client);
    CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
  }
  {
    CoordinatorServer restored(persistence_config(snapshot.path()));
    CTF_CHECK_OK(restored.start(transport::monotonic_now_ms()));
    const CollectiveRecord* record = restored.service().catalog().find(id);
    CTF_CHECK(record != nullptr);
    // The newer generation is the live one, not the generation that was
    // registered first and then superseded.
    CTF_CHECK_EQ(record->definition.generation, second_generation);
    CTF_CHECK_EQ(record->definition.label, std::string("generation-two"));
    CTF_CHECK_EQ(record->definition.logical_bytes, 2ull << 20);
    CTF_CHECK_EQ(record->generation_ancestry_depth, 2u);

    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", restored.port(), "generation-restored",
                                transport::monotonic_now_ms()));
    CollectiveInstance superseded;
    superseded.id = id;
    superseded.generation = first_generation;
    superseded.attempt = mint_identity();
    DecisionRecord decision;
    CTF_CHECK_OK(client.plan_flow_group(
        current_request(restored, superseded,
                        ctf::test::make_ring_plan(superseded, first.participants, 1ull << 18)),
        decision));
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCollectiveGeneration);
    CTF_CHECK_EQ(decision.code, ErrorCode::kCompletionStaleGeneration);

    // The preserved generation is usable: it accepts a new attempt.
    const CollectiveAttemptId attempt = mint_identity();
    protocol::AttemptAckPayload attempt_ack;
    CTF_CHECK_OK(client.begin_attempt(id, second_generation, attempt, 1ull << 20, attempt_ack));
    CTF_CHECK_EQ(attempt_ack.state, CollectiveState::kPlanning);

    disconnect(restored, client);
    CTF_CHECK_OK(restored.stop(transport::monotonic_now_ms()));
  }
}

CTF_TEST("persistence_restart", "liveness_is_not_resurrected_by_a_restart") {
  TempSnapshot snapshot("liveness");
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(421, 2, 2);
  const CollectiveId id = mint_identity();
  const ParticipantId first = mint_identity();
  const ParticipantId second = mint_identity();
  std::vector<ParticipantId> participants;
  participants.push_back(first);
  participants.push_back(second);
  CollectiveDefinition definition = ctf::test::make_definition(
      id, CollectiveClass::kAllReduce, participants, "liveness-all-reduce", 1ull << 20);
  const CollectiveAttemptId attempt = mint_identity();
  CollectiveGeneration generation;
  {
    CoordinatorServer server(persistence_config(snapshot.path()));
    CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "liveness-peer",
                                transport::monotonic_now_ms()));
    install_standard_policy(client);
    ingest_synthetic_evidence(server, client, fabric, 431);
    publish_participant(server, client, first, mint_identity(), fabric.topology.nodes[0].id);
    publish_participant(server, client, second, mint_identity(), fabric.topology.nodes[1].id);
    generation = register_collective(client, definition, RegistrationResult::kRegistered);
    protocol::AttemptAckPayload attempt_ack;
    CTF_CHECK_OK(client.begin_attempt(id, generation, attempt, 1ull << 20, attempt_ack));
    CTF_CHECK_EQ(attempt_ack.state, CollectiveState::kPlanning);
    CTF_CHECK_EQ(server.service().live_peer_count(), 2u);
    disconnect(server, client);
    CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
  }
  {
    CoordinatorServer restored(persistence_config(snapshot.path()));
    CTF_CHECK_OK(restored.start(transport::monotonic_now_ms()));
    // No participant is live after a restart, and none is inherited.
    CTF_CHECK_EQ(restored.service().authority().peer_count(), 0u);
    CTF_CHECK_EQ(restored.service().live_peer_count(), 0u);

    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", restored.port(), "liveness-restored",
                                transport::monotonic_now_ms()));
    const protocol::HelloAckPayload hello = client.hello();
    CollectiveInstance instance;
    instance.id = id;
    instance.generation = generation;
    instance.attempt = attempt;
    const DecisionRequest request = request_bound_to(
        hello, instance, ctf::test::make_ring_plan(instance, definition.participants, 1ull << 18));

    // The definition and its attempt survived, so the refusal is about liveness
    // and about nothing else.
    DecisionRecord decision;
    CTF_CHECK_OK(client.plan_flow_group(request, decision));
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kParticipantLiveness);
    CTF_CHECK_EQ(decision.code, ErrorCode::kEnvelopeIncarnationStale);

    // Until the participants publish again, and fresh congestion arrives.
    publish_participant(restored, client, first, mint_identity(), fabric.topology.nodes[0].id);
    publish_participant(restored, client, second, mint_identity(), fabric.topology.nodes[1].id);
    ingest_fresh_congestion(restored, client, fabric, 433);
    CTF_CHECK_OK(client.plan_flow_group(request, decision));
    CTF_CHECK_EQ(decision.outcome, Outcome::kAdmitted);
    CTF_CHECK(decision.all_members_live);

    disconnect(restored, client);
    CTF_CHECK_OK(restored.stop(transport::monotonic_now_ms()));
  }
}

CTF_TEST("persistence_restart", "an_abortive_stop_leaves_a_complete_snapshot_or_none") {
  TempSnapshot snapshot("abortive");
  const CollectiveId id = mint_identity();
  std::vector<ParticipantId> participants;
  participants.push_back(mint_identity());
  participants.push_back(mint_identity());
  CollectiveDefinition definition = ctf::test::make_definition(
      id, CollectiveClass::kAllReduce, participants, "abortive-all-reduce", 1ull << 20);
  CollectiveGeneration generation;
  {
    CoordinatorServer server(persistence_config(snapshot.path()));
    CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "abortive-peer",
                                transport::monotonic_now_ms()));
    generation = register_collective(client, definition, RegistrationResult::kRegistered);
    // No flush request is made: stop() alone must leave either a complete
    // snapshot or no file at all, never a partial one.
    disconnect(server, client);
    CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
  }

  SnapshotStore store(snapshot.path());
  SnapshotContents contents;
  std::string diagnostic;
  const LoadDisposition disposition = store.load(contents, &diagnostic);
  CTF_CHECK_MSG(disposition == LoadDisposition::kMissing || disposition == LoadDisposition::kLoaded,
                diagnostic);
  if (disposition == LoadDisposition::kLoaded) {
    CTF_CHECK_EQ(contents.records.size(), 1u);
    CTF_CHECK_EQ(contents.records.front().definition.id, id);
    CTF_CHECK_EQ(contents.records.front().definition.generation, generation);
    CTF_CHECK_EQ(contents.records.front().state, CollectiveState::kRegistered);
  }
}

CTF_TEST("persistence_restart", "a_corrupt_snapshot_is_reported_and_replaced_by_a_valid_one") {
  TempSnapshot snapshot("corrupt");
  snapshot.write_random_bytes(0x5A5A5A5A5A5A5A5Aull);
  const CollectiveId id = mint_identity();
  std::vector<ParticipantId> participants;
  participants.push_back(mint_identity());
  participants.push_back(mint_identity());
  CollectiveDefinition definition = ctf::test::make_definition(
      id, CollectiveClass::kAllReduce, participants, "corrupt-all-reduce", 1ull << 20);
  CollectiveGeneration generation;
  {
    CoordinatorServer server(persistence_config(snapshot.path()));
    // A snapshot that exists but cannot be believed never prevents a start.
    CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
    // The durable state is absent rather than half applied.
    CTF_CHECK_EQ(server.service().catalog().size(), 0u);
    LoadDisposition disposition = LoadDisposition::kLoaded;
    const Status restored = server.restore(transport::monotonic_now_ms(), disposition);
    CTF_CHECK_MSG(!restored.is_ok(), "a corrupt snapshot was accepted");
    CTF_CHECK(disposition != LoadDisposition::kLoaded);
    CTF_CHECK(disposition != LoadDisposition::kMissing);

    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "corrupt-peer",
                                transport::monotonic_now_ms()));
    generation = register_collective(client, definition, RegistrationResult::kRegistered);
    // A subsequent flush writes a complete snapshot over the rejected file.
    CTF_CHECK_OK(server.flush(transport::monotonic_now_ms()));
    disconnect(server, client);
    CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
  }

  SnapshotStore store(snapshot.path());
  SnapshotContents contents;
  std::string diagnostic;
  CTF_CHECK_MSG(store.load(contents, &diagnostic) == LoadDisposition::kLoaded, diagnostic);
  CTF_CHECK_EQ(contents.records.size(), 1u);
  CTF_CHECK_EQ(contents.records.front().definition.id, id);
  CTF_CHECK_EQ(contents.records.front().definition.generation, generation);
}

CTF_TEST("persistence_restart", "a_cancelled_collective_is_still_terminal_after_a_restart") {
  TempSnapshot snapshot("cancelled");
  const CollectiveId id = mint_identity();
  std::vector<ParticipantId> participants;
  participants.push_back(mint_identity());
  participants.push_back(mint_identity());
  CollectiveDefinition definition = ctf::test::make_definition(
      id, CollectiveClass::kAllReduce, participants, "cancelled-all-reduce", 1ull << 20);
  const CollectiveAttemptId attempt = mint_identity();
  CollectiveGeneration generation;
  {
    CoordinatorServer server(persistence_config(snapshot.path()));
    CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "cancelled-peer",
                                transport::monotonic_now_ms()));
    install_standard_policy(client);
    generation = register_collective(client, definition, RegistrationResult::kRegistered);
    definition.generation = generation;
    protocol::AttemptAckPayload attempt_ack;
    CTF_CHECK_OK(client.begin_attempt(id, generation, attempt, 1ull << 20, attempt_ack));
    CTF_CHECK_EQ(attempt_ack.state, CollectiveState::kPlanning);
    protocol::LifecycleAckPayload cancellation;
    CTF_CHECK_OK(client.cancel(id, generation, attempt, cancellation));
    CTF_CHECK_EQ(cancellation.state, CollectiveState::kCancelled);
    CTF_CHECK_OK(server.flush(transport::monotonic_now_ms()));
    disconnect(server, client);
    CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
  }
  {
    CoordinatorServer restored(persistence_config(snapshot.path()));
    CTF_CHECK_OK(restored.start(transport::monotonic_now_ms()));
    const CollectiveRecord* record = restored.service().catalog().find(id);
    CTF_CHECK(record != nullptr);
    CTF_CHECK_EQ(record->state, CollectiveState::kCancelled);
    CTF_CHECK_EQ(record->definition.generation, generation);
    CTF_CHECK_EQ(record->current_attempt, attempt);
    CTF_CHECK(!record->accepts_new_authority());

    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", restored.port(), "cancelled-restored",
                                transport::monotonic_now_ms()));
    const protocol::HelloAckPayload hello = client.hello();
    CollectiveInstance instance;
    instance.id = id;
    instance.generation = generation;
    instance.attempt = attempt;
    DecisionRecord decision;
    CTF_CHECK_OK(client.plan_flow_group(
        request_bound_to(hello, instance,
                         ctf::test::make_ring_plan(instance, definition.participants, 1ull << 18)),
        decision));
    CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
    CTF_CHECK_EQ(decision.axis, AuthorityAxis::kLifecycle);
    CTF_CHECK_EQ(decision.code, ErrorCode::kCompletionCancelled);

    // Neither a register nor a new attempt can restore it.
    protocol::RegisterAckPayload register_ack;
    CTF_CHECK_OK(client.register_collective(definition, register_ack));
    CTF_CHECK_EQ(register_ack.result, static_cast<std::uint8_t>(RegistrationResult::kRejectedTerminal));
    protocol::AttemptAckPayload attempt_ack;
    CTF_CHECK_OK(client.begin_attempt(id, generation, mint_identity(), 1ull << 20, attempt_ack));
    CTF_CHECK_EQ(attempt_ack.state, CollectiveState::kCancelled);
    CTF_CHECK(attempt_ack.reasons.contains("collective_terminal"));

    std::string body;
    CTF_CHECK_OK(client.inspect(1, id, 0, 64, body));
    CTF_CHECK_MSG(body.find("state: cancelled") != std::string::npos, body);

    disconnect(restored, client);
    CTF_CHECK_OK(restored.stop(transport::monotonic_now_ms()));
  }
}

CTF_TEST_MAIN()
