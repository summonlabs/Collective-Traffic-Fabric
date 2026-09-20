// Collective Traffic Fabric - versioned, integrity checked durable state.
// Copyright 2026 Summon Software Labs.
//
// The snapshot format has its own private codec.  It deliberately does not
// share a format with the wire protocol, so a change to one can never silently
// change the meaning of the other.  Both are strict in the same way: bounded
// counts, bounded strings, no trailing bytes, no partial application.
#include "ctf/persistence.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ctf {
namespace {

constexpr std::uint32_t kEnvelopeMagic = 0x43544653u;  // "CTFS"
constexpr std::uint32_t kEnvelopeBytes = 60;
constexpr std::uint32_t kDigestBytes = kSnapshotDigestBytes;

// ---- CRC-32 (IEEE 802.3, reflected) ---------------------------------------
std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t size) noexcept {
  static std::uint32_t table[256];
  static bool initialized = false;
  if (!initialized) {
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0u ? (value >> 1) ^ 0xEDB88320u : (value >> 1);
      }
      table[index] = value;
    }
    initialized = true;
  }
  crc = ~crc;
  for (std::size_t index = 0; index < size; ++index) {
    crc = table[(crc ^ data[index]) & 0xFFu] ^ (crc >> 8);
  }
  return ~crc;
}

// ---- snapshot digest -------------------------------------------------------
// A 32 byte digest over the payload.  It is a deterministic mixing function,
// not a cryptographic hash: it exists to detect corruption and truncation in a
// file this process wrote, and snapshot files are never treated as trusted
// input from another party.
void digest_payload(const std::uint8_t* data, std::size_t size, std::uint8_t out[kDigestBytes]) noexcept {
  std::uint64_t lanes[4] = {0x243F6A8885A308D3ull, 0x13198A2E03707344ull, 0xA4093822299F31D0ull,
                            0x082EFA98EC4E6C89ull};
  for (std::size_t index = 0; index < size; ++index) {
    std::uint64_t& lane = lanes[index % 4];
    lane ^= static_cast<std::uint64_t>(data[index]) + 0x9E3779B97F4A7C15ull + (lane << 6) + (lane >> 2);
    lane *= 0xFF51AFD7ED558CCDull;
    lane ^= lane >> 33;
  }
  for (int round = 0; round < 2; ++round) {
    for (int index = 0; index < 4; ++index) {
      lanes[index] ^= lanes[(index + 1) % 4];
      lanes[index] *= 0xC4CEB9FE1A85EC53ull;
      lanes[index] ^= lanes[index] >> 29;
    }
  }
  for (int index = 0; index < 4; ++index) {
    const std::uint64_t lane = lanes[index];
    for (int byte = 0; byte < 8; ++byte) {
      out[index * 8 + byte] = static_cast<std::uint8_t>((lane >> (byte * 8)) & 0xFFu);
    }
  }
  const std::uint64_t length = static_cast<std::uint64_t>(size);
  for (int byte = 0; byte < 8; ++byte) {
    out[byte] ^= static_cast<std::uint8_t>((length >> (byte * 8)) & 0xFFu);
  }
}

// ---- private snapshot writer ----------------------------------------------
class SnapWriter {
 public:
  void u8(std::uint8_t value) { data_.push_back(value); }
  void u16(std::uint16_t value) {
    data_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  }
  void u32(std::uint32_t value) {
    for (int index = 0; index < 4; ++index) data_.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
  void u64(std::uint64_t value) {
    for (int index = 0; index < 8; ++index) data_.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
  void identity(Identity value) {
    u64(value.high());
    u64(value.low());
  }
  void text(const std::string& value, std::uint32_t max_bytes) {
    const std::uint32_t length =
        static_cast<std::uint32_t>(value.size() > max_bytes ? max_bytes : value.size());
    u32(length);
    data_.insert(data_.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(length));
  }
  void boolean(bool value) { u8(value ? 1u : 0u); }
  template <typename Enum>
  void enumeration(Enum value) {
    u8(static_cast<std::uint8_t>(value));
  }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  [[nodiscard]] std::vector<std::uint8_t>& data() noexcept { return data_; }

 private:
  std::vector<std::uint8_t> data_;
};

class SnapReader {
 public:
  SnapReader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] bool u8(std::uint8_t& out) {
    if (offset_ + 1 > size_) return false;
    out = data_[offset_++];
    return true;
  }
  [[nodiscard]] bool u16(std::uint16_t& out) {
    if (offset_ + 2 > size_) return false;
    out = static_cast<std::uint16_t>(data_[offset_]) |
          static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
    offset_ += 2;
    return true;
  }
  [[nodiscard]] bool u32(std::uint32_t& out) {
    if (offset_ + 4 > size_) return false;
    out = 0;
    for (int index = 3; index >= 0; --index) {
      out = (out << 8) | data_[offset_ + static_cast<std::size_t>(index)];
    }
    offset_ += 4;
    return true;
  }
  [[nodiscard]] bool u64(std::uint64_t& out) {
    if (offset_ + 8 > size_) return false;
    out = 0;
    for (int index = 7; index >= 0; --index) {
      out = (out << 8) | data_[offset_ + static_cast<std::size_t>(index)];
    }
    offset_ += 8;
    return true;
  }
  [[nodiscard]] bool identity(Identity& out) {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    if (!u64(hi) || !u64(lo)) return false;
    out = Identity(hi, lo);
    return true;
  }
  [[nodiscard]] bool text(std::string& out, std::uint32_t max_bytes) {
    std::uint32_t length = 0;
    if (!u32(length)) return false;
    if (length > max_bytes) return false;
    if (offset_ + length > size_) return false;
    out.assign(reinterpret_cast<const char*>(data_ + offset_), length);
    offset_ += length;
    return true;
  }
  [[nodiscard]] bool boolean(bool& out) {
    std::uint8_t raw = 0;
    if (!u8(raw)) return false;
    if (raw > 1) return false;
    out = raw == 1;
    return true;
  }
  template <typename Enum>
  [[nodiscard]] bool enumeration(Enum& out, std::uint8_t max_value) {
    std::uint8_t raw = 0;
    if (!u8(raw)) return false;
    if (raw > max_value) return false;
    out = static_cast<Enum>(raw);
    return true;
  }
  [[nodiscard]] bool count(std::uint32_t& out, std::uint32_t max) {
    if (!u32(out)) return false;
    return out <= max;
  }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == size_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

// ---- field codecs ----------------------------------------------------------
void write_metadata(SnapWriter& writer, const Metadata& metadata) {
  writer.u32(static_cast<std::uint32_t>(metadata.entries().size()));
  for (const auto& entry : metadata.entries()) {
    writer.text(entry.first, limits::kMetadataKeyBytesMax);
    writer.text(entry.second, limits::kMetadataValueBytesMax);
  }
}

bool read_metadata(SnapReader& reader, Metadata& metadata) {
  std::uint32_t count = 0;
  if (!reader.count(count, limits::kMetadataEntriesMax)) return false;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string key;
    std::string value;
    if (!reader.text(key, limits::kMetadataKeyBytesMax)) return false;
    if (!reader.text(value, limits::kMetadataValueBytesMax)) return false;
    if (!metadata.set(key, value)) return false;
  }
  return true;
}

void write_definition(SnapWriter& writer, const CollectiveDefinition& definition) {
  writer.identity(definition.id);
  writer.identity(definition.generation);
  writer.enumeration(definition.collective_class);
  writer.enumeration(definition.algorithm_hint);
  writer.enumeration(definition.scope);
  writer.text(definition.label, limits::kLabelBytesMax);
  writer.identity(definition.origin_node);
  writer.identity(definition.fabric);
  writer.u32(static_cast<std::uint32_t>(definition.participants.size()));
  for (const ParticipantId& participant : definition.participants) writer.identity(participant);
  writer.u32(static_cast<std::uint32_t>(definition.phases.size()));
  for (const CollectivePhase& phase : definition.phases) {
    writer.u64(phase.phase_index.value());
    writer.text(phase.label, limits::kLabelBytesMax);
    writer.boolean(phase.synchronization_point);
    writer.u32(phase.step_count);
  }
  writer.u32(static_cast<std::uint32_t>(definition.steps.size()));
  for (const CollectiveStep& step : definition.steps) {
    writer.u64(step.phase_index.value());
    writer.u64(step.step_index.value());
    writer.enumeration(step.hint);
    writer.boolean(step.synchronization_point);
  }
  write_metadata(writer, definition.metadata);
  writer.boolean(definition.declares_barrier_semantics);
  writer.u64(definition.logical_bytes);
  writer.u64(definition.registration_sequence.value());
}

bool read_definition(SnapReader& reader, CollectiveDefinition& definition) {
  if (!reader.identity(definition.id)) return false;
  if (!reader.identity(definition.generation)) return false;
  if (!reader.enumeration(definition.collective_class, 14)) return false;
  if (!reader.enumeration(definition.algorithm_hint, 11)) return false;
  if (!reader.enumeration(definition.scope, 5)) return false;
  if (!reader.text(definition.label, limits::kLabelBytesMax)) return false;
  if (!reader.identity(definition.origin_node)) return false;
  if (!reader.identity(definition.fabric)) return false;
  std::uint32_t participants = 0;
  if (!reader.count(participants, limits::kParticipantsMaxPerCollective)) return false;
  definition.participants.resize(participants);
  for (std::uint32_t index = 0; index < participants; ++index) {
    if (!reader.identity(definition.participants[index])) return false;
  }
  std::uint32_t phases = 0;
  if (!reader.count(phases, limits::kPhasesMaxPerCollective)) return false;
  definition.phases.resize(phases);
  for (std::uint32_t index = 0; index < phases; ++index) {
    CollectivePhase& phase = definition.phases[index];
    std::uint64_t index_value = 0;
    if (!reader.u64(index_value)) return false;
    phase.phase_index = Sequence(index_value);
    if (!reader.text(phase.label, limits::kLabelBytesMax)) return false;
    if (!reader.boolean(phase.synchronization_point)) return false;
    if (!reader.u32(phase.step_count)) return false;
  }
  std::uint32_t steps = 0;
  if (!reader.count(steps, limits::kPhasesMaxPerCollective * limits::kStepsMaxPerPhase)) return false;
  definition.steps.resize(steps);
  for (std::uint32_t index = 0; index < steps; ++index) {
    CollectiveStep& step = definition.steps[index];
    std::uint64_t phase_index = 0;
    std::uint64_t step_index = 0;
    if (!reader.u64(phase_index)) return false;
    if (!reader.u64(step_index)) return false;
    step.phase_index = Sequence(phase_index);
    step.step_index = Sequence(step_index);
    if (!reader.enumeration(step.hint, 11)) return false;
    if (!reader.boolean(step.synchronization_point)) return false;
  }
  if (!read_metadata(reader, definition.metadata)) return false;
  if (!reader.boolean(definition.declares_barrier_semantics)) return false;
  if (!reader.u64(definition.logical_bytes)) return false;
  std::uint64_t registration_sequence = 0;
  if (!reader.u64(registration_sequence)) return false;
  definition.registration_sequence = Sequence(registration_sequence);
  return true;
}

void write_record(SnapWriter& writer, const CollectiveRecord& record) {
  write_definition(writer, record.definition);
  writer.enumeration(record.state);
  writer.identity(record.current_attempt);
  writer.u64(record.attempt_transfer_bytes);
  writer.u64(record.attempt_sequence.value());
  writer.u64(record.registration_sequence.value());
  writer.u64(record.last_mutation_sequence.value());
  writer.u64(record.registered_monotonic_ms);
  writer.u64(record.last_mutated_monotonic_ms);
  writer.u32(record.generation_ancestry_depth);
}

bool read_record(SnapReader& reader, CollectiveRecord& record) {
  if (!read_definition(reader, record.definition)) return false;
  if (!reader.enumeration(record.state, 8)) return false;
  if (!reader.identity(record.current_attempt)) return false;
  if (!reader.u64(record.attempt_transfer_bytes)) return false;
  std::uint64_t attempt_sequence = 0;
  std::uint64_t registration_sequence = 0;
  std::uint64_t last_mutation_sequence = 0;
  if (!reader.u64(attempt_sequence)) return false;
  if (!reader.u64(registration_sequence)) return false;
  if (!reader.u64(last_mutation_sequence)) return false;
  record.attempt_sequence = Sequence(attempt_sequence);
  record.registration_sequence = Sequence(registration_sequence);
  record.last_mutation_sequence = Sequence(last_mutation_sequence);
  if (!reader.u64(record.registered_monotonic_ms)) return false;
  if (!reader.u64(record.last_mutated_monotonic_ms)) return false;
  if (!reader.u32(record.generation_ancestry_depth)) return false;
  return true;
}

void write_topology(SnapWriter& writer, const TopologyEvidence& topology) {
  writer.identity(topology.generation);
  writer.u64(topology.captured_monotonic_ms);
  writer.boolean(topology.synthetic);
  writer.u32(static_cast<std::uint32_t>(topology.nodes.size()));
  for (const TopologyNode& node : topology.nodes) {
    writer.identity(node.id);
    writer.identity(node.rack);
    writer.boolean(node.accelerator_attached);
    writer.u32(node.accelerator_count);
  }
  writer.u32(static_cast<std::uint32_t>(topology.links.size()));
  for (const TopologyLink& link : topology.links) {
    writer.identity(link.id);
    writer.identity(link.a);
    writer.identity(link.b);
    writer.enumeration(link.link_class);
    writer.u64(link.capacity_bps);
    writer.u32(link.lanes);
    writer.boolean(link.shared_oversubscribed);
  }
}

bool read_topology(SnapReader& reader, TopologyEvidence& topology) {
  if (!reader.identity(topology.generation)) return false;
  if (!reader.u64(topology.captured_monotonic_ms)) return false;
  if (!reader.boolean(topology.synthetic)) return false;
  std::uint32_t nodes = 0;
  if (!reader.count(nodes, limits::kTopologyNodesMax)) return false;
  topology.nodes.resize(nodes);
  for (std::uint32_t index = 0; index < nodes; ++index) {
    TopologyNode& node = topology.nodes[index];
    if (!reader.identity(node.id)) return false;
    if (!reader.identity(node.rack)) return false;
    if (!reader.boolean(node.accelerator_attached)) return false;
    if (!reader.u32(node.accelerator_count)) return false;
  }
  std::uint32_t links = 0;
  if (!reader.count(links, limits::kTopologyLinksMax)) return false;
  topology.links.resize(links);
  for (std::uint32_t index = 0; index < links; ++index) {
    TopologyLink& link = topology.links[index];
    if (!reader.identity(link.id)) return false;
    if (!reader.identity(link.a)) return false;
    if (!reader.identity(link.b)) return false;
    if (!reader.enumeration(link.link_class, 5)) return false;
    if (!reader.u64(link.capacity_bps)) return false;
    if (!reader.u32(link.lanes)) return false;
    if (!reader.boolean(link.shared_oversubscribed)) return false;
  }
  return true;
}

void write_policy(SnapWriter& writer, const TrafficPolicy& policy) {
  writer.identity(policy.generation);
  writer.u64(policy.default_maximum_bandwidth_bps);
  writer.u64(policy.burst_multiplier_per_mille);
  for (std::size_t index = 0; index < CongestionResponseTable::kBucketCount; ++index) {
    writer.u32(policy.congestion_response.utilization_bps[index]);
    writer.u32(policy.congestion_response.rate_numerator_per_mille[index]);
  }
  writer.u32(static_cast<std::uint32_t>(policy.class_policies.size()));
  for (const TrafficClassPolicy& entry : policy.class_policies) {
    writer.enumeration(entry.traffic_class);
    writer.u64(entry.priority);
    writer.u64(entry.minimum_bandwidth_bps);
    writer.u64(entry.maximum_bandwidth_bps);
    writer.u64(entry.burst_bytes);
    writer.boolean(entry.requires_simultaneous_start);
    writer.boolean(entry.requires_evidence_freshness);
    writer.enumeration(entry.isolation);
  }
  writer.u32(static_cast<std::uint32_t>(policy.collective_policies.size()));
  for (const CollectiveClassPolicy& entry : policy.collective_policies) {
    writer.enumeration(entry.collective_class);
    writer.enumeration(entry.traffic_class);
    writer.boolean(entry.admit_when_unknown_semantics);
    writer.boolean(entry.allow_member_override);
    writer.u64(entry.freshness_requirement_ms);
  }
}

bool read_policy(SnapReader& reader, TrafficPolicy& policy) {
  if (!reader.identity(policy.generation)) return false;
  if (!reader.u64(policy.default_maximum_bandwidth_bps)) return false;
  if (!reader.u64(policy.burst_multiplier_per_mille)) return false;
  for (std::size_t index = 0; index < CongestionResponseTable::kBucketCount; ++index) {
    if (!reader.u32(policy.congestion_response.utilization_bps[index])) return false;
    if (!reader.u32(policy.congestion_response.rate_numerator_per_mille[index])) return false;
  }
  std::uint32_t classes = 0;
  if (!reader.count(classes, 16)) return false;
  policy.class_policies.resize(classes);
  for (std::uint32_t index = 0; index < classes; ++index) {
    TrafficClassPolicy& entry = policy.class_policies[index];
    if (!reader.enumeration(entry.traffic_class, 7)) return false;
    if (!reader.u64(entry.priority)) return false;
    if (!reader.u64(entry.minimum_bandwidth_bps)) return false;
    if (!reader.u64(entry.maximum_bandwidth_bps)) return false;
    if (!reader.u64(entry.burst_bytes)) return false;
    if (!reader.boolean(entry.requires_simultaneous_start)) return false;
    if (!reader.boolean(entry.requires_evidence_freshness)) return false;
    if (!reader.enumeration(entry.isolation, 2)) return false;
  }
  std::uint32_t collectives = 0;
  if (!reader.count(collectives, 16)) return false;
  policy.collective_policies.resize(collectives);
  for (std::uint32_t index = 0; index < collectives; ++index) {
    CollectiveClassPolicy& entry = policy.collective_policies[index];
    if (!reader.enumeration(entry.collective_class, 14)) return false;
    if (!reader.enumeration(entry.traffic_class, 7)) return false;
    if (!reader.boolean(entry.admit_when_unknown_semantics)) return false;
    if (!reader.boolean(entry.allow_member_override)) return false;
    if (!reader.u64(entry.freshness_requirement_ms)) return false;
  }
  return true;
}

// ---- OS helpers ------------------------------------------------------------
Status write_all(const std::string& path, const std::vector<std::uint8_t>& bytes, bool& rename_failed) {
  rename_failed = false;
  std::string temporary = path;
  temporary += ".tmp";
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, temporary.c_str(), "wb") != 0) file = nullptr;
#else
  file = std::fopen(temporary.c_str(), "wb");
#endif
  if (file == nullptr) {
    return Status(ErrorCode::kPersistenceOpenFailed, "cannot open temporary snapshot file for writing");
  }
  if (!bytes.empty()) {
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (written != bytes.size()) {
      std::fclose(file);
      std::remove(temporary.c_str());
      return Status(ErrorCode::kPersistenceWriteFailed, "short write while persisting the snapshot");
    }
  }
  if (std::fflush(file) != 0) {
    std::fclose(file);
    std::remove(temporary.c_str());
    return Status(ErrorCode::kPersistenceWriteFailed, "flush failed while persisting the snapshot");
  }
#if defined(_WIN32)
  const int descriptor = _fileno(file);
  if (descriptor >= 0) {
    _commit(descriptor);
  }
#else
  const int descriptor = fileno(file);
  if (descriptor >= 0) {
    ::fsync(descriptor);
  }
#endif
  if (std::fclose(file) != 0) {
    std::remove(temporary.c_str());
    return Status(ErrorCode::kPersistenceWriteFailed, "close failed while persisting the snapshot");
  }

  // Atomic replacement.  On Windows a plain rename fails when the destination
  // exists, so the destination is removed first and a failure to complete the
  // rename is reported rather than ignored (the previous file is already gone
  // only in the narrow window between the two calls, which is why the caller
  // must treat a rename failure as a hard error).
#if defined(_WIN32)
  std::remove(path.c_str());
#endif
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    rename_failed = true;
    return Status(ErrorCode::kPersistenceRenameFailed, "atomic replacement of the snapshot file failed");
  }
  return Status::ok();
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
  return crc32_update(0, data, size);
}

std::string_view to_string(LoadDisposition value) noexcept {
  switch (value) {
    case LoadDisposition::kLoaded: return "loaded";
    case LoadDisposition::kMissing: return "missing";
    case LoadDisposition::kRejectedCorrupt: return "rejected_corrupt";
    case LoadDisposition::kRejectedTruncated: return "rejected_truncated";
    case LoadDisposition::kRejectedIntegrity: return "rejected_integrity";
    case LoadDisposition::kRejectedVersion: return "rejected_version";
    case LoadDisposition::kRejectedOversized: return "rejected_oversized";
    case LoadDisposition::kRejectedImpossible: return "rejected_impossible";
    case LoadDisposition::kRejectedIo: return "rejected_io";
  }
  return "rejected_corrupt";
}

Status encode_snapshot(const SnapshotContents& contents, std::vector<std::uint8_t>& out) {
  if (contents.authority.policy_generation.is_some()) {
    Status status = contents.authority.policy.validate();
    if (!status.is_ok()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    std::string("refusing to persist an invalid policy: ") + status.message());
    }
  }
  if (contents.authority.topology_generation.is_some()) {
    Status status = contents.authority.topology.validate();
    if (!status.is_ok()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    std::string("refusing to persist an invalid topology: ") + status.message());
    }
  }
  if (contents.records.size() > limits::kSnapshotCollectivesMax) {
    return Status(ErrorCode::kPersistenceTooLarge, "snapshot holds more collectives than the format bound");
  }
  for (const CollectiveRecord& record : contents.records) {
    if (record.definition.id.is_none() || record.definition.generation.is_none()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    "refusing to persist a collective without an identity or generation");
    }
    if (record.definition.participants.empty()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    "refusing to persist a collective with no participants");
    }
    if (record.definition.participants.size() > limits::kParticipantsMaxPerCollective) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    "refusing to persist a collective beyond the participant bound");
    }
    if (record.state == CollectiveState::kUnregistered) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    "refusing to persist an unregistered collective");
    }
  }

  SnapWriter payload;
  payload.u32(kSnapshotFormatVersion);
  payload.u64(contents.written_monotonic_ms);
  payload.identity(contents.authority.last_epoch);
  payload.identity(contents.authority.policy_generation);
  payload.identity(contents.authority.topology_generation);
  payload.u64(contents.authority.restart_count);
  payload.boolean(contents.authority.topology_generation.is_some());
  if (contents.authority.topology_generation.is_some()) {
    write_topology(payload, contents.authority.topology);
  }
  payload.boolean(contents.authority.policy_generation.is_some());
  if (contents.authority.policy_generation.is_some()) {
    write_policy(payload, contents.authority.policy);
  }
  payload.u32(static_cast<std::uint32_t>(contents.records.size()));
  for (const CollectiveRecord& record : contents.records) write_record(payload, record);

  if (payload.size() > limits::kSnapshotBytesMax) {
    return Status(ErrorCode::kPersistenceTooLarge, "encoded snapshot exceeds the size bound");
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve(payload.size() + kEnvelopeBytes);
  SnapWriter envelope;
  envelope.u32(kEnvelopeMagic);
  envelope.u32(kSnapshotFormatVersion);
  envelope.u64(static_cast<std::uint64_t>(payload.size()));
  envelope.u32(crc32(payload.data().data(), payload.size()));
  envelope.u64(contents.written_monotonic_ms);
  std::uint8_t digest[kDigestBytes] = {};
  digest_payload(payload.data().data(), payload.size(), digest);
  for (std::uint8_t byte : digest) envelope.u8(byte);
  bytes = envelope.data();
  if (bytes.size() != kEnvelopeBytes) {
    return Status(ErrorCode::kInternalInvariant, "snapshot envelope has an unexpected size");
  }
  bytes.insert(bytes.end(), payload.data().begin(), payload.data().end());
  out = std::move(bytes);
  return Status::ok();
}

Status decode_snapshot(const std::vector<std::uint8_t>& bytes, SnapshotContents& out) {
  if (bytes.size() < kEnvelopeBytes) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot is shorter than its envelope");
  }
  if (bytes.size() > limits::kSnapshotBytesMax + kEnvelopeBytes) {
    return Status(ErrorCode::kPersistenceTooLarge, "snapshot exceeds the size bound");
  }
  SnapReader envelope(bytes.data(), kEnvelopeBytes);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint64_t payload_bytes = 0;
  std::uint32_t payload_crc = 0;
  std::uint64_t written = 0;
  if (!envelope.u32(magic) || !envelope.u32(version) || !envelope.u64(payload_bytes) ||
      !envelope.u32(payload_crc) || !envelope.u64(written)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot envelope is incomplete");
  }
  if (magic != kEnvelopeMagic) {
    return Status(ErrorCode::kPersistenceCorrupt, "snapshot magic does not match this format");
  }
  if (version != kSnapshotFormatVersion) {
    return Status(ErrorCode::kPersistenceVersionUnsupported, "snapshot format version is not supported");
  }
  if (payload_bytes > limits::kSnapshotBytesMax) {
    return Status(ErrorCode::kPersistenceTooLarge, "snapshot payload exceeds the size bound");
  }
  if (payload_bytes != bytes.size() - kEnvelopeBytes) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload length disagrees with the file size");
  }
  const std::uint8_t* payload = bytes.data() + kEnvelopeBytes;
  const std::size_t payload_size = static_cast<std::size_t>(payload_bytes);
  if (crc32(payload, payload_size) != payload_crc) {
    return Status(ErrorCode::kPersistenceIntegrityMismatch, "snapshot payload CRC32 does not match");
  }
  std::uint8_t digest[kDigestBytes] = {};
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    std::uint8_t byte = 0;
    if (!envelope.u8(byte)) {
      return Status(ErrorCode::kPersistenceTruncated, "snapshot digest is incomplete");
    }
    digest[index] = byte;
  }
  std::uint8_t expected[kDigestBytes] = {};
  digest_payload(payload, payload_size, expected);
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    if (digest[index] != expected[index]) {
      return Status(ErrorCode::kPersistenceIntegrityMismatch, "snapshot digest does not match its payload");
    }
  }

  SnapshotContents contents;
  SnapReader reader(payload, payload_size);
  std::uint32_t format = 0;
  if (!reader.u32(format)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload is incomplete");
  }
  if (format != kSnapshotFormatVersion) {
    return Status(ErrorCode::kPersistenceVersionUnsupported, "snapshot payload version is not supported");
  }
  contents.format_version = format;
  if (!reader.u64(contents.written_monotonic_ms)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload is incomplete");
  }
  if (contents.written_monotonic_ms != written) {
    // The envelope and the payload both carry the write timestamp.  Two
    // different values mean one of them was altered, which is exactly the kind
    // of corruption that must never load.
    return Status(ErrorCode::kPersistenceCorrupt,
                  "snapshot envelope and payload disagree about the write time");
  }
  if (!reader.identity(contents.authority.last_epoch)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload is incomplete");
  }
  if (!reader.identity(contents.authority.policy_generation)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload is incomplete");
  }
  if (!reader.identity(contents.authority.topology_generation)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload is incomplete");
  }
  if (!reader.u64(contents.authority.restart_count)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload is incomplete");
  }
  bool has_topology = false;
  if (!reader.boolean(has_topology)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload is incomplete");
  }
  if (has_topology) {
    if (!read_topology(reader, contents.authority.topology)) {
      return Status(ErrorCode::kPersistenceCorrupt, "snapshot topology section is malformed");
    }
    if (contents.authority.topology.generation != contents.authority.topology_generation) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    "snapshot topology generation disagrees with the durable record");
    }
  } else if (contents.authority.topology_generation.is_some()) {
    return Status(ErrorCode::kPersistenceImpossibleState,
                  "snapshot claims a topology generation but carries no topology");
  }
  bool has_policy = false;
  if (!reader.boolean(has_policy)) {
    return Status(ErrorCode::kPersistenceTruncated, "snapshot payload is incomplete");
  }
  if (has_policy) {
    if (!read_policy(reader, contents.authority.policy)) {
      return Status(ErrorCode::kPersistenceCorrupt, "snapshot policy section is malformed");
    }
    Status status = contents.authority.policy.validate();
    if (!status.is_ok()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    std::string("snapshot policy is not usable: ") + status.message());
    }
    if (contents.authority.policy.generation != contents.authority.policy_generation) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    "snapshot policy generation disagrees with the durable record");
    }
  } else if (contents.authority.policy_generation.is_some()) {
    return Status(ErrorCode::kPersistenceImpossibleState,
                  "snapshot claims a policy generation but carries no policy");
  }
  if (has_topology) {
    Status status = contents.authority.topology.validate();
    if (!status.is_ok()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    std::string("snapshot topology is not usable: ") + status.message());
    }
  }
  std::uint32_t records = 0;
  if (!reader.count(records, limits::kSnapshotCollectivesMax)) {
    return Status(ErrorCode::kPersistenceCorrupt, "snapshot collective count is out of range");
  }
  contents.records.resize(records);
  for (std::uint32_t index = 0; index < records; ++index) {
    if (!read_record(reader, contents.records[index])) {
      return Status(ErrorCode::kPersistenceCorrupt, "snapshot collective section is malformed");
    }
  }
  if (!reader.at_end()) {
    return Status(ErrorCode::kPersistenceCorrupt, "snapshot payload carries trailing bytes");
  }
  out = std::move(contents);
  return Status::ok();
}

SnapshotStore::SnapshotStore(std::string path) : path_(std::move(path)) {}

bool SnapshotStore::exists() const {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path_.c_str(), "rb") != 0) file = nullptr;
#else
  file = std::fopen(path_.c_str(), "rb");
#endif
  if (file == nullptr) return false;
  std::fclose(file);
  return true;
}

Status SnapshotStore::save(const SnapshotContents& contents, std::uint64_t now_monotonic_ms) {
  if (path_.empty()) {
    return Status(ErrorCode::kPersistenceDirectoryUnavailable, "no snapshot path is configured");
  }
  SnapshotContents stamped = contents;
  stamped.written_monotonic_ms = now_monotonic_ms;
  std::vector<std::uint8_t> bytes;
  Status status = encode_snapshot(stamped, bytes);
  if (!status.is_ok()) return status;
  bool rename_failed = false;
  status = write_all(path_, bytes, rename_failed);
  static_cast<void>(rename_failed);
  return status;
}

LoadDisposition SnapshotStore::load(SnapshotContents& out, std::string* diagnostic) {
  if (path_.empty()) {
    if (diagnostic != nullptr) *diagnostic = "no snapshot path is configured";
    return LoadDisposition::kRejectedIo;
  }
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path_.c_str(), "rb") != 0) file = nullptr;
#else
  file = std::fopen(path_.c_str(), "rb");
#endif
  if (file == nullptr) {
    if (diagnostic != nullptr) *diagnostic = "snapshot file is absent";
    return LoadDisposition::kMissing;
  }
  std::vector<std::uint8_t> bytes;
  std::uint8_t buffer[64 * 1024];
  for (;;) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read != 0) {
      if (bytes.size() + read > limits::kSnapshotBytesMax + kEnvelopeBytes) {
        std::fclose(file);
        if (diagnostic != nullptr) *diagnostic = "snapshot file exceeds the size bound";
        return LoadDisposition::kRejectedOversized;
      }
      bytes.insert(bytes.end(), buffer, buffer + read);
    }
    if (read < sizeof(buffer)) {
      if (std::ferror(file) != 0) {
        std::fclose(file);
        if (diagnostic != nullptr) *diagnostic = "snapshot file could not be read completely";
        return LoadDisposition::kRejectedIo;
      }
      break;
    }
  }
  std::fclose(file);

  SnapshotContents contents;
  Status status = decode_snapshot(bytes, contents);
  if (!status.is_ok()) {
    if (diagnostic != nullptr) *diagnostic = status.message();
    switch (status.code()) {
      case ErrorCode::kPersistenceTruncated: return LoadDisposition::kRejectedTruncated;
      case ErrorCode::kPersistenceIntegrityMismatch: return LoadDisposition::kRejectedIntegrity;
      case ErrorCode::kPersistenceVersionUnsupported: return LoadDisposition::kRejectedVersion;
      case ErrorCode::kPersistenceTooLarge: return LoadDisposition::kRejectedOversized;
      case ErrorCode::kPersistenceImpossibleState: return LoadDisposition::kRejectedImpossible;
      default: return LoadDisposition::kRejectedCorrupt;
    }
  }
  out = std::move(contents);
  return LoadDisposition::kLoaded;
}

}  // namespace ctf
