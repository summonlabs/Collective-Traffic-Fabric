// Collective Traffic Fabric - cancellation, retirement and stale completion
// proofs over real loopback TCP.
// Copyright 2026 Summon Software Labs.
//
// Every case here drives a real ctf::CoordinatorServer over real loopback TCP
// sockets with the strict protocol client.  Fabric evidence is SYNTHETIC
// (tests/support/synthetic.hpp): no physical fabric is measured anywhere.
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "ctf/catalog.hpp"
#include "ctf/coordinator.hpp"
#include "ctf/protocol.hpp"
#include "ctf/service.hpp"
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

CoordinatorConfig coordinator_config() {
  CoordinatorConfig config;
  config.transport.bind_host = "127.0.0.1";
  // Port 0 asks the operating system for an ephemeral port, so a case can never
  // collide with another coordinator on the same machine.
  config.transport.port = 0;
  config.service.coordinator_label = "ctf-integration-lifecycle";
  config.service.decision_history_capacity = 256;
  // Long enough that no participant can expire in the middle of a case.
  config.service.participant_ttl_ms = 60000;
  return config;
}

// Awaits the transport's own live session count.  A session is closed on the
// session's own thread, so a peer that has closed its socket cannot assume the
// coordinator has observed it yet.
void wait_for_sessions(CoordinatorServer& server, std::size_t expected) {
  for (int attempt = 0; attempt < kWaitAttempts && server.session_count() != expected; ++attempt) {
    std::this_thread::yield();
  }
  CTF_CHECK_MSG(server.session_count() == expected,
                "the coordinator did not reach the expected session count");
}

// Closes one client session and awaits the coordinator's teardown of it, so the
// case that follows sees exactly the accounting it expects.
void disconnect(CoordinatorServer& server, ClientSession& client, std::size_t remaining) {
  client.close();
  wait_for_sessions(server, remaining);
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Installs the production policy over the wire and reports the generation that
// is now in force.
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

// Ingests a complete SYNTHETIC evidence set over the wire on the supplied
// session.  A fresh topology drops the observations that described the previous
// one, so capacity and congestion are always supplied after it.
void ingest_synthetic_evidence(CoordinatorServer& server, ClientSession& session,
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
// sent.  The authority checks below are about lifecycle, not about staleness, so
// the request has to be current in every other respect.
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

// Registers a definition and returns the generation the coordinator assigned.
CollectiveGeneration register_collective(ClientSession& client, const CollectiveDefinition& definition,
                                         RegistrationResult expected) {
  protocol::RegisterAckPayload ack;
  CTF_CHECK_OK(client.register_collective(definition, ack));
  CTF_CHECK_EQ(ack.result, static_cast<std::uint8_t>(expected));
  CTF_CHECK(ack.generation.is_some());
  return ack.generation;
}

}  // namespace

CTF_TEST("integration_lifecycle", "cancel_is_terminal_and_a_later_completion_cannot_restore_it") {
  CoordinatorServer server(coordinator_config());
  CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "cancel-peer",
                              transport::monotonic_now_ms()));

  // Two participants are enough for a ring; this case never plans for real, so
  // it needs neither liveness nor evidence: the lifecycle refusal is decided
  // before any evidence is consulted.
  std::vector<ParticipantId> participants;
  participants.push_back(mint_identity());
  participants.push_back(mint_identity());
  CollectiveDefinition definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, participants, "cancel-me", 1ull << 20);
  const CollectiveGeneration generation =
      register_collective(client, definition, RegistrationResult::kRegistered);
  definition.generation = generation;

  const CollectiveAttemptId attempt = mint_identity();
  protocol::AttemptAckPayload attempt_ack;
  CTF_CHECK_OK(client.begin_attempt(definition.id, generation, attempt, 1ull << 20, attempt_ack));
  CTF_CHECK_EQ(attempt_ack.state, CollectiveState::kPlanning);

  protocol::LifecycleAckPayload cancellation;
  CTF_CHECK_OK(client.cancel(definition.id, generation, attempt, cancellation));
  CTF_CHECK_EQ(cancellation.id, definition.id);
  CTF_CHECK_EQ(cancellation.state, CollectiveState::kCancelled);
  CTF_CHECK_EQ(server.service().counters().cancellations, 1u);

  // A later completion for the same generation is refused.  The refusal is
  // carried by the acknowledgement, not by a dropped connection: the peer learns
  // the state it is actually in.
  const CollectiveAttemptId later_attempt = mint_identity();
  protocol::AttemptAckPayload later_ack;
  CTF_CHECK_OK(client.begin_attempt(definition.id, generation, later_attempt, 1ull << 20, later_ack));
  CTF_CHECK_EQ(later_ack.state, CollectiveState::kCancelled);
  CTF_CHECK(later_ack.reasons.contains("collective_terminal"));

  // A flow group plan for the cancelled generation is refused on the lifecycle
  // axis, and it is refused as a decision rather than as a broken session.
  CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = generation;
  instance.attempt = attempt;
  DecisionRecord decision;
  CTF_CHECK_OK(client.plan_flow_group(
      current_request(server, instance,
                      ctf::test::make_ring_plan(instance, definition.participants, 1ull << 18)),
      decision));
  CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(decision.axis, AuthorityAxis::kLifecycle);
  CTF_CHECK_EQ(decision.code, ErrorCode::kCompletionCancelled);

  // The definition stays cancelled: it is not reverted, and it accepts no new
  // authority.
  const CollectiveRecord* record = server.service().catalog().find(definition.id);
  CTF_CHECK(record != nullptr);
  CTF_CHECK_EQ(record->state, CollectiveState::kCancelled);
  CTF_CHECK_EQ(record->definition.generation, generation);
  CTF_CHECK(!record->accepts_new_authority());
  std::string body;
  CTF_CHECK_OK(client.inspect(1, definition.id, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "state: cancelled"), body);
  CTF_CHECK_MSG(contains(body, "accepts_new_authority: no"), body);

  disconnect(server, client, 0u);
  CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
}

CTF_TEST("integration_lifecycle", "retire_is_terminal_and_never_resurrected_by_registration") {
  CoordinatorServer server(coordinator_config());
  CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "retire-peer",
                              transport::monotonic_now_ms()));

  std::vector<ParticipantId> participants;
  participants.push_back(mint_identity());
  participants.push_back(mint_identity());
  CollectiveDefinition definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllGather, participants, "retire-me", 1ull << 20);
  const CollectiveGeneration generation =
      register_collective(client, definition, RegistrationResult::kRegistered);
  definition.generation = generation;

  const CollectiveAttemptId attempt = mint_identity();
  protocol::AttemptAckPayload attempt_ack;
  CTF_CHECK_OK(client.begin_attempt(definition.id, generation, attempt, 1ull << 20, attempt_ack));
  CTF_CHECK_EQ(attempt_ack.state, CollectiveState::kPlanning);

  protocol::LifecycleAckPayload retirement;
  CTF_CHECK_OK(client.retire(definition.id, generation, attempt, retirement));
  CTF_CHECK_EQ(retirement.state, CollectiveState::kRetired);
  CTF_CHECK_EQ(server.service().counters().retirements, 1u);

  // A later register with the same identity is refused, and it is refused as a
  // terminal definition rather than as a duplicate or a capacity problem.
  protocol::RegisterAckPayload unchanged_ack;
  CTF_CHECK_OK(client.register_collective(definition, unchanged_ack));
  CTF_CHECK_EQ(unchanged_ack.result, static_cast<std::uint8_t>(RegistrationResult::kRejectedTerminal));
  CTF_CHECK(unchanged_ack.reasons.contains("state_collective_retired"));
  // A refused registration mints and reports no generation at all; the stored
  // generation below is the one that must be unchanged.
  CTF_CHECK(unchanged_ack.generation.is_none());

  // A *changed* definition under the same identity is refused just the same: a
  // terminal collective is never resurrected by redefining it.
  CollectiveDefinition changed = definition;
  changed.label = "retire-me-relabelled";
  changed.logical_bytes = 2ull << 20;
  protocol::RegisterAckPayload changed_ack;
  CTF_CHECK_OK(client.register_collective(changed, changed_ack));
  CTF_CHECK_EQ(changed_ack.result, static_cast<std::uint8_t>(RegistrationResult::kRejectedTerminal));
  CTF_CHECK(changed_ack.generation.is_none());

  const CollectiveRecord* record = server.service().catalog().find(definition.id);
  CTF_CHECK(record != nullptr);
  CTF_CHECK_EQ(record->state, CollectiveState::kRetired);
  CTF_CHECK_EQ(record->definition.generation, generation);
  CTF_CHECK_EQ(record->definition.label, std::string("retire-me"));
  CTF_CHECK_EQ(record->definition.logical_bytes, 1ull << 20);
  CTF_CHECK_EQ(server.service().counters().generation_advances, 0u);

  // A later completion cannot restore it either.
  const CollectiveAttemptId later_attempt = mint_identity();
  protocol::AttemptAckPayload later_ack;
  CTF_CHECK_OK(client.begin_attempt(definition.id, generation, later_attempt, 1ull << 20, later_ack));
  CTF_CHECK_EQ(later_ack.state, CollectiveState::kRetired);
  CTF_CHECK(later_ack.reasons.contains("collective_terminal"));

  CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = generation;
  instance.attempt = attempt;
  DecisionRecord decision;
  CTF_CHECK_OK(client.plan_flow_group(
      current_request(server, instance,
                      ctf::test::make_ring_plan(instance, definition.participants, 1ull << 18)),
      decision));
  CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(decision.axis, AuthorityAxis::kLifecycle);
  CTF_CHECK_EQ(decision.code, ErrorCode::kCompletionRetired);

  std::string body;
  CTF_CHECK_OK(client.inspect(1, definition.id, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "state: retired"), body);
  CTF_CHECK_MSG(contains(body, "accepts_new_authority: no"), body);

  disconnect(server, client, 0u);
  CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
}

CTF_TEST("integration_lifecycle", "a_stale_generation_completion_is_refused_and_counted") {
  CoordinatorServer server(coordinator_config());
  CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "generation-peer",
                              transport::monotonic_now_ms()));

  std::vector<ParticipantId> participants;
  participants.push_back(mint_identity());
  participants.push_back(mint_identity());
  CollectiveDefinition first = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, participants, "generation-one", 1ull << 20);
  const CollectiveGeneration first_generation =
      register_collective(client, first, RegistrationResult::kRegistered);

  // A membership or semantics change mints a new generation and stales every
  // decision bound to the previous one.
  CollectiveDefinition second = first;
  second.label = "generation-two";
  const CollectiveGeneration second_generation =
      register_collective(client, second, RegistrationResult::kGenerationAdvanced);
  CTF_CHECK_NE(first_generation, second_generation);
  CTF_CHECK_EQ(server.service().counters().generation_advances, 1u);

  const CollectiveAttemptId stale_attempt = mint_identity();
  CollectiveInstance instance;
  instance.id = first.id;
  instance.generation = first_generation;
  instance.attempt = stale_attempt;
  DecisionRecord decision;
  CTF_CHECK_OK(client.plan_flow_group(
      current_request(server, instance,
                      ctf::test::make_ring_plan(instance, first.participants, 1ull << 18)),
      decision));
  CTF_CHECK_EQ(decision.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(decision.axis, AuthorityAxis::kCollectiveGeneration);
  CTF_CHECK_EQ(decision.code, ErrorCode::kCompletionStaleGeneration);
  CTF_CHECK_EQ(server.service().counters().stale_completions_rejected, 1u);

  // The attempt path refuses the same stale generation, and reports the state
  // the collective is actually in.
  protocol::AttemptAckPayload stale_ack;
  CTF_CHECK_OK(client.begin_attempt(first.id, first_generation, stale_attempt, 1ull << 20, stale_ack));
  CTF_CHECK_EQ(stale_ack.state, CollectiveState::kRegistered);
  CTF_CHECK(stale_ack.reasons.contains("attempt_generation_stale"));
  CTF_CHECK_EQ(server.service().counters().stale_completions_rejected, 2u);

  // The newer generation is the live one, and it accepts a new attempt.
  const CollectiveAttemptId current_attempt = mint_identity();
  protocol::AttemptAckPayload current_ack;
  CTF_CHECK_OK(client.begin_attempt(first.id, second_generation, current_attempt, 1ull << 20, current_ack));
  CTF_CHECK_EQ(current_ack.state, CollectiveState::kPlanning);
  const CollectiveRecord* record = server.service().catalog().find(first.id);
  CTF_CHECK(record != nullptr);
  CTF_CHECK_EQ(record->definition.generation, second_generation);
  CTF_CHECK_EQ(record->definition.label, std::string("generation-two"));

  disconnect(server, client, 0u);
  CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
}

CTF_TEST("integration_lifecycle", "authority_is_never_reused_across_attempts") {
  CoordinatorServer server(coordinator_config());
  CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(331, 2, 2);
  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "attempt-peer",
                              transport::monotonic_now_ms()));

  // This case plans for real, so the fabric needs a policy, a topology, current
  // capacity and congestion, and live participants.
  install_standard_policy(client);
  ingest_synthetic_evidence(server, client, fabric, 337);
  const ParticipantId first = mint_identity();
  const ParticipantId second = mint_identity();
  publish_participant(server, client, first, mint_identity(), fabric.topology.nodes[0].id);
  publish_participant(server, client, second, mint_identity(), fabric.topology.nodes[1].id);
  CTF_CHECK_EQ(server.service().live_peer_count(), 2u);

  std::vector<ParticipantId> participants;
  participants.push_back(first);
  participants.push_back(second);
  CollectiveDefinition definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, participants, "attempt-authority", 1ull << 20);
  const CollectiveGeneration generation =
      register_collective(client, definition, RegistrationResult::kRegistered);
  definition.generation = generation;

  const CollectiveAttemptId first_attempt = mint_identity();
  const CollectiveAttemptId second_attempt = mint_identity();
  protocol::AttemptAckPayload first_ack;
  CTF_CHECK_OK(client.begin_attempt(definition.id, generation, first_attempt, 1ull << 20, first_ack));
  CTF_CHECK_EQ(first_ack.state, CollectiveState::kPlanning);
  protocol::AttemptAckPayload second_ack;
  CTF_CHECK_OK(client.begin_attempt(definition.id, generation, second_attempt, 1ull << 20, second_ack));
  CTF_CHECK_EQ(second_ack.previous_attempt, first_attempt);
  CTF_CHECK_EQ(second_ack.state, CollectiveState::kPlanning);

  // A plan bound to the superseded attempt is refused on the attempt axis: a
  // phase transition cannot reuse the authority of the attempt it replaced.
  CollectiveInstance stale_instance;
  stale_instance.id = definition.id;
  stale_instance.generation = generation;
  stale_instance.attempt = first_attempt;
  DecisionRecord stale_decision;
  CTF_CHECK_OK(client.plan_flow_group(
      current_request(server, stale_instance,
                      ctf::test::make_ring_plan(stale_instance, definition.participants, 1ull << 18)),
      stale_decision));
  CTF_CHECK_EQ(stale_decision.outcome, Outcome::kRejectedStale);
  CTF_CHECK_EQ(stale_decision.axis, AuthorityAxis::kAttemptGeneration);
  CTF_CHECK_EQ(stale_decision.code, ErrorCode::kCompletionStaleAttempt);
  CTF_CHECK_EQ(server.service().counters().stale_completions_rejected, 1u);

  // The current attempt is admitted: authority is reissued, never inherited.
  CollectiveInstance current_instance;
  current_instance.id = definition.id;
  current_instance.generation = generation;
  current_instance.attempt = second_attempt;
  DecisionRecord current_decision;
  CTF_CHECK_OK(client.plan_flow_group(
      current_request(server, current_instance,
                      ctf::test::make_ring_plan(current_instance, definition.participants, 1ull << 18)),
      current_decision));
  CTF_CHECK_EQ(current_decision.outcome, Outcome::kAdmitted);
  CTF_CHECK_EQ(current_decision.instance.attempt, second_attempt);
  CTF_CHECK_EQ(current_decision.collective_generation, generation);

  disconnect(server, client, 0u);
  CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
}

CTF_TEST("integration_lifecycle", "repeated_connect_and_disconnect_leave_exact_accounting") {
  CoordinatorServer server(coordinator_config());
  CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
  const std::uint16_t port = server.port();

  const int cycles = 25;
  for (int cycle = 0; cycle < cycles; ++cycle) {
    ClientSession client;
    CTF_CHECK_OK(client.connect("127.0.0.1", port, "cycle-peer", transport::monotonic_now_ms()));
    // A real round trip per session, so the cycle is a session and not just a
    // socket that was opened and abandoned.
    std::string body;
    CTF_CHECK_OK(client.inspect(0, CollectiveId{}, 0, 8, body));
    CTF_CHECK_MSG(contains(body, "running: yes"), body);
    disconnect(server, client, 0u);
  }
  CTF_CHECK_EQ(server.session_count(), 0u);
  CTF_CHECK_EQ(server.service().session_count(), 0u);
  const CoordinatorService::Counters counters = server.service().counters();
  CTF_CHECK_EQ(counters.sessions_opened, static_cast<std::uint64_t>(cycles));
  CTF_CHECK_EQ(counters.sessions_closed, static_cast<std::uint64_t>(cycles));

  // The coordinator still serves a fresh client correctly afterwards.
  ClientSession fresh;
  CTF_CHECK_OK(fresh.connect("127.0.0.1", port, "fresh-peer", transport::monotonic_now_ms()));
  std::string body;
  CTF_CHECK_OK(fresh.inspect(0, CollectiveId{}, 0, 8, body));
  CTF_CHECK_MSG(contains(body, "running: yes"), body);
  CTF_CHECK_MSG(contains(body, "sessions: 1 of"), body);
  disconnect(server, fresh, 0u);
  CTF_CHECK_EQ(server.service().counters().sessions_opened, static_cast<std::uint64_t>(cycles) + 1u);
  CTF_CHECK_EQ(server.service().counters().sessions_closed, static_cast<std::uint64_t>(cycles) + 1u);

  CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
}

CTF_TEST("integration_lifecycle", "cancelling_every_collective_leaves_the_coordinator_serving") {
  CoordinatorServer server(coordinator_config());
  CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "sweep-peer",
                              transport::monotonic_now_ms()));

  const ParticipantId member_a = mint_identity();
  const ParticipantId member_b = mint_identity();
  std::vector<ParticipantId> participants;
  participants.push_back(member_a);
  participants.push_back(member_b);

  // Two definitions of this case's own: one carries a live attempt, the other
  // has never been attempted, so the sweep below covers both shapes.
  CollectiveDefinition first = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, participants, "sweep-one", 1ull << 20);
  first.generation = register_collective(client, first, RegistrationResult::kRegistered);
  const CollectiveAttemptId first_attempt = mint_identity();
  protocol::AttemptAckPayload first_ack;
  CTF_CHECK_OK(client.begin_attempt(first.id, first.generation, first_attempt, 1ull << 20, first_ack));
  CTF_CHECK_EQ(first_ack.state, CollectiveState::kPlanning);

  CollectiveDefinition second = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kBroadcast, participants, "sweep-two", 1ull << 20);
  second.generation = register_collective(client, second, RegistrationResult::kRegistered);

  // Cancel every definition the coordinator still holds: after this, nothing it
  // knows about accepts new authority.
  const std::vector<CollectiveId> ids = server.service().catalog().ids(0, 64);
  CTF_CHECK(ids.size() >= 2u);
  std::size_t cancelled = 0;
  for (const CollectiveId& id : ids) {
    const CollectiveRecord* record = server.service().catalog().find(id);
    if (record == nullptr || collective_state_is_terminal(record->state)) continue;
    protocol::LifecycleAckPayload ack;
    CTF_CHECK_OK(client.cancel(id, record->definition.generation, record->current_attempt, ack));
    CTF_CHECK_EQ(ack.state, CollectiveState::kCancelled);
    ++cancelled;
  }
  CTF_CHECK(cancelled >= 2u);
  for (const CollectiveId& id : ids) {
    const CollectiveRecord* record = server.service().catalog().find(id);
    CTF_CHECK(record != nullptr);
    CTF_CHECK(collective_state_is_terminal(record->state));
    CTF_CHECK(!record->accepts_new_authority());
  }

  // The coordinator still answers every inspection subject.
  std::string body;
  CTF_CHECK_OK(client.inspect(0, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "running: yes"), body);
  CTF_CHECK_OK(client.inspect(1, first.id, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "state: cancelled"), body);
  CTF_CHECK_OK(client.inspect(2, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "outcomes:"), body);
  CTF_CHECK_OK(client.inspect(3, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "peers: "), body);
  CTF_CHECK_OK(client.inspect(4, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "policy:"), body);
  CTF_CHECK_OK(client.inspect(5, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "topology:"), body);

  // And a new collective registers normally, under a fresh identity.
  CollectiveDefinition third = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, participants, "sweep-three", 1ull << 20);
  third.generation = register_collective(client, third, RegistrationResult::kRegistered);
  const CollectiveAttemptId third_attempt = mint_identity();
  protocol::AttemptAckPayload third_ack;
  CTF_CHECK_OK(client.begin_attempt(third.id, third.generation, third_attempt, 1ull << 20, third_ack));
  CTF_CHECK_EQ(third_ack.state, CollectiveState::kPlanning);
  CTF_CHECK_OK(client.inspect(1, third.id, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "state: planning"), body);
  CTF_CHECK_MSG(contains(body, "accepts_new_authority: yes"), body);

  disconnect(server, client, 0u);
  CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
}

CTF_TEST_MAIN()
