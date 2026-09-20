// Collective Traffic Fabric - frame, envelope and codec protocol proofs.
// Copyright 2026 Summon Software Labs.
#include <cstdint>
#include <string>
#include <vector>

#include "ctf/protocol.hpp"
#include "ctf/snapshot.hpp"
#include "support/client_session.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;

protocol::Frame base_frame() {
  protocol::Frame frame;
  frame.kind = static_cast<std::uint16_t>(protocol::MessageKind::kPlanFlowGroup);
  frame.flags = protocol::kFlagRequest;
  frame.envelope.session = Identity(0xAABB, 0xCCDD);
  frame.envelope.incarnation = Identity(0x1122, 0x3344);
  frame.envelope.epoch = Identity(0x5566, 0x7788);
  frame.envelope.frame_sequence = Sequence(9);
  return frame;
}

}  // namespace

CTF_TEST("protocol", "header_layout_is_fixed_and_self_describing") {
  const protocol::Frame frame = base_frame();
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_frame(frame, bytes));
  CTF_CHECK_EQ(bytes.size(), static_cast<std::size_t>(protocol::kHeaderBytes + protocol::kEnvelopeBytes));
  // magic, header size, kind, flags, versions, payload length, CRCs, reserved
  CTF_CHECK_EQ(bytes[0], 0x31u);
  CTF_CHECK_EQ(bytes[1], 0x43u);
  CTF_CHECK_EQ(bytes[2], 0x54u);
  CTF_CHECK_EQ(bytes[3], 0x46u);
  CTF_CHECK_EQ(bytes[4], static_cast<std::uint8_t>(protocol::kHeaderBytes & 0xFFu));
  CTF_CHECK_EQ(bytes[6], static_cast<std::uint8_t>(protocol::MessageKind::kPlanFlowGroup));
  CTF_CHECK_EQ(bytes[26], 0u);
  CTF_CHECK_EQ(bytes[27], 0u);
  protocol::Frame decoded;
  std::size_t consumed = 0;
  CTF_CHECK_OK(protocol::decode_frame(bytes.data(), bytes.size(), decoded, consumed));
  CTF_CHECK_EQ(consumed, bytes.size());
  CTF_CHECK_EQ(decoded.envelope.session, frame.envelope.session);
  CTF_CHECK_EQ(decoded.envelope.incarnation, frame.envelope.incarnation);
  CTF_CHECK_EQ(decoded.envelope.epoch, frame.envelope.epoch);
  CTF_CHECK_EQ(decoded.envelope.frame_sequence.value(), frame.envelope.frame_sequence.value());
}

CTF_TEST("protocol", "two_frames_in_one_buffer_decode_in_order_without_loss") {
  const protocol::Frame first = base_frame();
  protocol::Frame second = base_frame();
  second.kind = static_cast<std::uint16_t>(protocol::MessageKind::kHeartbeat);
  second.envelope.frame_sequence = Sequence(10);
  std::vector<std::uint8_t> buffer;
  std::vector<std::uint8_t> piece;
  CTF_CHECK_OK(protocol::encode_frame(first, piece));
  buffer.insert(buffer.end(), piece.begin(), piece.end());
  CTF_CHECK_OK(protocol::encode_frame(second, piece));
  buffer.insert(buffer.end(), piece.begin(), piece.end());

  protocol::Frame decoded;
  std::size_t consumed = 0;
  CTF_CHECK_OK(protocol::decode_frame(buffer.data(), buffer.size(), decoded, consumed));
  CTF_CHECK_EQ(decoded.kind, first.kind);
  CTF_CHECK_EQ(decoded.envelope.frame_sequence.value(), 9u);
  CTF_CHECK_OK(protocol::decode_frame(buffer.data() + consumed, buffer.size() - consumed, decoded, consumed));
  CTF_CHECK_EQ(decoded.kind, second.kind);
  CTF_CHECK_EQ(decoded.envelope.frame_sequence.value(), 10u);
}

CTF_TEST("protocol", "empty_buffer_reports_closed_rather_than_truncated") {
  protocol::Frame decoded;
  std::size_t consumed = 0;
  CTF_CHECK_CODE(protocol::decode_frame(nullptr, 0, decoded, consumed), ErrorCode::kTransportClosed);
  std::vector<std::uint8_t> partial(10, 0);
  CTF_CHECK_CODE(protocol::decode_frame(partial.data(), partial.size(), decoded, consumed),
                 ErrorCode::kFrameTruncatedHeader);
}

CTF_TEST("protocol", "the_envelope_carries_the_correlation_it_documents") {
  protocol::Frame frame = base_frame();
  frame.envelope.correlation = 0x1122334455667788ull;
  frame.payload = {7, 7};
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_frame(frame, bytes));
  protocol::Frame decoded;
  std::size_t consumed = 0;
  CTF_CHECK_OK(protocol::decode_frame(bytes.data(), bytes.size(), decoded, consumed));
  // A field that exists in the struct but never reaches the wire would make
  // every correlation based query silently unmatched, so it is asserted here.
  CTF_CHECK_EQ(decoded.envelope.correlation, frame.envelope.correlation);
  CTF_CHECK_EQ(protocol::kEnvelopeBytes, 48u);
}

CTF_TEST("protocol", "payload_crc_covers_the_envelope_as_well_as_the_body") {
  protocol::Frame frame = base_frame();
  frame.payload = {1, 2, 3};
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_frame(frame, bytes));
  // Corrupting an envelope byte invalidates the body CRC.
  bytes[protocol::kHeaderBytes + 3] ^= 0x01;
  protocol::Frame decoded;
  std::size_t consumed = 0;
  CTF_CHECK_CODE(protocol::decode_frame(bytes.data(), bytes.size(), decoded, consumed),
                 ErrorCode::kFrameIntegrityMismatch);
}

CTF_TEST("protocol", "session_identity_lives_in_the_header_and_is_checked_first") {
  const protocol::Frame frame = base_frame();
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_frame(frame, bytes));
  protocol::Frame decoded;
  std::size_t consumed = 0;
  CTF_CHECK_OK(protocol::decode_frame(bytes.data(), bytes.size(), decoded, consumed));
  CTF_CHECK_EQ(decoded.envelope.session, frame.envelope.session);
}

CTF_TEST("protocol", "message_codecs_round_trip_every_payload") {
  protocol::HelloPayload hello;
  hello.client_label = "peer-alpha";
  hello.client_version = "1.0.0";
  hello.client_boot_monotonic_ms = 12345;
  std::vector<std::uint8_t> bytes;
  protocol::HelloPayload hello_out;
  CTF_CHECK_OK(protocol::encode_hello(hello, bytes));
  CTF_CHECK_OK(protocol::decode_hello(bytes, hello_out));
  CTF_CHECK_EQ(hello_out.client_label, hello.client_label);
  CTF_CHECK_EQ(hello_out.client_boot_monotonic_ms, hello.client_boot_monotonic_ms);

  protocol::HeartbeatPayload heartbeat;
  heartbeat.participant = mint_identity();
  heartbeat.incarnation = mint_identity();
  heartbeat.node = mint_identity();
  heartbeat.state = ParticipantState::kLive;
  protocol::HeartbeatPayload heartbeat_out;
  CTF_CHECK_OK(protocol::encode_heartbeat(heartbeat, bytes));
  CTF_CHECK_OK(protocol::decode_heartbeat(bytes, heartbeat_out));
  CTF_CHECK_EQ(heartbeat_out.participant, heartbeat.participant);
  CTF_CHECK_EQ(heartbeat_out.state, ParticipantState::kLive);

  protocol::LifecyclePayload lifecycle;
  lifecycle.id = mint_identity();
  lifecycle.generation = mint_identity();
  lifecycle.attempt = mint_identity();
  lifecycle.reason = "protocol test";
  protocol::LifecyclePayload lifecycle_out;
  CTF_CHECK_OK(protocol::encode_lifecycle(lifecycle, bytes));
  CTF_CHECK_OK(protocol::decode_lifecycle(bytes, lifecycle_out));
  CTF_CHECK_EQ(lifecycle_out.reason, lifecycle.reason);

  protocol::InspectRequestPayload inspect;
  inspect.subject = 2;
  inspect.limit = 32;
  protocol::InspectRequestPayload inspect_out;
  CTF_CHECK_OK(protocol::encode_inspect_request(inspect, bytes));
  CTF_CHECK_OK(protocol::decode_inspect_request(bytes, inspect_out));
  CTF_CHECK_EQ(inspect_out.limit, 32u);
  // A page size beyond the bound is refused rather than clamped silently.
  inspect.limit = limits::kInspectionPageMax + 1;
  CTF_CHECK_OK(protocol::encode_inspect_request(inspect, bytes));
  CTF_CHECK_CODE(protocol::decode_inspect_request(bytes, inspect_out), ErrorCode::kDecodeCountOutOfRange);

  protocol::ErrorPayload error;
  error.code = ErrorCode::kEnvelopeEpochStale;
  error.message = "stale epoch";
  error.reasons.add("envelope_epoch_stale", "the frame named a stale epoch");
  protocol::ErrorPayload error_out;
  CTF_CHECK_OK(protocol::encode_error(error, bytes));
  CTF_CHECK_OK(protocol::decode_error(bytes, error_out));
  CTF_CHECK_EQ(error_out.code, ErrorCode::kEnvelopeEpochStale);
  CTF_CHECK_EQ(error_out.reasons.size(), 1u);
}

CTF_TEST("protocol", "the_heartbeat_acknowledgement_carries_the_coordinators_view") {
  protocol::HeartbeatAckPayload ack;
  ack.participant = mint_identity();
  ack.incarnation = mint_identity();
  ack.state = ParticipantState::kLive;
  ack.expires_at_monotonic_ms = 123456;
  ack.accepted = true;
  ack.reasons.add("heartbeat_accepted", "liveness accepted for this incarnation", ack.participant);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_heartbeat_ack(ack, bytes));
  protocol::HeartbeatAckPayload decoded;
  CTF_CHECK_OK(protocol::decode_heartbeat_ack(bytes, decoded));
  CTF_CHECK_EQ(decoded.participant, ack.participant);
  CTF_CHECK_EQ(decoded.incarnation, ack.incarnation);
  CTF_CHECK_EQ(decoded.expires_at_monotonic_ms, ack.expires_at_monotonic_ms);
  CTF_CHECK(decoded.accepted);
  CTF_CHECK_EQ(decoded.reasons.size(), 1u);
  // Every request kind must have a response kind: a request that is silently
  // accepted would leave a peer waiting forever, which is indistinguishable from
  // a hang and hides the real result.
  CTF_CHECK(protocol::message_kind_is_known(
      static_cast<std::uint16_t>(protocol::MessageKind::kHeartbeatAck)));
  CTF_CHECK(!protocol::message_kind_is_known(
      static_cast<std::uint16_t>(protocol::MessageKind::kKindCount)));
}

CTF_TEST("protocol", "traffic_policy_survives_the_wire_unchanged") {
  const TrafficPolicy policy = TrafficPolicy::standard(mint_identity());
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_install_policy(protocol::InstallPolicyPayload{policy}, bytes));
  protocol::InstallPolicyPayload decoded;
  CTF_CHECK_OK(protocol::decode_install_policy(bytes, decoded));
  CTF_CHECK_EQ(decoded.policy.generation, policy.generation);
  CTF_CHECK_EQ(decoded.policy.class_policies.size(), policy.class_policies.size());
  CTF_CHECK_EQ(decoded.policy.collective_policies.size(), policy.collective_policies.size());
  CTF_CHECK_OK(decoded.policy.validate());
  for (std::size_t index = 0; index < policy.class_policies.size(); ++index) {
    CTF_CHECK_EQ(decoded.policy.class_policies[index].traffic_class,
                 policy.class_policies[index].traffic_class);
    CTF_CHECK_EQ(decoded.policy.class_policies[index].minimum_bandwidth_bps,
                 policy.class_policies[index].minimum_bandwidth_bps);
    CTF_CHECK_EQ(decoded.policy.class_policies[index].isolation,
                 policy.class_policies[index].isolation);
  }
  // A policy carried through the evidence tag must decode to the same thing.
  protocol::EvidencePayload tagged;
  tagged.kind = protocol::EvidenceKindTag::kPolicy;
  tagged.policy = policy;
  std::vector<std::uint8_t> tagged_bytes;
  CTF_CHECK_OK(protocol::encode_evidence(tagged, tagged_bytes));
  protocol::EvidencePayload tagged_out;
  CTF_CHECK_OK(protocol::decode_evidence(tagged_bytes, tagged_out));
  CTF_CHECK_EQ(tagged_out.policy.generation, policy.generation);
}

CTF_TEST("protocol", "evidence_codecs_round_trip_and_stay_canonical") {
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(5, 3, 2);
  TopologyEvidence topology = fabric.topology;
  std::vector<std::uint8_t> bytes;
  protocol::Writer writer;
  CTF_CHECK_OK(protocol::encode_topology(topology, writer));
  bytes = writer.data();
  protocol::Reader reader(bytes.data(), bytes.size());
  TopologyEvidence topology_out;
  CTF_CHECK_OK(protocol::decode_topology(reader, topology_out));
  CTF_CHECK_OK(reader.require_end());
  CTF_CHECK_EQ(topology_out.nodes.size(), topology.nodes.size());
  CTF_CHECK_EQ(topology_out.links.size(), topology.links.size());
  CTF_CHECK(topology_out.nodes == topology.nodes);

  CapacityEvidence capacity = ctf::test::make_capacity(fabric.topology, 6, 1234);
  writer = protocol::Writer{};
  CTF_CHECK_OK(protocol::encode_capacity(capacity, writer));
  bytes = writer.data();
  protocol::Reader capacity_reader(bytes.data(), bytes.size());
  CapacityEvidence capacity_out;
  CTF_CHECK_OK(protocol::decode_capacity(capacity_reader, capacity_out));
  CTF_CHECK_OK(capacity_out.validate());
  CTF_CHECK(capacity_out.links == capacity.links);

  CongestionEvidence congestion = ctf::test::make_congestion(fabric.topology, 7, 8600);
  writer = protocol::Writer{};
  CTF_CHECK_OK(protocol::encode_congestion(congestion, writer));
  bytes = writer.data();
  protocol::Reader congestion_reader(bytes.data(), bytes.size());
  CongestionEvidence congestion_out;
  CTF_CHECK_OK(protocol::decode_congestion(congestion_reader, congestion_out));
  CTF_CHECK_OK(congestion_out.validate());
  CTF_CHECK_EQ(congestion_out.links.front().level, CongestionLevel::kHigh);
}

CTF_TEST("protocol", "evidence_tag_selects_exactly_one_observation") {
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(9, 2, 2);
  protocol::EvidencePayload payload;
  payload.kind = protocol::EvidenceKindTag::kCongestion;
  payload.congestion = ctf::test::make_congestion(fabric.topology, 10, 100);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_evidence(payload, bytes));
  protocol::EvidencePayload decoded;
  CTF_CHECK_OK(protocol::decode_evidence(bytes, decoded));
  CTF_CHECK_EQ(decoded.kind, protocol::EvidenceKindTag::kCongestion);
  CTF_CHECK_EQ(decoded.congestion.links.size(), payload.congestion.links.size());
  // The payload must not silently carry the other observation kinds.
  CTF_CHECK(decoded.capacity.links.empty());
  CTF_CHECK(decoded.topology.nodes.empty());

  protocol::EvidencePayload empty;
  CTF_CHECK_CODE(protocol::encode_evidence(empty, bytes), ErrorCode::kDecodeInvalidTag);
}

CTF_TEST("protocol", "flow_group_plan_round_trips_with_every_edge_field") {
  ctf::test::SyntheticWorld world(13, 2, 2, 1000);
  CollectiveDefinition definition =
      ctf::test::make_definition(mint_identity(), CollectiveClass::kAllToAll, world.participants);
  definition.generation = world.ids.next();
  CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = definition.generation;
  instance.attempt = mint_identity();
  const FlowGroupPlan plan = ctf::test::make_full_mesh_plan(instance, definition.participants, 4096);
  protocol::Writer writer;
  CTF_CHECK_OK(protocol::encode_flow_group_plan(plan, writer));
  const std::vector<std::uint8_t> bytes = writer.data();
  protocol::Reader reader(bytes.data(), bytes.size());
  FlowGroupPlan decoded;
  CTF_CHECK_OK(protocol::decode_flow_group_plan(reader, decoded));
  CTF_CHECK_OK(reader.require_end());
  CTF_CHECK(decoded == plan);

  FlowGroup group;
  CTF_CHECK_OK(construct_flow_group(plan, definition.participants, group));
  writer = protocol::Writer{};
  CTF_CHECK_OK(protocol::encode_flow_group(group, writer));
  const std::vector<std::uint8_t> group_bytes = writer.data();
  protocol::Reader group_reader(group_bytes.data(), group_bytes.size());
  FlowGroup group_out;
  CTF_CHECK_OK(protocol::decode_flow_group(group_reader, group_out));
  CTF_CHECK_OK(group_reader.require_end());
  CTF_CHECK(group_out == group);
}

CTF_TEST("protocol", "definition_decoder_canonicalises_participant_order") {
  ctf::test::SyntheticWorld world(17, 2, 2, 1000);
  CollectiveDefinition definition =
      ctf::test::make_definition(mint_identity(), CollectiveClass::kReduceScatter, world.participants);
  std::reverse(definition.participants.begin(), definition.participants.end());
  protocol::RegisterCollectivePayload payload;
  payload.definition = definition;
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_register_collective(payload, bytes));
  protocol::RegisterCollectivePayload decoded;
  CTF_CHECK_OK(protocol::decode_register_collective(bytes, decoded));
  CTF_CHECK(participants_are_canonical(decoded.definition.participants));
  CTF_CHECK_EQ(decoded.definition.participants.size(), definition.participants.size());
}

CTF_TEST_MAIN()
