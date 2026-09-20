// Collective Traffic Fabric - strongly typed identities, generations, epochs.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_IDENTITY_HPP
#define CTF_IDENTITY_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ctf {

// ---------------------------------------------------------------------------
// Strongly typed 128-bit identity.
//
// All identities in this runtime are opaque 128-bit values.  They are never
// derived from user supplied strings: the coordinator mints them and binds
// them to the authenticated session envelope.  A value of zero is the "unset"
// or "none" identity and every consumer must treat it as absent rather than as
// a match.
// ---------------------------------------------------------------------------
class Identity {
 public:
  constexpr Identity() noexcept = default;
  constexpr explicit Identity(std::uint64_t hi, std::uint64_t lo) noexcept : hi_(hi), lo_(lo) {}

  [[nodiscard]] constexpr std::uint64_t high() const noexcept { return hi_; }
  [[nodiscard]] constexpr std::uint64_t low() const noexcept { return lo_; }
  [[nodiscard]] constexpr bool is_none() const noexcept { return hi_ == 0 && lo_ == 0; }
  [[nodiscard]] constexpr bool is_some() const noexcept { return !is_none(); }

  // Canonical lowercase hexadecimal, always 32 characters.
  [[nodiscard]] std::string to_string() const;
  // Parses canonical 32 character lowercase hexadecimal.  Returns false for any
  // other spelling; no leniency, no partial parsing.
  [[nodiscard]] static bool parse(std::string_view text, Identity& out) noexcept;

  friend constexpr bool operator==(Identity a, Identity b) noexcept { return a.hi_ == b.hi_ && a.lo_ == b.lo_; }
  friend constexpr bool operator!=(Identity a, Identity b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Identity a, Identity b) noexcept {
    return a.hi_ != b.hi_ ? a.hi_ < b.hi_ : a.lo_ < b.lo_;
  }
  friend constexpr bool operator>(Identity a, Identity b) noexcept { return b < a; }
  friend constexpr bool operator<=(Identity a, Identity b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(Identity a, Identity b) noexcept { return !(a < b); }

 private:
  std::uint64_t hi_ = 0;
  std::uint64_t lo_ = 0;
};

using CollectiveId = Identity;
using CollectiveGeneration = Identity;
using CollectiveAttemptId = Identity;
using ParticipantId = Identity;
using FlowGroupId = Identity;
using TopologyGeneration = Identity;
using PolicyGeneration = Identity;
using CoordinatorEpoch = Identity;
using BootIncarnation = Identity;
using EvidenceGeneration = Identity;
using SessionId = Identity;
using NodeId = Identity;
using RackId = Identity;
using SwitchId = Identity;
using LinkId = Identity;
using FabricInstanceId = Identity;

// Minting is explicit about its source so that provenance is never implied.
class IdentityMinter {
 public:
  // Deterministic, reproducible stream.  Used by tests and by seeded workload
  // generators; never used to mint authority on a live coordinator.
  explicit IdentityMinter(std::uint64_t seed) noexcept : state_(mix_seed(seed)) {}
  [[nodiscard]] Identity next() noexcept;
 private:
  // SplitMix64 finalisation of the seed.  Two seeds that differ in a single bit
  // must produce completely different streams, which a plain OR with a constant
  // does not guarantee.
  [[nodiscard]] static constexpr std::uint64_t mix_seed(std::uint64_t seed) noexcept {
    std::uint64_t value = seed + 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
  }
  std::uint64_t state_;
};

// Generates a fresh identity from operating system entropy plus a monotonic
// counter.  Never blocks; never returns Identity{}.  Used for boot
// incarnations, attempts, flow groups, sessions and coordinator epochs.
[[nodiscard]] Identity mint_identity() noexcept;

// ---------------------------------------------------------------------------
// Monotonic counters.  These are ordered evidence, not identities: comparing
// two different kinds of counter is never meaningful and no conversion exists.
// ---------------------------------------------------------------------------
enum class SequenceRole : std::uint8_t { kAttempt = 0, kPhase = 1, kStep = 2, kFrame = 3, kPlan = 4 };

class Sequence {
 public:
  constexpr Sequence() noexcept = default;
  constexpr explicit Sequence(std::uint64_t value) noexcept : value_(value) {}
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return value_ == 0; }
  [[nodiscard]] std::string to_string() const;
  friend constexpr bool operator==(Sequence a, Sequence b) noexcept { return a.value_ == b.value_; }
  friend constexpr bool operator!=(Sequence a, Sequence b) noexcept { return !(a == b); }
  friend constexpr bool operator<(Sequence a, Sequence b) noexcept { return a.value_ < b.value_; }
  friend constexpr bool operator>(Sequence a, Sequence b) noexcept { return b < a; }
  friend constexpr bool operator<=(Sequence a, Sequence b) noexcept { return !(b < a); }
  friend constexpr bool operator>=(Sequence a, Sequence b) noexcept { return !(a < b); }
  Sequence& operator++() noexcept {
    ++value_;
    return *this;
  }
  Sequence operator++(int) noexcept {
    Sequence copy(*this);
    ++value_;
    return copy;
  }
 private:
  std::uint64_t value_ = 0;
};

// Domain separation for sequence numbers.  A frame sequence may never be
// compared against a phase sequence; doing so is a programming error and is
// caught in every build configuration.
[[nodiscard]] inline bool same_sequence_role(SequenceRole a, SequenceRole b) noexcept { return a == b; }

// Hash support.  These live in the ctf namespace and are found by ADL, so an
// unordered_map<Identity, ...> works from any namespace without declaring a
// std:: specialisation (which would be ill formed for a user defined type).
[[nodiscard]] inline std::size_t hash_value(const Identity& id) noexcept {
  std::uint64_t value = id.high() ^ (id.low() + 0x9E3779B97F4A7C15ull + (id.high() << 6) + (id.high() >> 2));
  value ^= value >> 33;
  value *= 0xFF51AFD7ED558CCDull;
  value ^= value >> 33;
  return static_cast<std::size_t>(value);
}

[[nodiscard]] inline std::size_t hash_value(const Sequence& sequence) noexcept {
  return static_cast<std::size_t>(sequence.value() * 0x9E3779B97F4A7C15ull);
}

// Hashers for the unordered containers.  They are named types rather than
// std::hash specialisations so that this header is safe to include from inside
// any namespace, and so no standard namespace is extended.
struct IdentityHash {
  [[nodiscard]] std::size_t operator()(const Identity& id) const noexcept { return hash_value(id); }
};

struct IdentityEqual {
  [[nodiscard]] bool operator()(const Identity& a, const Identity& b) const noexcept { return a == b; }
};

struct SequenceHash {
  [[nodiscard]] std::size_t operator()(const Sequence& sequence) const noexcept { return hash_value(sequence); }
};

struct SequenceEqual {
  [[nodiscard]] bool operator()(const Sequence& a, const Sequence& b) const noexcept { return a == b; }
};

// Container aliases.  Every keyed container in this runtime uses these, so a
// single definition decides how identities are hashed and compared.
template <typename Value>
using IdentityMap = std::unordered_map<Identity, Value, IdentityHash, IdentityEqual>;

template <typename Value>
using SequenceMap = std::unordered_map<Sequence, Value, SequenceHash, SequenceEqual>;

using IdentitySet = std::unordered_set<Identity, IdentityHash, IdentityEqual>;
using SequenceSet = std::unordered_set<Sequence, SequenceHash, SequenceEqual>;

}  // namespace ctf

#endif  // CTF_IDENTITY_HPP
