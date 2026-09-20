// Collective Traffic Fabric - versioned, integrity checked durable state file.
// Copyright 2026 Summon Software Labs.
//
// Only durable state crosses this boundary: collective definitions, their
// generations, policy, topology description and the last coordinator epoch.
// Liveness, capacity and congestion observations are never written, so a
// restart can never resurrect them.
#ifndef CTF_PERSISTENCE_HPP
#define CTF_PERSISTENCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ctf/authority.hpp"
#include "ctf/catalog.hpp"
#include "ctf/error.hpp"

namespace ctf {

inline constexpr std::uint32_t kSnapshotFormatVersion = 1;
inline constexpr std::uint32_t kSnapshotMagic = 0x43544653u;  // "CTFS"
inline constexpr std::uint32_t kSnapshotDigestBytes = 32;

struct SnapshotContents {
  std::uint32_t format_version = kSnapshotFormatVersion;
  std::uint64_t written_monotonic_ms = 0;
  DurableAuthority authority{};
  std::vector<CollectiveRecord> records;
};

struct SnapshotEnvelope {
  std::uint32_t format_version = 0;
  std::uint64_t payload_bytes = 0;
  std::uint32_t payload_crc32 = 0;
  std::uint64_t written_monotonic_ms = 0;
  std::uint8_t digest[kSnapshotDigestBytes] = {};
};

enum class LoadDisposition : std::uint8_t {
  kLoaded = 0,
  kMissing = 1,           // no file: a first boot, not an error
  kRejectedCorrupt = 2,
  kRejectedTruncated = 3,
  kRejectedIntegrity = 4,
  kRejectedVersion = 5,
  kRejectedOversized = 6,
  kRejectedImpossible = 7,
  kRejectedIo = 8,
};

[[nodiscard]] std::string_view to_string(LoadDisposition value) noexcept;

// Pure serialisation.  No I/O, no clock; the caller supplies timestamps.
[[nodiscard]] Status encode_snapshot(const SnapshotContents& contents, std::vector<std::uint8_t>& out);
[[nodiscard]] Status decode_snapshot(const std::vector<std::uint8_t>& bytes, SnapshotContents& out);

// Atomic, durable replacement: write to a unique temporary sibling, flush,
// rename over the target.  A crash at any point leaves either the previous
// complete file or the new complete file, never a mix, and never a partial
// application of an unvalidated payload.
class SnapshotStore {
 public:
  explicit SnapshotStore(std::string path);

  [[nodiscard]] Status save(const SnapshotContents& contents, std::uint64_t now_monotonic_ms);
  [[nodiscard]] LoadDisposition load(SnapshotContents& out, std::string* diagnostic = nullptr);
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] bool exists() const;

 private:
  std::string path_;
};

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).  Exposed so tests can
// verify the integrity layer independently of the snapshot codec.
[[nodiscard]] std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;

}  // namespace ctf

#endif  // CTF_PERSISTENCE_HPP
