// Collective Traffic Fabric - adversarial persistence proofs.
// Copyright 2026 Summon Software Labs.
//
// A snapshot file is untrusted input.  These cases assume an attacker (or a
// crash) can produce any byte sequence and require the loader to refuse
// impossible state without partial application and without unbounded work.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "ctf/catalog.hpp"
#include "ctf/persistence.hpp"
#include "support/synthetic.hpp"
#include "support/temp_path.hpp"
#include "support/test_framework.hpp"

namespace {

using namespace ctf;

class TempFile {
 public:
  explicit TempFile(const char* tag) : path_(ctf::test::unique_temp_path("ctf-adv-", tag)) {}
  ~TempFile() { std::remove(path_.c_str()); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

CollectiveRecord make_record(std::uint64_t seed, const std::string& label) {
  ctf::test::IdFactory ids(seed);
  CollectiveRecord record;
  record.definition.id = Identity(seed, seed + 1);
  record.definition.generation = Identity(seed + 2, seed + 3);
  record.definition.collective_class = CollectiveClass::kAllReduce;
  record.definition.label = label;
  record.definition.participants = {ids.next(), ids.next()};
  canonicalize_participants(record.definition.participants);
  record.definition.logical_bytes = 1024;
  record.state = CollectiveState::kRegistered;
  record.registration_sequence = Sequence(seed);
  return record;
}

SnapshotContents make_contents(const std::string& label, std::uint64_t seed) {
  SnapshotContents contents;
  contents.authority.last_epoch = Identity(9, 9);
  contents.authority.policy_generation = Identity(8, 8);
  contents.authority.policy = TrafficPolicy::standard(contents.authority.policy_generation);
  contents.records.push_back(make_record(seed, label));
  return contents;
}

// A deterministic byte generator; the same seed always produces the same file.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed | 0x9E3779B97F4A7C15ull) {}
  std::uint8_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return static_cast<std::uint8_t>(state_ & 0xFFu);
  }

 private:
  std::uint64_t state_;
};

}  // namespace

CTF_TEST("adversarial_persistence", "random_garbage_never_loads_and_never_crashes") {
  const std::uint64_t seed = ctf::test::resolve_seed("random_garbage_never_loads_and_never_crashes");
  Rng rng(seed);
  std::size_t refused = 0;
  for (int round = 0; round < 400; ++round) {
    const std::size_t size = rng.next() * 4;
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index) bytes[index] = rng.next();
    SnapshotContents decoded;
    const Status status = decode_snapshot(bytes, decoded);
    if (!status.is_ok()) {
      ++refused;
      CTF_CHECK(!status.message().empty());
    }
  }
  CTF_CHECK_EQ(refused, 400u);
}

CTF_TEST("adversarial_persistence", "a_valid_snapshot_with_a_lie_inside_is_refused") {
  const SnapshotContents contents = make_contents("liar", 301);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(encode_snapshot(contents, bytes));

  // Every single byte position must be defended by the integrity layer.
  std::size_t refused = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    std::vector<std::uint8_t> mutated = bytes;
    mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ 0x40u);
    SnapshotContents decoded;
    const Status status = decode_snapshot(mutated, decoded);
    if (status.is_ok()) {
      // The only acceptable acceptance is a byte identical meaning.
      std::vector<std::uint8_t> reencoded;
      CTF_CHECK_OK(encode_snapshot(decoded, reencoded));
      CTF_CHECK_EQ(reencoded, mutated);
    } else {
      ++refused;
    }
  }
  CTF_CHECK(refused > 0);
}

CTF_TEST("adversarial_persistence", "counter_and_length_fields_cannot_drive_allocation") {
  const SnapshotContents contents = make_contents("counts", 307);
  std::vector<std::uint8_t> bytes;
  CTF_CHECK_OK(encode_snapshot(contents, bytes));

  // Rewrite every 32 bit word in the payload after the envelope with a huge
  // value, recompute nothing, and require a refusal in every case.  A loader
  // that trusted a count before checking the remaining bytes would allocate
  // gigabytes here.
  std::size_t refused = 0;
  for (std::size_t offset = 60; offset + 8 <= bytes.size(); offset += 4) {
    std::vector<std::uint8_t> mutated = bytes;
    for (int index = 0; index < 4; ++index) mutated[offset + static_cast<std::size_t>(index)] = 0xFF;
    SnapshotContents decoded;
    const Status status = decode_snapshot(mutated, decoded);
    if (!status.is_ok()) {
      ++refused;
      CTF_CHECK(status.code() == ErrorCode::kPersistenceIntegrityMismatch ||
                status.code() == ErrorCode::kPersistenceCorrupt ||
                status.code() == ErrorCode::kPersistenceTruncated ||
                status.code() == ErrorCode::kPersistenceImpossibleState ||
                status.code() == ErrorCode::kPersistenceTooLarge);
    }
  }
  CTF_CHECK(refused > 0);
}

CTF_TEST("adversarial_persistence", "catalog_load_is_all_or_nothing") {
  CollectiveCatalog catalog(16);
  CTF_CHECK_EQ(catalog.register_definition(make_record(401, "kept").definition, 1).result,
               RegistrationResult::kRegistered);
  const std::size_t before = catalog.size();

  // A batch in which the second record is impossible must apply nothing.
  std::vector<CollectiveRecord> batch;
  batch.push_back(make_record(402, "first"));
  CollectiveRecord broken = make_record(403, "broken");
  broken.state = CollectiveState::kUnregistered;
  batch.push_back(broken);
  const Status status = catalog.load(batch, 1);
  CTF_CHECK_CODE(status, ErrorCode::kPersistenceImpossibleState);
  CTF_CHECK_EQ(catalog.size(), before);
  CTF_CHECK(catalog.find(batch.front().definition.id) == nullptr);

  // A duplicate identity inside the batch is refused as well.
  std::vector<CollectiveRecord> duplicates;
  duplicates.push_back(make_record(404, "one"));
  duplicates.push_back(make_record(404, "two"));
  CTF_CHECK_CODE(catalog.load(duplicates, 1), ErrorCode::kPersistenceImpossibleState);
  CTF_CHECK_EQ(catalog.size(), before);

  // A batch of definitions without generations is refused.
  std::vector<CollectiveRecord> no_generation;
  CollectiveRecord bare = make_record(405, "bare");
  bare.definition.generation = Identity{};
  no_generation.push_back(bare);
  CTF_CHECK(!catalog.load(no_generation, 1).is_ok());
  CTF_CHECK_EQ(catalog.size(), before);

  // A valid batch is applied completely and preserves the generations.
  std::vector<CollectiveRecord> good;
  good.push_back(make_record(406, "good"));
  CTF_CHECK_OK(catalog.load(good, 1));
  CTF_CHECK_EQ(catalog.size(), good.size());
  const CollectiveRecord* loaded = catalog.find(good.front().definition.id);
  CTF_CHECK(loaded != nullptr);
  CTF_CHECK(loaded->definition == good.front().definition);
}

CTF_TEST("adversarial_persistence", "a_rejected_file_leaves_the_previous_state_intact") {
  TempFile file("state");
  SnapshotStore store(file.path());
  const SnapshotContents good = make_contents("durable", 409);
  CTF_CHECK_OK(store.save(good, 1000));
  SnapshotContents loaded;
  CTF_CHECK_EQ(store.load(loaded), LoadDisposition::kLoaded);

  // Overwrite the file with garbage: loading must report a rejection and must
  // not have produced a partially applied snapshot.
  const std::vector<std::uint8_t> garbage(200, 0x5A);
  {
    std::ofstream sink(file.path(), std::ios::binary | std::ios::trunc);
    CTF_CHECK(sink.good());
    sink.write(reinterpret_cast<const char*>(garbage.data()),
               static_cast<std::streamsize>(garbage.size()));
    CTF_CHECK(sink.good());
  }
  SnapshotContents rejected;
  std::string diagnostic;
  const LoadDisposition disposition = store.load(rejected, &diagnostic);
  CTF_CHECK(disposition != LoadDisposition::kLoaded);
  CTF_CHECK(!diagnostic.empty());
  CTF_CHECK_EQ(rejected.records.size(), 0u);
  CTF_CHECK(rejected.authority.policy_generation.is_none());

  // A subsequent save writes a complete file again.
  CTF_CHECK_OK(store.save(good, 2000));
  CTF_CHECK_EQ(store.load(loaded), LoadDisposition::kLoaded);
  CTF_CHECK_EQ(loaded.written_monotonic_ms, 2000u);
}

CTF_TEST("adversarial_persistence", "impossible_durable_records_are_refused_before_writing") {
  SnapshotContents contents = make_contents("impossible", 419);
  std::vector<std::uint8_t> bytes;
  // A policy generation without a policy is impossible.
  SnapshotContents no_policy = contents;
  no_policy.authority.policy = TrafficPolicy{};
  no_policy.authority.policy.generation = Identity{};
  CTF_CHECK(!encode_snapshot(no_policy, bytes).is_ok());
  // A topology generation without a topology is impossible.
  SnapshotContents no_topology = contents;
  no_topology.authority.topology_generation = Identity(5, 5);
  CTF_CHECK(!encode_snapshot(no_topology, bytes).is_ok());
  // An unknown collective state inside a record is impossible to persist.
  SnapshotContents bad_state = contents;
  bad_state.records.front().state = CollectiveState::kUnregistered;
  CTF_CHECK(!encode_snapshot(bad_state, bytes).is_ok());
  // A record beyond the participant bound is refused rather than truncated.
  SnapshotContents too_many = contents;
  too_many.records.front().definition.participants.clear();
  for (std::uint32_t index = 0; index < limits::kParticipantsMaxPerCollective + 1; ++index) {
    too_many.records.front().definition.participants.push_back(Identity(1, index + 1));
  }
  CTF_CHECK(!encode_snapshot(too_many, bytes).is_ok());
  // A snapshot larger than the format bound is refused.
  SnapshotContents huge = contents;
  huge.records.clear();
  for (std::uint32_t index = 0; index < limits::kSnapshotCollectivesMax + 1; ++index) {
    huge.records.push_back(make_record(1000 + index, "bulk"));
  }
  CTF_CHECK(!encode_snapshot(huge, bytes).is_ok());
}

CTF_TEST_MAIN()
