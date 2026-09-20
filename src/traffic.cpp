// Collective Traffic Fabric - traffic class policy and admission control.
// Copyright 2026 Summon Software Labs.
#include "ctf/traffic.hpp"

#include <algorithm>

namespace ctf {
namespace {

std::uint64_t mix64(std::uint64_t state, std::uint64_t value) noexcept {
  state ^= value + 0x9E3779B97F4A7C15ull + (state << 6) + (state >> 2);
  state *= 0xFF51AFD7ED558CCDull;
  state ^= state >> 33;
  return state;
}

}  // namespace

std::string_view to_string(TrafficClass value) noexcept {
  switch (value) {
    case TrafficClass::kUnclassified: return "unclassified";
    case TrafficClass::kLatencyCritical: return "latency_critical";
    case TrafficClass::kControlPlane: return "control_plane";
    case TrafficClass::kBulkData: return "bulk_data";
    case TrafficClass::kBestEffortBulk: return "best_effort_bulk";
    case TrafficClass::kBackground: return "background";
    case TrafficClass::kIsolated: return "isolated";
    case TrafficClass::kProbe: return "probe";
  }
  return "unclassified";
}

bool traffic_class_from_string(std::string_view text, TrafficClass& out) noexcept {
  struct Entry { std::string_view name; TrafficClass value; };
  static constexpr Entry kTable[] = {
      {"unclassified", TrafficClass::kUnclassified},
      {"latency_critical", TrafficClass::kLatencyCritical},
      {"control_plane", TrafficClass::kControlPlane},
      {"bulk_data", TrafficClass::kBulkData},
      {"best_effort_bulk", TrafficClass::kBestEffortBulk},
      {"background", TrafficClass::kBackground},
      {"isolated", TrafficClass::kIsolated},
      {"probe", TrafficClass::kProbe},
  };
  for (const Entry& entry : kTable) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::uint8_t traffic_class_strength(TrafficClass value) noexcept {
  // Total order used by every contention comparison.  Unclassified sits at the
  // bottom: it can never win contention, and nothing can be demoted into it.
  switch (value) {
    case TrafficClass::kUnclassified: return 0;
    case TrafficClass::kBackground: return 1;
    case TrafficClass::kProbe: return 2;
    case TrafficClass::kBestEffortBulk: return 3;
    case TrafficClass::kBulkData: return 4;
    case TrafficClass::kControlPlane: return 5;
    case TrafficClass::kLatencyCritical: return 6;
    case TrafficClass::kIsolated: return 7;
  }
  return 0;
}

bool traffic_class_is_classified(TrafficClass value) noexcept { return value != TrafficClass::kUnclassified; }

std::string_view to_string(IsolationMode value) noexcept {
  switch (value) {
    case IsolationMode::kShared: return "shared";
    case IsolationMode::kClassIsolated: return "class_isolated";
    case IsolationMode::kDedicatedPath: return "dedicated_path";
  }
  return "shared";
}

std::string_view to_string(PacingMode value) noexcept {
  switch (value) {
    case PacingMode::kNone: return "none";
    case PacingMode::kImmediate: return "immediate";
    case PacingMode::kTokenBucket: return "token_bucket";
    case PacingMode::kRateLimited: return "rate_limited";
    case PacingMode::kShaped: return "shaped";
  }
  return "none";
}

std::size_t TrafficClassPolicy::hash() const noexcept {
  std::uint64_t state = 0x243F6A8885A308D3ull;
  state = mix64(state, static_cast<std::uint64_t>(traffic_class));
  state = mix64(state, priority);
  state = mix64(state, minimum_bandwidth_bps);
  state = mix64(state, maximum_bandwidth_bps);
  state = mix64(state, burst_bytes);
  state = mix64(state, static_cast<std::uint64_t>(requires_simultaneous_start));
  state = mix64(state, static_cast<std::uint64_t>(requires_evidence_freshness));
  state = mix64(state, static_cast<std::uint64_t>(isolation));
  return static_cast<std::size_t>(state);
}

bool CongestionResponseTable::is_valid() const noexcept {
  for (std::size_t index = 0; index < kBucketCount; ++index) {
    if (utilization_bps[index] == 0 || utilization_bps[index] > 10000) return false;
    if (rate_numerator_per_mille[index] == 0 || rate_numerator_per_mille[index] > 1000) return false;
    if (index > 0 && utilization_bps[index] <= utilization_bps[index - 1]) return false;
  }
  return true;
}

const TrafficClassPolicy* TrafficPolicy::find_class(TrafficClass value) const noexcept {
  const auto raw = static_cast<std::uint8_t>(value);
  for (const TrafficClassPolicy& policy : class_policies) {
    if (static_cast<std::uint8_t>(policy.traffic_class) == raw) return &policy;
  }
  return nullptr;
}

const CollectiveClassPolicy* TrafficPolicy::find_collective(CollectiveClass value) const noexcept {
  const auto raw = static_cast<std::uint8_t>(value);
  for (const CollectiveClassPolicy& policy : collective_policies) {
    if (static_cast<std::uint8_t>(policy.collective_class) == raw) return &policy;
  }
  return nullptr;
}

Status TrafficPolicy::validate() const {
  if (generation.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "traffic policy has no generation");
  }
  if (class_policies.empty()) {
    return Status(ErrorCode::kValidationPolicyMissing, "traffic policy has no class policies");
  }
  if (class_policies.size() > static_cast<std::size_t>(TrafficClass::kProbe) + 1) {
    return Status(ErrorCode::kValidationPolicyMissing, "traffic policy has too many class policies");
  }
  bool seen_class[16] = {};
  for (const TrafficClassPolicy& policy : class_policies) {
    const auto raw = static_cast<std::uint8_t>(policy.traffic_class);
    if (raw >= 16) return Status(ErrorCode::kValidationClassUnknownRejected, "unknown traffic class in policy");
    if (seen_class[raw]) {
      return Status(ErrorCode::kDecodeDuplicateEntry, "duplicate traffic class policy");
    }
    seen_class[raw] = true;
    if (policy.priority > kMaximumPriority) {
      return Status(ErrorCode::kValidationRateInvalid, "traffic class priority out of range");
    }
    if (policy.maximum_bandwidth_bps != kUnlimitedRate && policy.minimum_bandwidth_bps > policy.maximum_bandwidth_bps) {
      return Status(ErrorCode::kValidationRateInvalid, "traffic class floor exceeds its ceiling");
    }
    if (policy.burst_bytes > (1ull << 62)) {
      return Status(ErrorCode::kValidationRateInvalid, "traffic class burst is absurd");
    }
  }
  if (find_class(TrafficClass::kUnclassified) == nullptr) {
    return Status(ErrorCode::kValidationPolicyMissing, "policy must state the unclassified class explicitly");
  }
  if (find_collective(CollectiveClass::kUnknown) == nullptr) {
    return Status(ErrorCode::kValidationPolicyMissing, "policy must state the unknown collective class explicitly");
  }
  if (!congestion_response.is_valid()) {
    return Status(ErrorCode::kValidationRateInvalid, "congestion response table is not monotonic");
  }
  if (burst_multiplier_per_mille > 1000) {
    return Status(ErrorCode::kValidationRateInvalid, "burst multiplier exceeds the whole rate");
  }
  if (collective_policies.size() > 16) {
    return Status(ErrorCode::kValidationPolicyMissing, "traffic policy has too many collective policies");
  }
  return Status::ok();
}

std::size_t TrafficPolicy::hash() const noexcept {
  std::uint64_t state = 0x13198A2E03707344ull;
  for (const TrafficClassPolicy& policy : class_policies) {
    state = mix64(state, static_cast<std::uint64_t>(policy.hash()));
  }
  for (const CollectiveClassPolicy& policy : collective_policies) {
    state = mix64(state, static_cast<std::uint64_t>(policy.collective_class));
    state = mix64(state, static_cast<std::uint64_t>(policy.traffic_class));
    state = mix64(state, static_cast<std::uint64_t>(policy.admit_when_unknown_semantics));
    state = mix64(state, static_cast<std::uint64_t>(policy.allow_member_override));
    state = mix64(state, policy.freshness_requirement_ms);
  }
  state = mix64(state, default_maximum_bandwidth_bps);
  state = mix64(state, burst_multiplier_per_mille);
  return static_cast<std::size_t>(state);
}

TrafficPolicy TrafficPolicy::standard(PolicyGeneration generation) {
  TrafficPolicy policy;
  policy.generation = generation;

  auto add_class = [&policy](TrafficClass traffic_class, std::uint64_t priority,
                             std::uint64_t minimum_bandwidth_bps, std::uint64_t maximum_bandwidth_bps,
                             bool simultaneous, bool requires_freshness, IsolationMode isolation) {
    TrafficClassPolicy entry;
    entry.traffic_class = traffic_class;
    entry.priority = priority;
    entry.minimum_bandwidth_bps = minimum_bandwidth_bps;
    entry.maximum_bandwidth_bps = maximum_bandwidth_bps;
    entry.burst_bytes = 0;  // derived from the rate when a decision is issued
    entry.requires_simultaneous_start = simultaneous;
    entry.requires_evidence_freshness = requires_freshness;
    entry.isolation = isolation;
    policy.class_policies.push_back(entry);
  };

  // The documented default table.  Floors are guarantees the fabric must honor;
  // ceilings are limits the runtime will not exceed.  0 floors mean "no floor
  // claimed" and are never interpreted as "no bandwidth".
  add_class(TrafficClass::kUnclassified, 0, 0, 0, false, true, IsolationMode::kShared);
  add_class(TrafficClass::kLatencyCritical, 15, 100ull * 1000 * 1000, 40ull * 1000 * 1000 * 1000ull, true, true,
            IsolationMode::kClassIsolated);
  add_class(TrafficClass::kControlPlane, 14, 10ull * 1000 * 1000, 10ull * 1000 * 1000 * 1000ull, false, false,
            IsolationMode::kClassIsolated);
  add_class(TrafficClass::kBulkData, 8, 1ull * 1000 * 1000 * 1000, kUnlimitedRate, false, true,
            IsolationMode::kShared);
  add_class(TrafficClass::kBestEffortBulk, 5, 0, 50ull * 1000 * 1000 * 1000ull, false, true, IsolationMode::kShared);
  add_class(TrafficClass::kBackground, 1, 0, 10ull * 1000 * 1000 * 1000ull, false, false, IsolationMode::kShared);
  add_class(TrafficClass::kIsolated, 15, 1ull * 1000 * 1000 * 1000, kUnlimitedRate, false, true,
            IsolationMode::kDedicatedPath);
  add_class(TrafficClass::kProbe, 3, 0, 100ull * 1000 * 1000, false, false, IsolationMode::kShared);

  auto add_collective = [&policy](CollectiveClass collective_class, TrafficClass traffic_class,
                                  bool admit_unknown, bool allow_override, std::uint64_t freshness_ms) {
    CollectiveClassPolicy entry;
    entry.collective_class = collective_class;
    entry.traffic_class = traffic_class;
    entry.admit_when_unknown_semantics = admit_unknown;
    entry.allow_member_override = allow_override;
    entry.freshness_requirement_ms = freshness_ms;
    policy.collective_policies.push_back(entry);
  };

  add_collective(CollectiveClass::kUnknown, TrafficClass::kUnclassified, false, false, 0);
  add_collective(CollectiveClass::kUnknownVendor, TrafficClass::kUnclassified, false, false, 0);
  add_collective(CollectiveClass::kAllReduce, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kAllGather, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kReduceScatter, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kBroadcast, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kGather, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kScatter, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kAllToAll, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kPointToPoint, TrafficClass::kBestEffortBulk, true, true, 2000);
  add_collective(CollectiveClass::kBarrierOnly, TrafficClass::kLatencyCritical, true, false, 500);
  add_collective(CollectiveClass::kSendRecvGrouped, TrafficClass::kBestEffortBulk, true, true, 2000);
  add_collective(CollectiveClass::kReduce, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kScan, TrafficClass::kBulkData, true, true, 2000);
  add_collective(CollectiveClass::kSendRecvSplit, TrafficClass::kBestEffortBulk, true, true, 2000);
  return policy;
}

Status TokenBucket::refill(TokenBucketState& state, std::uint64_t clock_now_ms, std::uint64_t rate_bps,
                           std::uint64_t burst_bytes) {
  if (rate_bps == kUnlimitedRate) {
    state.rate_bps = kUnlimitedRate;
    state.burst_bytes = burst_bytes;
    state.tokens = burst_bytes;
    state.last_refill_monotonic_ms = clock_now_ms;
    state.initialized = true;
    return Status::ok();
  }
  if (rate_bps == 0) {
    return Status(ErrorCode::kAdmissionBucketUnset, "token bucket has no rate");
  }
  if (!state.initialized) {
    state.tokens = burst_bytes;
    state.last_refill_monotonic_ms = clock_now_ms;
    state.rate_bps = rate_bps;
    state.burst_bytes = burst_bytes;
    state.initialized = true;
    return Status::ok();
  }
  if (clock_now_ms < state.last_refill_monotonic_ms) {
    return Status(ErrorCode::kAdmissionClockRegression,
                  "monotonic clock moved backwards; refusing to grant tokens");
  }
  const std::uint64_t elapsed_ms = clock_now_ms - state.last_refill_monotonic_ms;
  state.rate_bps = rate_bps;
  state.burst_bytes = burst_bytes;
  if (elapsed_ms != 0) {
    // tokens = min(burst, tokens + rate * elapsed / 1000).  The multiplication
    // is done in 64 bit halves so the computation is overflow free for every
    // legal rate without depending on a 128 bit extension.
    const std::uint64_t quotient = rate_bps / 1000u;
    const std::uint64_t remainder = rate_bps % 1000u;
    std::uint64_t gained = quotient * elapsed_ms;
    if (gained / elapsed_ms != quotient) {
      gained = burst_bytes;  // saturate: the elapsed interval refills the bucket
    } else {
      gained += (remainder * elapsed_ms) / 1000u;
      if (gained < quotient * elapsed_ms) gained = burst_bytes;
    }
    const std::uint64_t capped = gained > burst_bytes ? burst_bytes : gained;
    const std::uint64_t headroom = state.tokens >= burst_bytes ? 0 : burst_bytes - state.tokens;
    state.tokens += capped > headroom ? headroom : capped;
    state.last_refill_monotonic_ms = clock_now_ms;
  }
  if (state.tokens > burst_bytes) state.tokens = burst_bytes;
  return Status::ok();
}

Status TokenBucket::try_consume(TokenBucketState& state, std::uint64_t clock_now_ms, std::uint64_t bytes,
                                std::uint64_t rate_bps, std::uint64_t burst_bytes) {
  Status status = refill(state, clock_now_ms, rate_bps, burst_bytes);
  if (!status.is_ok()) return status;
  if (state.rate_bps == kUnlimitedRate) {
    return Status::ok();
  }
  if (bytes <= state.tokens) {
    state.tokens -= bytes;
    return Status::ok();
  }
  return Status(ErrorCode::kAdmissionRateLimited, "token bucket is empty for this request");
}

std::uint64_t TokenBucket::delay_until_available_ms(const TokenBucketState& state, std::uint64_t bytes) {
  if (state.rate_bps == kUnlimitedRate) return 0;
  if (state.rate_bps == 0) return 0;
  if (bytes <= state.tokens) return 0;
  const std::uint64_t deficit = bytes - state.tokens;
  // Ceiling division; a zero result is promoted to 1ms so callers never spin.
  const std::uint64_t milliseconds =
      (deficit * 1000ull + state.rate_bps - 1ull) / state.rate_bps;
  return milliseconds == 0 ? 1 : milliseconds;
}

}  // namespace ctf
