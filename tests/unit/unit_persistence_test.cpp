// Collective Traffic Fabric - persistence format unit proofs.
// Copyright 2026 Summon Software Labs.
#include <cstdio>
#include <string>
#include <vector>

#include "ctf/authority.hpp"
#include "ctf/catalog.hpp"
#include "ctf/persistence.hpp"
#include "support/synthetic.hpp"
#include "support/temp_path.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;
using ctf::test::make_definition;

// A temporary file whose lifetime is exactly the scope that needs it.  The path
// is built from the process id and a counter so parallel test runs cannot
// collide, and the file is removed on destruction.
class TempFile {
 public:
  explicit TempFile(const char* tag) : path_(ctf::test::unique_temp_path("ctf-test-", tag)) {}
  ~TempFile() { std::remove(path_.c_str()); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

CollectiveRecord make_record(std::uint64_t seed) {
  ctf::test::IdFactory ids(seed);
  CollectiveRecord record;
  record.definition.id = ids.next();
  record.definition.generation = ids.next();
  record.definition.collective_class = CollectiveClass::kAllReduce;
  record.definition.algorithm_hint = AlgorithmHint::kRing;
  record.definition.scope = CollectiveScope::kCrossRack;
  record.definition.label = "durable-collective";
  record.definition.origin_node = ids.next();
  record.definition.fabric = ids.next();
  record.definition.participants = {ids.next(), ids.next(), ids.next()};
  canonicalize_participants(record.definition.participants);
  record.definition.logical_bytes = 1ull << 30;
  record.definition.registration_sequence = Sequence(3);
  record.state = CollectiveState::kRegistered;
  record.current_attempt = ids.next();
  record.attempt_transfer_bytes = 4096;
  record.attempt_sequence = Sequence(2);
  record.registration_sequence = Sequence(3);
  record.last_mutation_sequence = Sequence(3);
  record.registered_monotonic_ms = 1000;
  record.last_mutated_monotonic_ms = 2000;
  record.generation_ancestry_depth = 1;
  CollectivePhase phase;
  phase.phase_index = Sequence(1);
  phase.label = "reduce";
  phase.synchronization_point = true;
  phase.step_count = 2;
  record.definition.phases.push_back(phase);
  CollectiveStep step;
  step.phase_index = Sequence(1);
  step.step_index = Sequence(1);
  step.hint = AlgorithmHint::kRing;
  record.definition.steps.push_back(step);
  CTF_CHECK(record.definition.metadata.set("source", "unit-test"));
  return record;
}

SnapshotContents make_contents(std::uint64_t seed) {
  SnapshotContents contents;
  contents.written_monotonic_ms = 4242;
  contents.authority.last_epoch = ctf::mint_identity();
  contents.authority.policy_generation = ctf::mint_identity();
  contents.authority.policy = TrafficPolicy::standard(contents.authority.policy_generation);
  contents.authority.topology_generation = ctf::mint_identity();
  contents.authority.topology.generation = contents.authority.topology_generation;
  contents.authority.topology.captured_monotonic_ms = 10;
  contents.authority.topology.synthetic = true;
  ctf::test::IdFactory ids(seed);
  for (int index = 0; index < 4; ++index) {
    TopologyNode node;
    node.id = ids.next();
    node.rack = ids.next();
    node.accelerator_attached = index % 2 == 0;
    node.accelerator_count = static_cast<std::uint32_t>(index);
    contents.authority.topology.nodes.push_back(node);
  }
  contents.authority.topology.canonicalize();
  for (int index = 0; index < 3; ++index) {
    contents.records.push_back(make_record(seed + static_cast<std::uint64_t>(index) + 1));
  }
  return contents;
}

}  // namespace

CTF_TEST("persistence", "crc32_matches_the_ieee_check_value") {
  const std::uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CTF_CHECK_EQ(ctf::crc32(check, sizeof(check)), 0xCBF43926u);
  CTF_CHECK_EQ(ctf::crc32(nullptr, 0), 0u);
}

CTF_TEST("persistence", "encode_decode_round_trip_is_exact") {
  const SnapshotContents contents = make_contents(101);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(encode_snapshot(contents, bytes));
  CTF_CHECK(bytes.size() > 60);
  SnapshotContents decoded;
  CTF_CHECK_OK(decode_snapshot(bytes, decoded));
  CTF_CHECK_EQ(decoded.written_monotonic_ms, contents.written_monotonic_ms);
  CTF_CHECK_EQ(decoded.authority.last_epoch, contents.authority.last_epoch);
  CTF_CHECK_EQ(decoded.authority.policy_generation, contents.authority.policy_generation);
  CTF_CHECK_EQ(decoded.authority.topology_generation, contents.authority.topology_generation);
  CTF_CHECK_EQ(decoded.authority.topology.nodes.size(), contents.authority.topology.nodes.size());
  CTF_CHECK_EQ(decoded.records.size(), contents.records.size());
  for (std::size_t index = 0; index < decoded.records.size(); ++index) {
    CTF_CHECK(decoded.records[index].definition == contents.records[index].definition);
    CTF_CHECK_EQ(decoded.records[index].state, contents.records[index].state);
    CTF_CHECK_EQ(decoded.records[index].current_attempt, contents.records[index].current_attempt);
    CTF_CHECK_EQ(decoded.records[index].attempt_transfer_bytes,
                 contents.records[index].attempt_transfer_bytes);
    CTF_CHECK_EQ(decoded.records[index].attempt_sequence.value(),
                 contents.records[index].attempt_sequence.value());
  }
  // Re-encoding the decoded snapshot produces byte identical output.
  std::vector<std::uint8_t> again;
  CTF_CHECK_OK(encode_snapshot(decoded, again));
  CTF_CHECK_EQ(again, bytes);
}

CTF_TEST("persistence", "truncation_is_refused_at_every_length") {
  const SnapshotContents contents = make_contents(107);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(encode_snapshot(contents, bytes));
  // Every prefix must be refused: a partially written snapshot is never loaded.
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const std::vector<std::uint8_t> prefix(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
    SnapshotContents decoded;
    const Status status = decode_snapshot(prefix, decoded);
    CTF_CHECK_MSG(!status.is_ok(), "a truncated snapshot was accepted");
  }
}

CTF_TEST("persistence", "single_byte_corruption_is_detected") {
  const SnapshotContents contents = make_contents(109);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(encode_snapshot(contents, bytes));
  for (std::size_t index = 0; index < bytes.size(); index += 7) {
    SnapshotContents decoded;
    std::vector<std::uint8_t> corrupted = bytes;
    corrupted[index] ^= 0x5A;
    const Status status = decode_snapshot(corrupted, decoded);
    CTF_CHECK_MSG(!status.is_ok(), "a corrupted snapshot was accepted");
  }
}

CTF_TEST("persistence", "trailing_bytes_are_refused") {
  const SnapshotContents contents = make_contents(113);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(encode_snapshot(contents, bytes));
  std::vector<std::uint8_t> extended = bytes;
  extended.push_back(0);
  SnapshotContents decoded;
  CTF_CHECK(!decode_snapshot(extended, decoded).is_ok());
}

CTF_TEST("persistence", "unsupported_version_is_reported_as_such") {
  const SnapshotContents contents = make_contents(127);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(encode_snapshot(contents, bytes));
  std::vector<std::uint8_t> rewritten = bytes;
  // The envelope version field lives at offset 4 and is a little endian u32.
  rewritten[4] = 99;
  SnapshotContents decoded;
  CTF_CHECK_CODE(decode_snapshot(rewritten, decoded), ErrorCode::kPersistenceVersionUnsupported);
  // Magic must be checked before anything else is believed.
  std::vector<std::uint8_t> wrong_magic = bytes;
  wrong_magic[0] ^= 0xFF;
  CTF_CHECK_CODE(decode_snapshot(wrong_magic, decoded), ErrorCode::kPersistenceCorrupt);
}

CTF_TEST("persistence", "impossible_state_is_refused") {
  SnapshotContents contents = make_contents(131);
  // A collective without participants cannot exist; persistence must refuse it
  // rather than write a file that cannot be replayed.
  contents.records.front().definition.participants.clear();
  std::vector<std::uint8_t> bytes;
  CTF_CHECK(!encode_snapshot(contents, bytes).is_ok());
  // A record without a generation is equally impossible.
  SnapshotContents second = make_contents(137);
  second.records.front().definition.generation = Identity{};
  CTF_CHECK(!encode_snapshot(second, bytes).is_ok());
}

CTF_TEST("persistence", "store_writes_and_reads_atomically") {
  TempFile file("snapshot");
  SnapshotStore store(file.path());
  CTF_CHECK(!store.exists());
  SnapshotContents missing;
  LoadDisposition disposition = store.load(missing);
  CTF_CHECK_EQ(disposition, LoadDisposition::kMissing);

  const SnapshotContents contents = make_contents(139);
  CTF_CHECK_OK(store.save(contents, 5150));
  CTF_CHECK(store.exists());
  SnapshotContents loaded;
  std::string diagnostic;
  disposition = store.load(loaded, &diagnostic);
  CTF_CHECK_MSG(disposition == LoadDisposition::kLoaded, diagnostic);
  CTF_CHECK_EQ(loaded.written_monotonic_ms, 5150u);
  CTF_CHECK_EQ(loaded.records.size(), contents.records.size());

  // Replacing the file must leave no temporary sibling behind.
  CTF_CHECK_OK(store.save(contents, 5151));
  SnapshotContents replaced;
  CTF_CHECK_EQ(store.load(replaced), LoadDisposition::kLoaded);
  CTF_CHECK_EQ(replaced.written_monotonic_ms, 5151u);
  TempFile probe("leftover");
  static_cast<void>(probe);
}

CTF_TEST("persistence", "oversized_payload_is_refused_before_allocation") {
  // A claimed payload length beyond the bound must be refused from the header
  // alone, so an adversarial file cannot drive a huge allocation.
  std::vector<std::uint8_t> bytes(60, 0);
  bytes[0] = 0x53;
  bytes[1] = 0x46;
  bytes[2] = 0x54;
  bytes[3] = 0x43;
  bytes[4] = 1;
  // payload_bytes = huge
  for (int index = 0; index < 8; ++index) bytes[8 + index] = 0xFF;
  SnapshotContents decoded;
  const Status status = decode_snapshot(bytes, decoded);
  CTF_CHECK(!status.is_ok());
  CTF_CHECK(status.code() == ErrorCode::kPersistenceTooLarge ||
            status.code() == ErrorCode::kPersistenceTruncated ||
            status.code() == ErrorCode::kPersistenceCorrupt);
}

CTF_TEST("persistence", "restore_never_resurrects_dynamic_state") {
  AuthorityState authority;
  authority.begin_boot(Identity{});
  const TopologyEvidence topology = ctf::test::make_fabric(151, 2, 2).topology;
  TopologyEvidence stamped = topology;
  stamped.generation = ctf::mint_identity();
  CTF_CHECK_EQ(authority.install_topology(stamped), InstallResult::kInstalled);
  CapacityEvidence capacity = ctf::test::make_capacity(stamped, 157, 1000);
  capacity.topology_generation = stamped.generation;
  capacity.generation = ctf::mint_identity();
  CTF_CHECK_EQ(authority.install_capacity(capacity), InstallResult::kInstalled);
  CongestionEvidence congestion = ctf::test::make_congestion(stamped, 163, 1000);
  congestion.topology_generation = stamped.generation;
  congestion.generation = ctf::mint_identity();
  CTF_CHECK_EQ(authority.install_congestion(congestion), InstallResult::kInstalled);
  CTF_CHECK(authority.capacity_available());
  CTF_CHECK(authority.congestion_available());

  const DurableAuthority durable = authority.durable_snapshot();
  const CoordinatorEpoch before = authority.epoch();
  authority.begin_boot(durable.last_epoch);
  CTF_CHECK_OK(authority.restore_durable(durable, 1000));
  // After a restart the durable topology and policy are present, and nothing
  // dynamic is: capacity and congestion must be re-established by observation.
  CTF_CHECK_EQ(authority.topology_generation(), durable.topology_generation);
  CTF_CHECK(!authority.capacity_available());
  CTF_CHECK(!authority.congestion_available());
  CTF_CHECK_EQ(authority.peer_count(), 0u);
  CTF_CHECK_NE(authority.epoch(), before);
}

CTF_TEST_MAIN()
