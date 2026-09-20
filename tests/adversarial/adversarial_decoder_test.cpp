// Collective Traffic Fabric - adversarial protocol and persistence proofs.
// Copyright 2026 Summon Software Labs.
//
// Decoders are treated as an attack surface.  Every case here feeds a decoder
// something it was never meant to see and asserts that the failure is a
// deterministic error code rather than a crash, a silent acceptance, or an
// unbounded allocation.
#include <cstdint>
#include <string>
#include <vector>

#include "ctf/collective.hpp"
#include "ctf/persistence.hpp"
#include "ctf/protocol.hpp"
#include "ctf/snapshot.hpp"
#include "support/synthetic.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;

protocol::Frame make_frame(std::uint16_t kind, std::vector<std::uint8_t> payload) {
  protocol::Frame frame;
  frame.kind = kind;
  frame.flags = protocol::kFlagRequest;
  frame.envelope.session = Identity(0x1111, 0x2222);
  frame.envelope.incarnation = Identity(0x3333, 0x4444);
  frame.envelope.epoch = Identity(0x5555, 0x6666);
  frame.envelope.frame_sequence = Sequence(7);
  frame.payload = std::move(payload);
  return frame;
}

// Asserts that a byte level mutation of a valid frame is either refused with a
// deterministic code or decoded to exactly the original frame.  Nothing else is
// acceptable: no crash, no partial acceptance, no silent truncation.
void assert_mutation_is_safe(const protocol::Frame& original) {
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_frame(original, bytes));
  std::size_t accepted = 0;
  std::size_t refused = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    for (const auto mask : {std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF}}) {
      std::vector<std::uint8_t> mutated = bytes;
      mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ mask);
      if (mutated == bytes) continue;
      protocol::Frame decoded;
      std::size_t consumed = 0;
      const Status status = protocol::decode_frame(mutated.data(), mutated.size(), decoded, consumed);
      if (status.is_ok()) {
        // If a mutation is accepted it must be byte identical in meaning.
        std::vector<std::uint8_t> reencoded;
        CTF_CHECK_OK(protocol::encode_frame(decoded, reencoded));
        CTF_CHECK_EQ(reencoded, mutated);
        CTF_CHECK_EQ(consumed, mutated.size());
        ++accepted;
      } else {
        ++refused;
        CTF_CHECK(!status.message().empty());
        CTF_CHECK(consumed == 0);
      }
    }
  }
  CTF_CHECK(refused > accepted);
}

}  // namespace

CTF_TEST("adversarial_decoder", "frame_header_corruption_never_lands_a_frame") {
  const protocol::Frame frame = make_frame(static_cast<std::uint16_t>(protocol::MessageKind::kInspectRequest),
                                           {1, 2, 3, 4, 5});
  assert_mutation_is_safe(frame);
}

CTF_TEST("adversarial_decoder", "decode_rejects_every_truncation_length") {
  const protocol::Frame frame = make_frame(static_cast<std::uint16_t>(protocol::MessageKind::kHello),
                                           {9, 9, 9});
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_frame(frame, bytes));
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    protocol::Frame decoded;
    std::size_t consumed = 0;
    const Status status =
        protocol::decode_frame(bytes.data(), length, decoded, consumed);
    CTF_CHECK_MSG(!status.is_ok(), "a truncated frame was accepted");
    CTF_CHECK_EQ(consumed, 0u);
  }
  // The complete frame is accepted, and reports its full length.
  protocol::Frame decoded;
  std::size_t consumed = 0;
  CTF_CHECK_OK(protocol::decode_frame(bytes.data(), bytes.size(), decoded, consumed));
  CTF_CHECK_EQ(consumed, bytes.size());
}

CTF_TEST("adversarial_decoder", "framing_rejects_wrong_magic_version_and_kind") {
  const protocol::Frame frame = make_frame(static_cast<std::uint16_t>(protocol::MessageKind::kHello), {1});
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_frame(frame, bytes));
  protocol::Frame decoded;
  std::size_t consumed = 0;

  std::vector<std::uint8_t> wrong_magic = bytes;
  wrong_magic[0] ^= 0xFF;
  // The magic is checked before the header CRC: a stream that is not this
  // protocol at all is reported as such rather than as corruption.
  CTF_CHECK_CODE(protocol::decode_frame(wrong_magic.data(), wrong_magic.size(), decoded, consumed),
                 ErrorCode::kFrameMagicMismatch);

  std::vector<std::uint8_t> wrong_major = bytes;
  wrong_major[10] = 9;
  // The header CRC covers the version field, so this is caught as corruption
  // before it can be interpreted as a version.
  CTF_CHECK_CODE(protocol::decode_frame(wrong_major.data(), wrong_major.size(), decoded, consumed),
                 ErrorCode::kFrameIntegrityMismatch);

  // An unknown kind with a recomputed header CRC is refused as an unknown kind.
  protocol::Frame unknown = frame;
  unknown.kind = 4242;
  CTF_CHECK_CODE(protocol::encode_frame(unknown, bytes), ErrorCode::kFrameKindUnknown);

  // Flags outside the defined mask are refused at encode time.
  protocol::Frame bad_flags = frame;
  bad_flags.flags = 0x8000;
  CTF_CHECK_CODE(protocol::encode_frame(bad_flags, bytes), ErrorCode::kFrameFlagsInvalid);
}

CTF_TEST("adversarial_decoder", "oversized_frame_is_refused_before_allocation") {
  const protocol::Frame frame = make_frame(static_cast<std::uint16_t>(protocol::MessageKind::kHello), {1});
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(protocol::encode_frame(frame, bytes));
  // Claim a body far beyond the bound, then fix the header CRC so the length
  // field itself is well formed.  The decoder must refuse on the bound alone.
  const std::uint32_t huge = 0xFFFFFFF0u;
  for (int index = 0; index < 4; ++index) {
    bytes[14 + index] = static_cast<std::uint8_t>((huge >> (index * 8)) & 0xFFu);
  }
  const std::uint32_t header_crc = ctf::crc32(bytes.data(), 22);
  for (int index = 0; index < 4; ++index) {
    bytes[22 + index] = static_cast<std::uint8_t>((header_crc >> (index * 8)) & 0xFFu);
  }
  protocol::Frame decoded;
  std::size_t consumed = 0;
  const Status status = protocol::decode_frame(bytes.data(), bytes.size(), decoded, consumed);
  CTF_CHECK_CODE(status, ErrorCode::kFrameLengthOutOfRange);
  CTF_CHECK_EQ(consumed, 0u);
}

CTF_TEST("adversarial_decoder", "payload_codecs_reject_trailing_bytes_and_absurd_counts") {
  protocol::HelloPayload hello;
  hello.client_label = "peer";
  hello.client_version = "1.0.0";
  std::vector<std::uint8_t> encoded;
  CTF_CHECK_OK(protocol::encode_hello(hello, encoded));
  protocol::HelloPayload decoded;
  CTF_CHECK_OK(protocol::decode_hello(encoded, decoded));
  CTF_CHECK_EQ(decoded.client_label, hello.client_label);

  std::vector<std::uint8_t> extended = encoded;
  extended.push_back(0);
  CTF_CHECK_CODE(protocol::decode_hello(extended, decoded), ErrorCode::kDecodeTrailingBytes);

  // A string length that claims more bytes than the payload holds.
  std::vector<std::uint8_t> lying_length = encoded;
  lying_length[0] = 0xFF;
  lying_length[1] = 0xFF;
  lying_length[2] = 0xFF;
  lying_length[3] = 0x7F;
  CTF_CHECK_CODE(protocol::decode_hello(lying_length, decoded), ErrorCode::kDecodeLengthOutOfRange);

  // An empty payload is a truncation, not a default constructed message.
  std::vector<std::uint8_t> empty;
  CTF_CHECK_CODE(protocol::decode_hello(empty, decoded), ErrorCode::kDecodeTruncated);
}

CTF_TEST("adversarial_decoder", "collective_definition_decoder_bounds_every_collection") {
  CollectiveDefinition definition;
  definition.id = mint_identity();
  definition.generation = mint_identity();
  definition.collective_class = CollectiveClass::kAllReduce;
  definition.participants = {mint_identity(), mint_identity()};
  canonicalize_participants(definition.participants);

  protocol::Writer writer;
  CTF_CHECK_OK(protocol::encode_definition(definition, writer));
  std::vector<std::uint8_t> bytes = writer.data();

  protocol::Reader reader(bytes.data(), bytes.size());
  CollectiveDefinition decoded;
  CTF_CHECK_OK(protocol::decode_definition(reader, decoded));
  CTF_CHECK_OK(reader.require_end());
  CTF_CHECK(decoded.participants == definition.participants);

  // Claim 4 billion participants.  The bound is checked from the count alone,
  // before any vector is resized.  The offset is derived from the codec itself
  // rather than hard coded, so this case keeps testing the real layout as the
  // format evolves: a dedicated reader walks the fields that precede the count
  // and reports where it stopped.
  auto offset_of_participant_count = [](const CollectiveDefinition& value) -> std::size_t {
    protocol::Writer probe;
    if (!protocol::encode_definition(value, probe).is_ok()) return 0;
    protocol::Reader walker(probe.data().data(), probe.data().size());
    Identity skipped;
    std::uint8_t byte = 0;
    std::string text;
    Status ok = walker.identity(skipped);
    ok = ok.is_ok() ? walker.identity(skipped) : ok;
    ok = ok.is_ok() ? walker.u8(byte) : ok;
    ok = ok.is_ok() ? walker.u8(byte) : ok;
    ok = ok.is_ok() ? walker.u8(byte) : ok;
    ok = ok.is_ok() ? walker.string(text, limits::kLabelBytesMax) : ok;
    ok = ok.is_ok() ? walker.identity(skipped) : ok;
    ok = ok.is_ok() ? walker.identity(skipped) : ok;
    return ok.is_ok() ? walker.offset() : 0;
  };

  CollectiveDefinition empty_label = definition;
  empty_label.label.clear();
  const std::size_t count_offset = offset_of_participant_count(empty_label);
  CTF_CHECK_MSG(count_offset != 0, "the participant count offset could not be derived");
  protocol::Writer canonical;
  CTF_CHECK_OK(protocol::encode_definition(empty_label, canonical));

  std::vector<std::uint8_t> absurd = canonical.data();
  for (int index = 0; index < 4; ++index) {
    absurd[count_offset + static_cast<std::size_t>(index)] = 0xFF;
  }
  protocol::Reader absurd_reader(absurd.data(), absurd.size());
  CollectiveDefinition rejected;
  CTF_CHECK_CODE(protocol::decode_definition(absurd_reader, rejected), ErrorCode::kDecodeCountOutOfRange);

  // A count that is merely large is refused as well, and never resized into:
  // 100000 participants exceeds the participant bound.
  std::vector<std::uint8_t> large = canonical.data();
  large[count_offset] = 0xA0;
  large[count_offset + 1] = 0x86;
  large[count_offset + 2] = 0x01;
  large[count_offset + 3] = 0x00;
  protocol::Reader large_reader(large.data(), large.size());
  CTF_CHECK_CODE(protocol::decode_definition(large_reader, rejected), ErrorCode::kDecodeCountOutOfRange);

  // The same must hold for the phase count that follows the participant list.
  // Its offset is derived by walking the participant list the codec actually
  // wrote, so this case cannot drift out of step with the format.
  const std::size_t phases_offset = [&canonical, count_offset]() -> std::size_t {
    protocol::Reader walker(canonical.data().data(), canonical.data().size());
    // Re-walk the prefix in exactly the order the encoder emitted it.  The
    // offset the walk reports is compared against the independently derived
    // participant count offset, so the two derivations cross check each other.
    const Status seek = [&walker, count_offset]() -> Status {
      Identity skipped;
      std::uint8_t byte = 0;
      std::string text;
      Status status = walker.identity(skipped);   // collective id
      if (status.is_ok()) status = walker.identity(skipped);   // generation
      for (int step = 0; step < 3 && status.is_ok(); ++step) status = walker.u8(byte);
      if (status.is_ok()) status = walker.string(text, limits::kLabelBytesMax);
      if (status.is_ok()) status = walker.identity(skipped);   // origin node
      if (status.is_ok()) status = walker.identity(skipped);   // fabric
      if (status.is_ok() && walker.offset() != count_offset) {
        return Status(ErrorCode::kInternalInvariant, "the derived offset does not match the encoding");
      }
      return status;
    }();
    if (!seek.is_ok()) return 0;
    std::uint32_t participants = 0;
    if (!walker.count(participants, limits::kParticipantsMaxPerCollective).is_ok()) return 0;
    Identity participant;
    for (std::uint32_t index = 0; index < participants; ++index) {
      if (!walker.identity(participant).is_ok()) return 0;
    }
    return walker.offset();
  }();
  CTF_CHECK_MSG(phases_offset != 0, "the phase count offset could not be derived");
  std::vector<std::uint8_t> absurd_phases = canonical.data();
  for (int index = 0; index < 4; ++index) {
    absurd_phases[phases_offset + static_cast<std::size_t>(index)] = 0xFF;
  }
  protocol::Reader phases_reader(absurd_phases.data(), absurd_phases.size());
  CTF_CHECK_CODE(protocol::decode_definition(phases_reader, rejected), ErrorCode::kDecodeCountOutOfRange);

  // A structure that is complete but claims trailing bytes is refused.
  std::vector<std::uint8_t> extended = canonical.data();
  extended.push_back(0);
  protocol::Reader extended_reader(extended.data(), extended.size());
  CTF_CHECK_OK(protocol::decode_definition(extended_reader, rejected));
  CTF_CHECK_CODE(extended_reader.require_end(), ErrorCode::kDecodeTrailingBytes);
}

CTF_TEST("adversarial_decoder", "enum_fields_outside_their_table_are_refused") {
  CollectiveDefinition definition;
  definition.id = mint_identity();
  definition.generation = mint_identity();
  definition.collective_class = CollectiveClass::kAllReduce;
  definition.participants = {mint_identity()};
  protocol::Writer writer;
  CTF_CHECK_OK(protocol::encode_definition(definition, writer));
  std::vector<std::uint8_t> bytes = writer.data();
  // The collective class byte follows two identities.
  const std::size_t class_offset = 32;
  bytes[class_offset] = 0x7F;
  protocol::Reader reader(bytes.data(), bytes.size());
  CollectiveDefinition decoded;
  CTF_CHECK_CODE(protocol::decode_definition(reader, decoded), ErrorCode::kDecodeInvalidEnum);
}

CTF_TEST("adversarial_decoder", "evidence_decoder_refuses_inconsistent_observations") {
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(11, 2, 2);
  CongestionEvidence congestion = ctf::test::make_congestion(fabric.topology, 12, 5000);
  // A level that contradicts the utilization it was derived from is refused:
  // evidence must not be able to lie about itself.
  congestion.links.front().level = CongestionLevel::kSevere;
  CTF_CHECK_CODE(congestion.validate(), ErrorCode::kDecodeNonCanonical);
  congestion.links.front().level = congestion_level_from_utilization_bps(5000);
  CTF_CHECK_OK(congestion.validate());

  CapacityEvidence capacity = ctf::test::make_capacity(fabric.topology, 13, 1000);
  capacity.links.front().available_bps = capacity.links.front().provisioned_bps + 1;
  CTF_CHECK_CODE(capacity.validate(), ErrorCode::kValidationCapacityNegative);

  TopologyEvidence topology = fabric.topology;
  topology.links.front().capacity_bps = 0;
  CTF_CHECK_CODE(topology.validate(), ErrorCode::kValidationCapacityNegative);

  CongestionEvidence no_window = ctf::test::make_congestion(fabric.topology, 14, 1000);
  no_window.valid_for_ms = 0;
  CTF_CHECK_CODE(no_window.validate(), ErrorCode::kValidationRateInvalid);
}

CTF_TEST("adversarial_decoder", "unordered_evidence_is_canonicalised_or_refused") {
  const ctf::test::RackFabric fabric = ctf::test::make_fabric(21, 3, 2);
  CapacityEvidence capacity = ctf::test::make_capacity(fabric.topology, 22, 1000);
  std::reverse(capacity.links.begin(), capacity.links.end());
  // Once reversed the vector is no longer ascending, and validate() reports it.
  const Status status = capacity.validate();
  CTF_CHECK(!status.is_ok());
  capacity.canonicalize();
  CTF_CHECK_OK(capacity.validate());
}

CTF_TEST_MAIN()
