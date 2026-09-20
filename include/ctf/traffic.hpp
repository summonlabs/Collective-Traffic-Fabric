// Collective Traffic Fabric - collective aware traffic classes, priority and
// isolation policy, pacing intents.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_TRAFFIC_HPP
#define CTF_TRAFFIC_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cstddef>

#include "ctf/collective.hpp"
#include "ctf/error.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"

namespace ctf {

// Network level treatment classes understood by this runtime.  These describe
// the *network* obligation a collective places on the fabric; they deliberately
// do not name a scheduler implementation.
enum class TrafficClass : std::uint8_t {
  kUnclassified = 0,      // no treatment may be granted
  kLatencyCritical = 1,   // barrier-like, small, delay sensitive
  kControlPlane = 2,      // bootstrap, handshake, coordinator traffic
  kBulkData = 3,          // high volume, throughput sensitive
  kBestEffortBulk = 4,    // bulk that may yield
  kBackground = 5,        // lowest, yields to everything
  kIsolated = 6,          // isolation mandated by policy; not multiplexed
  kProbe = 7,             // tiny diagnostic traffic
};

[[nodiscard]] std::string_view to_string(TrafficClass value) noexcept;
[[nodiscard]] bool traffic_class_from_string(std::string_view text, TrafficClass& out) noexcept;
// Stronger means "wins contention and is admitted earlier".  The ordering is a
// total order used by every comparison in the policy engine.
[[nodiscard]] std::uint8_t traffic_class_strength(TrafficClass value) noexcept;
[[nodiscard]] bool traffic_class_is_classified(TrafficClass value) noexcept;

enum class IsolationMode : std::uint8_t {
  kShared = 0,            // multiplexed with peers of the same class
  kClassIsolated = 1,     // separated from other traffic classes
  kDedicatedPath = 2,     // isolated to its own links; refuses sharing entirely
};

[[nodiscard]] std::string_view to_string(IsolationMode value) noexcept;

// All rates are in logical bytes per second and stored as integers so that the
// engine is exactly reproducible.  0 means "unspecified" and never means
// "unlimited": unlimited must be stated explicitly with kUnlimitedRate.
inline constexpr std::uint64_t kUnlimitedRate = 0xFFFFFFFFFFFFFFFFull;
inline constexpr std::uint64_t kMinimumPriority = 0;
inline constexpr std::uint64_t kMaximumPriority = 15;

struct TrafficClassPolicy {
  TrafficClass traffic_class = TrafficClass::kUnclassified;
  std::uint64_t priority = 0;                 // 0 (lowest) .. 15 (highest)
  std::uint64_t minimum_bandwidth_bps = 0;    // floor the fabric must honor
  std::uint64_t maximum_bandwidth_bps = kUnlimitedRate;
  std::uint64_t burst_bytes = 0;              // token bucket depth
  bool requires_simultaneous_start = false;
  bool requires_evidence_freshness = true;    // congestion evidence mandatory
  IsolationMode isolation = IsolationMode::kShared;

  [[nodiscard]] std::size_t hash() const noexcept;
};

// Congestion response is a deterministic integer table, not a guess.
struct CongestionResponseTable {
  // Bucket boundaries on observed utilization in basis points
  // (10_000 = 100%).  Exactly kCongestionBucketCount entries, ascending.
  static constexpr std::size_t kBucketCount = 5;
  std::uint32_t utilization_bps[kBucketCount] = {6000, 7500, 8500, 9500, 9900};
  // Rate numerator per bucket, applied over denominator 1000.
  // Index 0 is used below the first boundary, index 4 at or above the last.
  std::uint32_t rate_numerator_per_mille[kBucketCount] = {1000, 800, 600, 400, 150};

  [[nodiscard]] bool is_valid() const noexcept;
};

struct CollectiveClassPolicy {
  CollectiveClass collective_class = CollectiveClass::kUnknown;
  TrafficClass traffic_class = TrafficClass::kUnclassified;
  bool admit_when_unknown_semantics = false;  // must be false to keep UNKNOWN weak
  bool allow_member_override = false;         // can a peer request another class
  std::uint64_t freshness_requirement_ms = 2000;  // congestion evidence age bound
};

// The complete, generation stamped policy set.  Every traffic decision binds
// the policy generation that made it legal.
struct TrafficPolicy {
  PolicyGeneration generation{};
  std::vector<TrafficClassPolicy> class_policies;
  std::vector<CollectiveClassPolicy> collective_policies;
  CongestionResponseTable congestion_response{};
  std::uint64_t default_maximum_bandwidth_bps = kUnlimitedRate;
  std::uint64_t burst_multiplier_per_mille = 250;  // burst = 25% of rate by default

  [[nodiscard]] const TrafficClassPolicy* find_class(TrafficClass value) const noexcept;
  [[nodiscard]] const CollectiveClassPolicy* find_collective(CollectiveClass value) const noexcept;
  [[nodiscard]] Status validate() const;
  [[nodiscard]] std::size_t hash() const noexcept;

  // The production policy: deterministic, documented, and the basis of the
  // documented default classification table.
  [[nodiscard]] static TrafficPolicy standard(PolicyGeneration generation);
};

enum class PacingMode : std::uint8_t {
  kNone = 0,
  kImmediate = 1,
  kTokenBucket = 2,
  kRateLimited = 3,
  kShaped = 4,
};

[[nodiscard]] std::string_view to_string(PacingMode value) noexcept;

// A pacing intent is advice bound to one flow group and one generation set.
// It is not a scheduler command and it carries no authority of its own.
struct PacingIntent {
  PacingMode mode = PacingMode::kNone;
  FlowGroupId flow_group{};
  CollectiveInstance instance{};
  std::uint64_t admitted_bandwidth_bps = 0;
  std::uint64_t ceiling_bandwidth_bps = 0;
  std::uint64_t burst_bytes = 0;
  std::uint64_t utilization_bps = 0;     // evidence used
  std::uint64_t release_after_monotonic_ms = 0;  // 0 -> release now
  Identity reason_code{};                // unset; textual reason lives in the chain

  friend bool operator==(const PacingIntent& a, const PacingIntent& b) {
    return a.mode == b.mode && a.flow_group == b.flow_group && a.instance == b.instance &&
           a.admitted_bandwidth_bps == b.admitted_bandwidth_bps &&
           a.ceiling_bandwidth_bps == b.ceiling_bandwidth_bps && a.burst_bytes == b.burst_bytes &&
           a.utilization_bps == b.utilization_bps &&
           a.release_after_monotonic_ms == b.release_after_monotonic_ms;
  }
};

// Deterministic token bucket admission control.  The clock is always supplied
// by the caller; this type never reads wall time, so it is exactly testable.
struct TokenBucketState {
  std::uint64_t tokens = 0;
  std::uint64_t last_refill_monotonic_ms = 0;
  std::uint64_t rate_bps = 0;
  std::uint64_t burst_bytes = 0;
  bool initialized = false;
};

class TokenBucket {
 public:
  // Refills from the supplied monotonic clock and attempts to consume tokens.
  // A clock that moves backwards is rejected with kAdmissionClockRegression
  // rather than silently granting tokens.
  [[nodiscard]] static Status try_consume(TokenBucketState& state, std::uint64_t clock_now_ms,
                                          std::uint64_t bytes, std::uint64_t rate_bps,
                                          std::uint64_t burst_bytes);
  [[nodiscard]] static Status refill(TokenBucketState& state, std::uint64_t clock_now_ms,
                                     std::uint64_t rate_bps, std::uint64_t burst_bytes);
  // Milliseconds until enough tokens exist for bytes.  0 means now.
  [[nodiscard]] static std::uint64_t delay_until_available_ms(const TokenBucketState& state,
                                                             std::uint64_t bytes);
};

}  // namespace ctf

#endif  // CTF_TRAFFIC_HPP
