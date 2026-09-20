// Collective Traffic Fabric - real loopback TCP integration proofs.
// Copyright 2026 Summon Software Labs.
//
// Every case here drives a real ctf::CoordinatorServer over real loopback TCP
// sockets with the strict protocol client.  The fabric evidence this suite
// ingests is SYNTHETIC: it is built by tests/support/synthetic.hpp, no physical
// fabric is measured, and no case claims a hardware result.
#include <algorithm>
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

// Bounded yields spent awaiting an asynchronous condition.  The transport and
// the service publish no blocking form of their own counters, so the condition
// itself is polled to a bound: this is not a sleep standing in for a condition,
// it is the condition, observed until it holds.
constexpr int kWaitAttempts = 200000;

CoordinatorConfig shared_config() {
  CoordinatorConfig config;
  config.transport.bind_host = "127.0.0.1";
  // Port 0 asks the operating system for an ephemeral port, so two runs of this
  // suite cannot collide on a fixed port.
  config.transport.port = 0;
  config.service.coordinator_label = "ctf-integration-loopback";
  config.service.decision_history_capacity = 512;
  // Long enough that no participant can expire in the middle of a case,
  // whatever the machine's scheduling does.
  config.service.participant_ttl_ms = 60000;
  return config;
}

// Awaits the transport's own live session count.
void wait_for_sessions(CoordinatorServer& server, std::size_t expected) {
  for (int attempt = 0; attempt < kWaitAttempts && server.session_count() != expected; ++attempt) {
    std::this_thread::yield();
  }
  CTF_CHECK_MSG(server.session_count() == expected,
                "the coordinator did not reach the expected session count");
}

// Awaits the coordinator's own shutdown flag.  A shutdown acknowledgement is
// written before the flag is stored, so a peer that has read the
// acknowledgement may still observe the flag unset.
void wait_for_shutdown_request(CoordinatorServer& server) {
  for (int attempt = 0; attempt < kWaitAttempts && !server.shutdown_requested(); ++attempt) {
    std::this_thread::yield();
  }
  CTF_CHECK_MSG(server.shutdown_requested(), "the coordinator never recorded the shutdown request");
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Publishes one participant over the wire and proves the coordinator recorded it
// as live, bound to the session that sent it and to nothing else.  The
// acknowledgement is written after the liveness table is updated, so the record
// is already in place when the strict client returns.
void publish_participant(CoordinatorServer& server, ClientSession& publisher,
                         const ParticipantId& participant, const BootIncarnation& incarnation,
                         const NodeId& node) {
  CTF_CHECK_OK(publisher.heartbeat(participant, incarnation, node, ParticipantState::kLive));
  const PeerLiveness liveness = server.service().authority().peer_snapshot(participant);
  CTF_CHECK_MSG(liveness.is_current(transport::monotonic_now_ms()),
                "the coordinator did not record the published participant as live");
  CTF_CHECK_EQ(liveness.bound_session, publisher.hello().session);
  CTF_CHECK_EQ(liveness.incarnation, incarnation);
  CTF_CHECK_EQ(liveness.node, node);
  CTF_CHECK_EQ(liveness.state, ParticipantState::kLive);
}

// Installs the production policy over the wire.  A policy is coordinator owned
// state, so the peer proposes it and the acknowledgement reports the generation
// that is now in force; the proposal leaves the generation unset so the
// coordinator mints it.
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
  CTF_CHECK(ack.generation.is_some());
  return ack.generation;
}

// Ingests a complete SYNTHETIC evidence set over the wire on a session of its
// own, then closes that session and awaits its teardown.  A fresh topology
// drops the observations that described the previous one, so capacity and
// congestion are always supplied after it.
void ingest_synthetic_evidence(CoordinatorServer& server, const ctf::test::RackFabric& fabric,
                               std::uint64_t seed) {
  const std::size_t baseline = server.session_count();
  const std::uint64_t now = transport::monotonic_now_ms();
  ClientSession session;
  CTF_CHECK_OK(session.connect("127.0.0.1", server.port(), "evidence", now));

  const PolicyGeneration policy_generation = install_standard_policy(session);
  CTF_CHECK_EQ(server.service().authority().policy_generation(), policy_generation);

  TopologyEvidence topology = fabric.topology;
  topology.generation = mint_identity();
  topology.captured_monotonic_ms = now;
  CTF_CHECK_OK(session.ingest_topology(topology));
  // The acknowledgement alone is not proof: an ingestion the coordinator
  // refused also arrives as an acknowledgement.  The installed generation is.
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

  session.close();
  wait_for_sessions(server, baseline);
}

// Binds a request to exactly the generations the handshake acknowledged.  A
// decision may only be issued for the generation set it names, so the request
// has to carry what the coordinator reported and nothing else.
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

// Builds one raw inspect request with an explicit flag set and frame sequence.
protocol::Frame raw_inspect_frame(const ClientSession& client, std::uint16_t flags,
                                 std::uint64_t frame_sequence) {
  protocol::Frame frame;
  frame.kind = static_cast<std::uint16_t>(protocol::MessageKind::kInspectRequest);
  frame.flags = flags;
  frame.envelope.session = client.hello().session;
  frame.envelope.incarnation = client.hello().incarnation;
  frame.envelope.epoch = client.hello().epoch;
  frame.envelope.frame_sequence = Sequence(frame_sequence);
  protocol::InspectRequestPayload payload;
  payload.subject = 0;
  payload.limit = 8;
  CTF_CHECK_OK(protocol::encode_inspect_request(payload, frame.payload));
  return frame;
}

// Sends one already built frame and requires the coordinator's error response,
// reporting the deterministic code that response carries.
Status send_and_read_error(ClientSession& client, const protocol::Frame& frame, ErrorCode& code) {
  Status status = client.connection().send_frame(frame);
  if (!status.is_ok()) return status;
  protocol::Frame response;
  status = client.connection().receive_frame(response);
  if (!status.is_ok()) return status;
  if (static_cast<protocol::MessageKind>(response.kind) != protocol::MessageKind::kErrorResponse) {
    return Status(ErrorCode::kInternalInvariant, "the coordinator did not answer with an error response");
  }
  protocol::ErrorPayload error;
  status = protocol::decode_error(response.payload, error);
  if (!status.is_ok()) return status;
  code = error.code;
  return Status::ok();
}

// The coordinator the read-mostly cases share.  The session limit case builds
// its own instance so the bound it configures cannot affect this one, and the
// shutdown case is registered last because accepting a shutdown request is a
// one way transition for the process that hosts the coordinator.
class SharedCoordinator {
 public:
  SharedCoordinator() : server_(shared_config()) {
    CTF_CHECK_OK(server_.start(transport::monotonic_now_ms()));
  }
  ~SharedCoordinator() {
    if (server_.is_running()) {
      static_cast<void>(server_.stop(transport::monotonic_now_ms()));
    }
  }
  SharedCoordinator(const SharedCoordinator&) = delete;
  SharedCoordinator& operator=(const SharedCoordinator&) = delete;
  [[nodiscard]] CoordinatorServer& server() noexcept { return server_; }

 private:
  CoordinatorServer server_;
};

CoordinatorServer& shared_coordinator() {
  static SharedCoordinator shared;
  // The shutdown case stops the shared coordinator.  A case selected on its own
  // by a filter must still find a serving coordinator, and a boot always drops
  // the liveness and observations of the previous one.
  if (!shared.server().is_running()) {
    CTF_CHECK_OK(shared.server().start(transport::monotonic_now_ms()));
  }
  return shared.server();
}

}  // namespace

CTF_TEST("integration_loopback", "hello_ack_carries_session_incarnation_and_epoch") {
  CoordinatorServer& server = shared_coordinator();
  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "hello-probe", transport::monotonic_now_ms()));

  const protocol::HelloAckPayload& hello = client.hello();
  // The coordinator mints the session and the incarnation; the peer never
  // supplies an identity that the coordinator then believes.
  CTF_CHECK(hello.session.is_some());
  CTF_CHECK(hello.incarnation.is_some());
  CTF_CHECK_EQ(hello.epoch, server.service().epoch());
  CTF_CHECK(server.service().authority().epoch_is_current(hello.epoch));
  CTF_CHECK_EQ(hello.coordinator_label, std::string("ctf-integration-loopback"));
  CTF_CHECK_EQ(hello.max_frame_payload_bytes, limits::kFramePayloadMaxBytes);
  CTF_CHECK_EQ(server.session_count(), 1u);

  client.close();
  wait_for_sessions(server, 0u);
  CTF_CHECK_EQ(server.service().session_count(), 0u);
}

CTF_TEST("integration_loopback", "supported_path_is_admitted_with_hello_bound_generations") {
  CoordinatorServer& server = shared_coordinator();
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(211, 2, 2);

  // Evidence first: the handshake that follows has to be able to report the
  // generations the decision will bind, so the policy and every observation are
  // installed over the wire before the workload session connects.
  ingest_synthetic_evidence(server, fabric, 213);

  const ParticipantId first = mint_identity();
  const ParticipantId second = mint_identity();
  ClientSession members;
  CTF_CHECK_OK(members.connect("127.0.0.1", server.port(), "loopback-members",
                              transport::monotonic_now_ms()));
  publish_participant(server, members, first, mint_identity(), fabric.topology.nodes[0].id);
  publish_participant(server, members, second, mint_identity(), fabric.topology.nodes[1].id);
  CTF_CHECK_EQ(server.service().live_peer_count(), 2u);

  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", server.port(), "loopback-workload",
                              transport::monotonic_now_ms()));
  const protocol::HelloAckPayload hello = client.hello();
  CTF_CHECK_EQ(hello.epoch, server.service().epoch());
  CTF_CHECK_EQ(hello.policy_generation, server.service().authority().policy_generation());
  CTF_CHECK_EQ(hello.topology_generation, server.service().authority().topology_generation());
  CTF_CHECK_EQ(hello.capacity_generation, server.service().authority().capacity_generation());
  CTF_CHECK_EQ(hello.congestion_generation, server.service().authority().congestion_generation());

  std::vector<ParticipantId> participants;
  participants.push_back(first);
  participants.push_back(second);
  CollectiveDefinition definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, participants, "loopback-all-reduce", 1ull << 20);

  protocol::RegisterAckPayload registration;
  CTF_CHECK_OK(client.register_collective(definition, registration));
  CTF_CHECK_EQ(registration.id, definition.id);
  CTF_CHECK_EQ(registration.result, static_cast<std::uint8_t>(RegistrationResult::kRegistered));
  CTF_CHECK(registration.generation.is_some());
  definition.generation = registration.generation;

  const CollectiveAttemptId attempt = mint_identity();
  protocol::AttemptAckPayload attempt_ack;
  CTF_CHECK_OK(client.begin_attempt(definition.id, registration.generation, attempt, 1ull << 20,
                                    attempt_ack));
  CTF_CHECK_EQ(attempt_ack.id, definition.id);
  CTF_CHECK_EQ(attempt_ack.attempt, attempt);
  CTF_CHECK_EQ(attempt_ack.state, CollectiveState::kPlanning);

  CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = registration.generation;
  instance.attempt = attempt;
  const FlowGroupPlan plan = ctf::test::make_ring_plan(instance, definition.participants, 1ull << 18);
  const DecisionRequest request = request_bound_to(hello, instance, plan);

  DecisionRecord decision;
  CTF_CHECK_OK(client.plan_flow_group(request, decision));
  CTF_CHECK_EQ(decision.outcome, Outcome::kAdmitted);
  CTF_CHECK_EQ(decision.collective_generation, registration.generation);
  CTF_CHECK_EQ(decision.instance.attempt, attempt);
  CTF_CHECK(decision.flow_group_constructed);
  CTF_CHECK(decision.flow_group.is_some());
  CTF_CHECK_EQ(decision.group.edges.size(), 2u);
  CTF_CHECK(decision.participants == definition.participants);
  CTF_CHECK(decision.all_members_live);
  CTF_CHECK(decision.capacity_evidence_used);
  CTF_CHECK(decision.congestion_evidence_used);
  // Every generation the decision binds is one the handshake acknowledged.
  CTF_CHECK_EQ(decision.epoch, hello.epoch);
  CTF_CHECK_EQ(decision.policy_generation, hello.policy_generation);
  CTF_CHECK_EQ(decision.topology_generation, hello.topology_generation);
  CTF_CHECK_EQ(decision.capacity_generation, hello.capacity_generation);
  CTF_CHECK_EQ(decision.congestion_generation, hello.congestion_generation);

  std::string body;
  CTF_CHECK_OK(client.inspect(0, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "coordinator: ctf-integration-loopback"), body);
  CTF_CHECK_MSG(contains(body, "running: yes"), body);
  CTF_CHECK_MSG(contains(body, "epoch: " + hello.epoch.to_string()), body);
  CTF_CHECK_MSG(contains(body, "topology_generation: " + hello.topology_generation.to_string()), body);
  CTF_CHECK_MSG(contains(body, "congestion_generation: " + hello.congestion_generation.to_string()), body);
  CTF_CHECK_MSG(contains(body, "collectives: "), body);

  CTF_CHECK_OK(client.inspect(1, definition.id, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "collective: " + definition.id.to_string()), body);
  CTF_CHECK_MSG(contains(body, "generation: " + registration.generation.to_string()), body);
  CTF_CHECK_MSG(contains(body, "state: active"), body);
  CTF_CHECK_MSG(contains(body, "accepts_new_authority: yes"), body);
  CTF_CHECK_MSG(contains(body, "class=all_reduce"), body);
  CTF_CHECK_MSG(contains(body, "participants=2"), body);

  CTF_CHECK_OK(client.inspect(2, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "decisions: "), body);
  CTF_CHECK_MSG(contains(body, definition.id.to_string()), body);
  CTF_CHECK_MSG(contains(body, "ADMITTED"), body);
  CTF_CHECK_MSG(contains(body, "outcomes:"), body);

  CTF_CHECK_OK(client.inspect(3, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "peers: 2"), body);
  CTF_CHECK_MSG(contains(body, first.to_string()), body);
  CTF_CHECK_MSG(contains(body, second.to_string()), body);
  CTF_CHECK_MSG(contains(body, "state=live"), body);

  CTF_CHECK_OK(client.inspect(4, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "policy: " + hello.policy_generation.to_string()), body);
  CTF_CHECK_MSG(contains(body, "collective=all_reduce"), body);
  CTF_CHECK_MSG(contains(body, "class=bulk_data"), body);

  CTF_CHECK_OK(client.inspect(5, CollectiveId{}, 0, 64, body));
  CTF_CHECK_MSG(contains(body, "topology: " + hello.topology_generation.to_string()), body);
  CTF_CHECK_MSG(contains(body, "SYNTHETIC"), body);
  CTF_CHECK_MSG(contains(body, "nodes: 4 links: 2"), body);

  // The explanation is rendered from the retained decision, so it must name the
  // same bound generations the decision itself carries.
  std::string explanation;
  CTF_CHECK_OK(client.explain(definition.id, attempt, explanation));
  CTF_CHECK_MSG(contains(explanation, "outcome: ADMITTED"), explanation);
  CTF_CHECK_MSG(contains(explanation, "  collective_generation=" + decision.collective_generation.to_string()),
                explanation);
  CTF_CHECK_MSG(contains(explanation, "  topology_generation=" + decision.topology_generation.to_string()),
                explanation);
  CTF_CHECK_MSG(contains(explanation, "  policy_generation=" + decision.policy_generation.to_string()),
                explanation);
  CTF_CHECK_MSG(contains(explanation, "  capacity_generation=" + decision.capacity_generation.to_string()),
                explanation);
  CTF_CHECK_MSG(contains(explanation, "  congestion_generation=" + decision.congestion_generation.to_string()),
                explanation);
  CTF_CHECK_MSG(contains(explanation, "  coordinator_epoch=" + decision.epoch.to_string()), explanation);

  client.close();
  members.close();
  wait_for_sessions(server, 0u);
}

CTF_TEST("integration_loopback", "two_clients_keep_definitions_decisions_and_liveness_separate") {
  CoordinatorServer& server = shared_coordinator();
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(223, 2, 2);
  ingest_synthetic_evidence(server, fabric, 227);

  // Two peers, each publishing its own participant set on its own session.
  const ParticipantId alpha_first = mint_identity();
  const ParticipantId alpha_second = mint_identity();
  const ParticipantId beta_first = mint_identity();
  const ParticipantId beta_second = mint_identity();
  ClientSession client_alpha;
  CTF_CHECK_OK(client_alpha.connect("127.0.0.1", server.port(), "peer-alpha",
                                    transport::monotonic_now_ms()));
  publish_participant(server, client_alpha, alpha_first, mint_identity(), fabric.topology.nodes[0].id);
  publish_participant(server, client_alpha, alpha_second, mint_identity(), fabric.topology.nodes[1].id);
  ClientSession client_beta;
  CTF_CHECK_OK(client_beta.connect("127.0.0.1", server.port(), "peer-beta",
                                   transport::monotonic_now_ms()));
  publish_participant(server, client_beta, beta_first, mint_identity(), fabric.topology.nodes[2].id);
  publish_participant(server, client_beta, beta_second, mint_identity(), fabric.topology.nodes[3].id);

  // Each participant is attributed to the session that published it, and to no
  // other session.
  CTF_CHECK_NE(client_alpha.hello().session, client_beta.hello().session);
  CTF_CHECK_EQ(server.service().authority().peer_snapshot(alpha_first).bound_session,
               client_alpha.hello().session);
  CTF_CHECK_EQ(server.service().authority().peer_snapshot(alpha_second).bound_session,
               client_alpha.hello().session);
  CTF_CHECK_EQ(server.service().authority().peer_snapshot(beta_first).bound_session,
               client_beta.hello().session);
  CTF_CHECK_EQ(server.service().authority().peer_snapshot(beta_second).bound_session,
               client_beta.hello().session);
  const protocol::HelloAckPayload hello_alpha = client_alpha.hello();
  const protocol::HelloAckPayload hello_beta = client_beta.hello();

  std::vector<ParticipantId> alpha_members;
  alpha_members.push_back(alpha_first);
  alpha_members.push_back(alpha_second);
  std::vector<ParticipantId> beta_members;
  beta_members.push_back(beta_first);
  beta_members.push_back(beta_second);
  CollectiveDefinition alpha_definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, alpha_members, "alpha-all-reduce", 1ull << 20);
  CollectiveDefinition beta_definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kReduceScatter, beta_members, "beta-reduce-scatter", 1ull << 20);
  CTF_CHECK_NE(alpha_definition.id, beta_definition.id);

  protocol::RegisterAckPayload alpha_registration;
  CTF_CHECK_OK(client_alpha.register_collective(alpha_definition, alpha_registration));
  CTF_CHECK_EQ(alpha_registration.result, static_cast<std::uint8_t>(RegistrationResult::kRegistered));
  protocol::RegisterAckPayload beta_registration;
  CTF_CHECK_OK(client_beta.register_collective(beta_definition, beta_registration));
  CTF_CHECK_EQ(beta_registration.result, static_cast<std::uint8_t>(RegistrationResult::kRegistered));
  alpha_definition.generation = alpha_registration.generation;
  beta_definition.generation = beta_registration.generation;

  // Both durable definitions coexist in one catalog, each under its own
  // generation and with its own participant set.
  const CollectiveRecord* alpha_record = server.service().catalog().find(alpha_definition.id);
  const CollectiveRecord* beta_record = server.service().catalog().find(beta_definition.id);
  CTF_CHECK(alpha_record != nullptr);
  CTF_CHECK(beta_record != nullptr);
  CTF_CHECK_EQ(alpha_record->definition.generation, alpha_registration.generation);
  CTF_CHECK_EQ(beta_record->definition.generation, beta_registration.generation);
  CTF_CHECK(alpha_record->definition.participants == alpha_definition.participants);
  CTF_CHECK(beta_record->definition.participants == beta_definition.participants);

  const CollectiveAttemptId alpha_attempt = mint_identity();
  const CollectiveAttemptId beta_attempt = mint_identity();
  protocol::AttemptAckPayload alpha_attempt_ack;
  protocol::AttemptAckPayload beta_attempt_ack;
  CTF_CHECK_OK(client_alpha.begin_attempt(alpha_definition.id, alpha_registration.generation,
                                          alpha_attempt, 1ull << 20, alpha_attempt_ack));
  CTF_CHECK_OK(client_beta.begin_attempt(beta_definition.id, beta_registration.generation, beta_attempt,
                                         1ull << 20, beta_attempt_ack));
  CTF_CHECK_EQ(alpha_attempt_ack.state, CollectiveState::kPlanning);
  CTF_CHECK_EQ(beta_attempt_ack.state, CollectiveState::kPlanning);

  CollectiveInstance alpha_instance;
  alpha_instance.id = alpha_definition.id;
  alpha_instance.generation = alpha_registration.generation;
  alpha_instance.attempt = alpha_attempt;
  CollectiveInstance beta_instance;
  beta_instance.id = beta_definition.id;
  beta_instance.generation = beta_registration.generation;
  beta_instance.attempt = beta_attempt;

  DecisionRecord alpha_decision;
  DecisionRecord beta_decision;
  CTF_CHECK_OK(client_alpha.plan_flow_group(
      request_bound_to(hello_alpha, alpha_instance,
                       ctf::test::make_ring_plan(alpha_instance, alpha_definition.participants, 1ull << 18)),
      alpha_decision));
  CTF_CHECK_OK(client_beta.plan_flow_group(
      request_bound_to(hello_beta, beta_instance,
                       ctf::test::make_ring_plan(beta_instance, beta_definition.participants, 1ull << 18)),
      beta_decision));

  // Each decision is bound to its own collective, its own attempt and its own
  // participant set; neither carries a member of the other.
  CTF_CHECK_EQ(alpha_decision.outcome, Outcome::kAdmitted);
  CTF_CHECK_EQ(beta_decision.outcome, Outcome::kAdmitted);
  CTF_CHECK_EQ(alpha_decision.instance.id, alpha_definition.id);
  CTF_CHECK_EQ(beta_decision.instance.id, beta_definition.id);
  CTF_CHECK_EQ(alpha_decision.instance.attempt, alpha_attempt);
  CTF_CHECK_EQ(beta_decision.instance.attempt, beta_attempt);
  CTF_CHECK(alpha_decision.participants == alpha_definition.participants);
  CTF_CHECK(beta_decision.participants == beta_definition.participants);
  CTF_CHECK_NE(alpha_decision.flow_group, beta_decision.flow_group);
  CTF_CHECK(std::find(beta_decision.participants.begin(), beta_decision.participants.end(), alpha_first) ==
            beta_decision.participants.end());
  CTF_CHECK(std::find(alpha_decision.participants.begin(), alpha_decision.participants.end(), beta_first) ==
            alpha_decision.participants.end());
  CTF_CHECK_EQ(alpha_decision.epoch, hello_alpha.epoch);
  CTF_CHECK_EQ(beta_decision.epoch, hello_beta.epoch);

  client_alpha.close();
  client_beta.close();
  wait_for_sessions(server, 0u);
}

CTF_TEST("integration_loopback", "session_limit_closes_the_extra_connection_and_accounts_for_it") {
  // A second, separately configured coordinator: the session bound this case
  // needs must not change how the shared coordinator above behaves.
  CoordinatorConfig config = shared_config();
  config.transport.max_sessions = 1;
  config.service.coordinator_label = "ctf-integration-session-limit";
  CoordinatorServer server(config);
  CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
  const std::uint16_t port = server.port();

  ClientSession served;
  CTF_CHECK_OK(served.connect("127.0.0.1", port, "limit-served", transport::monotonic_now_ms()));
  CTF_CHECK_EQ(server.accepted_total(), 1u);
  CTF_CHECK_EQ(server.rejected_total(), 0u);
  CTF_CHECK_EQ(server.session_count(), 1u);

  // The transport accepts the socket and closes it immediately without ever
  // handing it to the coordinator, so the peer observes a closed connection
  // rather than a served session.
  ClientSession refused;
  const Status refused_status = refused.connect("127.0.0.1", port, "limit-refused",
                                               transport::monotonic_now_ms());
  CTF_CHECK_MSG(!refused_status.is_ok(), "the coordinator served a connection beyond its session bound");
  CTF_CHECK(refused_status.code() == ErrorCode::kTransportClosed ||
            refused_status.code() == ErrorCode::kTransportReceiveFailed ||
            refused_status.code() == ErrorCode::kTransportSendFailed ||
            refused_status.code() == ErrorCode::kTransportConnectFailed);
  // The refusal is accounted, never silently dropped, and the served session is
  // untouched: the session bound is a resource limit, not a defect.
  CTF_CHECK_EQ(server.rejected_total(), 1u);
  CTF_CHECK_EQ(server.accepted_total(), 1u);
  CTF_CHECK_EQ(server.session_count(), 1u);
  CTF_CHECK(!refused.connected());

  std::string body;
  CTF_CHECK_OK(served.inspect(0, CollectiveId{}, 0, 8, body));
  CTF_CHECK_MSG(contains(body, "running: yes"), body);

  served.close();
  wait_for_sessions(server, 0u);
  CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
}

CTF_TEST("integration_loopback", "bad_frames_are_counted_and_their_connection_semantics_hold") {
  CoordinatorServer& server = shared_coordinator();
  const std::uint64_t stale_before = server.stale_frame_count();
  const std::uint64_t violations_before = server.protocol_violation_count();
  const std::uint64_t malformed_before = server.malformed_frame_count();

  // A well formed frame from the right session, carrying an epoch that is
  // guaranteed not to be the current one.  Its sequence is 1 because a frame
  // whose envelope is refused never consumes a sequence number.
  ClientSession stale_probe;
  CTF_CHECK_OK(stale_probe.connect("127.0.0.1", server.port(), "stale-epoch-probe",
                                   transport::monotonic_now_ms()));
  protocol::Frame stale_frame = raw_inspect_frame(stale_probe, protocol::kFlagRequest, 1);
  stale_frame.envelope.epoch =
      CoordinatorEpoch(stale_probe.hello().epoch.high() ^ 1ull, stale_probe.hello().epoch.low());
  ErrorCode stale_code = ErrorCode::kOk;
  CTF_CHECK_OK(send_and_read_error(stale_probe, stale_frame, stale_code));
  CTF_CHECK_EQ(stale_code, ErrorCode::kEnvelopeEpochStale);
  CTF_CHECK_EQ(server.stale_frame_count(), stale_before + 1);
  CTF_CHECK_EQ(server.protocol_violation_count(), violations_before);
  CTF_CHECK_EQ(server.malformed_frame_count(), malformed_before);
  // src/coordinator.cpp documents a refused envelope as non fatal: the session
  // continues to serve, and the frame the strict client sends next is exactly
  // the sequence number the refused frame did not consume.
  std::string body;
  CTF_CHECK_OK(stale_probe.inspect(0, CollectiveId{}, 0, 8, body));
  CTF_CHECK_MSG(contains(body, "running: yes"), body);

  // A frame whose flag set contradicts its kind is a protocol violation, and
  // that is non fatal too.
  ClientSession flags_probe;
  CTF_CHECK_OK(flags_probe.connect("127.0.0.1", server.port(), "flags-probe",
                                   transport::monotonic_now_ms()));
  protocol::Frame flags_frame = raw_inspect_frame(flags_probe, protocol::kFlagResponse, 1);
  ErrorCode flags_code = ErrorCode::kOk;
  CTF_CHECK_OK(send_and_read_error(flags_probe, flags_frame, flags_code));
  CTF_CHECK_EQ(flags_code, ErrorCode::kFrameFlagsInvalid);
  CTF_CHECK_EQ(server.protocol_violation_count(), violations_before + 1);
  CTF_CHECK_EQ(server.stale_frame_count(), stale_before + 1);
  CTF_CHECK_EQ(server.malformed_frame_count(), malformed_before);
  // This frame passed envelope validation, so it did consume its sequence: the
  // follow up continues from the next one and is answered normally.
  protocol::Frame follow_up = raw_inspect_frame(flags_probe, protocol::kFlagRequest, 2);
  CTF_CHECK_OK(flags_probe.connection().send_frame(follow_up));
  protocol::Frame follow_up_response;
  CTF_CHECK_OK(flags_probe.connection().receive_frame(follow_up_response));
  CTF_CHECK_EQ(static_cast<protocol::MessageKind>(follow_up_response.kind),
               protocol::MessageKind::kInspectResponse);
  protocol::InspectResponsePayload follow_up_body;
  CTF_CHECK_OK(protocol::decode_inspect_response(follow_up_response.payload, follow_up_body));
  CTF_CHECK(contains(follow_up_body.body, "running: yes"));

  // A connection owns exactly one session.  A frame naming a session this
  // coordinator never bound to this connection is an attempt to act as somebody
  // else: it is a protocol violation, it is answered, and the connection is
  // closed rather than left half registered.
  ClientSession foreign_probe;
  CTF_CHECK_OK(foreign_probe.connect("127.0.0.1", server.port(), "foreign-session-probe",
                                     transport::monotonic_now_ms()));
  protocol::Frame foreign_frame = raw_inspect_frame(foreign_probe, protocol::kFlagRequest, 1);
  foreign_frame.envelope.session =
      SessionId(foreign_probe.hello().session.high() ^ (1ull << 63), foreign_probe.hello().session.low());
  ErrorCode foreign_code = ErrorCode::kOk;
  CTF_CHECK_OK(send_and_read_error(foreign_probe, foreign_frame, foreign_code));
  CTF_CHECK_EQ(foreign_code, ErrorCode::kEnvelopeSessionMismatch);
  CTF_CHECK_EQ(server.protocol_violation_count(), violations_before + 2);
  CTF_CHECK_EQ(server.stale_frame_count(), stale_before + 1);
  CTF_CHECK_EQ(server.malformed_frame_count(), malformed_before);
  protocol::Frame trailing;
  const Status closed = foreign_probe.connection().receive_frame(trailing);
  CTF_CHECK_MSG(!closed.is_ok(), "the connection stayed open after a foreign-session frame");
  CTF_CHECK(closed.code() == ErrorCode::kTransportClosed ||
            closed.code() == ErrorCode::kTransportReceiveFailed);

  stale_probe.close();
  flags_probe.close();
  foreign_probe.close();
  wait_for_sessions(server, 0u);
}

CTF_TEST("integration_loopback", "shutdown_request_is_acknowledged_and_stop_closes_the_listener") {
  CoordinatorServer& server = shared_coordinator();
  const std::uint16_t port = server.port();
  CTF_CHECK(server.is_running());
  CTF_CHECK(!server.shutdown_requested());

  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", port, "shutdown-requester", transport::monotonic_now_ms()));
  CTF_CHECK_OK(client.shutdown_coordinator());
  wait_for_shutdown_request(server);

  // The acknowledged peer detaches before the coordinator is stopped, and its
  // teardown is awaited.  stop() is defined to shut down live sessions itself,
  // but a session still attached at stop time leaves this transport's shutdown
  // blocked forever (the session worker is never unblocked and its join never
  // returns); that defect is reported rather than worked around in the library,
  // so the case stops the coordinator with the acknowledged peer already gone
  // and still proves that stop() succeeds and the listener is closed after it.
  client.close();
  wait_for_sessions(server, 0u);

  CTF_CHECK_OK(server.stop(transport::monotonic_now_ms()));
  CTF_CHECK(!server.is_running());

  // The acknowledged peer is detached above, before the stop: a stop issued
  // while a peer is still attached does not complete in this transport.  The
  // listening socket is closed and the accepted socket is shut down, yet the
  // session worker never reaches its close path and the join inside
  // transport::Server::shutdown() never returns.  That is a reported library
  // defect and is deliberately not worked around inside the library, so the
  // case stops the coordinator with the acknowledged peer already gone and
  // still proves that stop() succeeds and the listener is closed after it.
  //
  // A stopped coordinator accepts no new work: its listener is closed, so a
  // fresh connection is refused rather than queued or half served.
  ClientSession late;
  const Status late_status = late.connect("127.0.0.1", port, "late-peer", transport::monotonic_now_ms());
  CTF_CHECK_MSG(!late_status.is_ok(), "a stopped coordinator accepted a new connection");
  CTF_CHECK(!late.connected());
}

CTF_TEST("integration_loopback", "stop_is_safe_while_the_coordinator_is_being_observed") {
  CoordinatorConfig config = shared_config();
  config.service.coordinator_label = "ctf-integration-observed";
  CoordinatorServer server(config);
  CTF_CHECK_OK(server.start(transport::monotonic_now_ms()));
  const std::uint16_t port = server.port();
  ClientSession client;
  CTF_CHECK_OK(client.connect("127.0.0.1", port, "observed-peer", transport::monotonic_now_ms()));

  // Real work first, so the teardown below has a catalog, counters and history
  // to leave in a defined state.
  std::vector<ParticipantId> participants;
  participants.push_back(mint_identity());
  participants.push_back(mint_identity());
  CollectiveDefinition definition = ctf::test::make_definition(
      mint_identity(), CollectiveClass::kAllReduce, participants, "observed-all-reduce", 1ull << 20);
  protocol::RegisterAckPayload registration;
  CTF_CHECK_OK(client.register_collective(definition, registration));
  CTF_CHECK_EQ(registration.result, static_cast<std::uint8_t>(RegistrationResult::kRegistered));

  // The peer detaches before the stop: this case observes the teardown itself,
  // not the session shutdown path.
  client.close();
  wait_for_sessions(server, 0u);

  std::atomic<bool> stopped{false};
  std::thread stopper([&]() {
    static_cast<void>(server.stop(transport::monotonic_now_ms()));
    stopped.store(true, std::memory_order_release);
  });
  // The observer reads the coordinator's own state while the teardown runs.  It
  // takes the transport and service mutexes on purpose: a stop must complete
  // even while another thread is asking the coordinator what it is.
  std::size_t observations = 0;
  while (!stopped.load(std::memory_order_acquire)) {
    static_cast<void>(server.is_running());
    static_cast<void>(server.session_count());
    static_cast<void>(server.service().session_count());
    static_cast<void>(server.service().inspect_summary());
    ++observations;
    std::this_thread::yield();
  }
  stopper.join();
  CTF_CHECK_MSG(observations > 0, "the observer never ran while the coordinator was stopping");
  CTF_CHECK(!server.is_running());
  CTF_CHECK_EQ(server.session_count(), 0u);
  CTF_CHECK_EQ(server.service().session_count(), 0u);

  // A second coordinator, started and stopped after the observed one, still
  // honours the shutdown-request path.
  CoordinatorServer second(config);
  CTF_CHECK_OK(second.start(transport::monotonic_now_ms()));
  ClientSession peer;
  CTF_CHECK_OK(peer.connect("127.0.0.1", second.port(), "observed-second",
                            transport::monotonic_now_ms()));
  CTF_CHECK_OK(peer.shutdown_coordinator());
  wait_for_shutdown_request(second);
  peer.close();
  wait_for_sessions(second, 0u);
  CTF_CHECK_OK(second.stop(transport::monotonic_now_ms()));
  CTF_CHECK(!second.is_running());
}

CTF_TEST_MAIN()
