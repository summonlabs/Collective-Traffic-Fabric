// Collective Traffic Fabric - admission control implementation.
// Copyright 2026 Summon Software Labs.
#include "ctf/admission.hpp"

#include <algorithm>

namespace ctf {

std::string_view to_string(AdmissionOutcome value) noexcept {
  switch (value) {
    case AdmissionOutcome::kGranted: return "granted";
    case AdmissionOutcome::kRateLimited: return "rate_limited";
    case AdmissionOutcome::kClockRegression: return "clock_regression";
    case AdmissionOutcome::kNotApplicable: return "not_applicable";
    case AdmissionOutcome::kBucketTableFull: return "bucket_table_full";
  }
  return "not_applicable";
}

AdmissionController::AdmissionController(std::uint32_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

AdmissionResult AdmissionController::admit(const DecisionRecord& decision, std::uint64_t now_monotonic_ms) {
  AdmissionResult result;
  if (!outcome_grants_authority(decision.outcome) || !decision.flow_group_constructed) {
    result.outcome = AdmissionOutcome::kNotApplicable;
    result.reasons.add("admission_not_applicable",
                       "the decision grants no authority, so no traffic may be released for it",
                       decision.instance.id);
    return result;
  }
  if (decision.pacing.mode == PacingMode::kNone || decision.pacing.mode == PacingMode::kImmediate) {
    result.outcome = AdmissionOutcome::kGranted;
    result.reasons.add("admission_immediate", "the pacing mode releases this flow group immediately",
                       decision.flow_group);
    return result;
  }
  if (decision.pacing.admitted_bandwidth_bps == kUnlimitedRate) {
    result.outcome = AdmissionOutcome::kGranted;
    result.reasons.add("admission_unlimited",
                       "the class ceiling is explicitly unlimited, so no token bucket applies",
                       decision.flow_group);
    return result;
  }
  if (decision.pacing.admitted_bandwidth_bps == 0) {
    result.outcome = AdmissionOutcome::kRateLimited;
    result.code = ErrorCode::kAdmissionRateLimited;
    result.wait_ms = 0;
    result.reasons.add("admission_zero_rate",
                       "the admitted rate is zero; no traffic may be released until evidence changes",
                       decision.flow_group);
    return result;
  }

  const std::uint64_t bytes = decision.group.total_logical_bytes;
  const std::uint64_t burst = decision.pacing.burst_bytes;

  std::lock_guard<std::mutex> guard(mutex_);
  auto existing = buckets_.find(decision.flow_group);
  if (existing == buckets_.end()) {
    if (buckets_.size() >= capacity_) {
      result.outcome = AdmissionOutcome::kBucketTableFull;
      result.code = ErrorCode::kAdmissionBucketCapacity;
      result.reasons.add("admission_bucket_table_full",
                         "the admission table is full; no bucket can be created for this flow group",
                         decision.flow_group);
      return result;
    }
    existing = buckets_.emplace(decision.flow_group, Entry{}).first;
  }
  Entry& entry = existing->second;
  entry.last_used_monotonic_ms = now_monotonic_ms;

  const std::uint64_t request_bytes = bytes == 0 ? 1 : bytes;
  const std::uint64_t rate = decision.pacing.admitted_bandwidth_bps;
  // The effective bucket depth is the policy's burst ceiling, but never smaller
  // than a single request: a request that cannot fit in any bucket would be
  // refused forever, which would make a legal decision permanently unusable.
  // The consequence is stated explicitly: the first release of an oversized
  // request is admitted and sets the bucket depth, and every later release of
  // the same size is paced by the refill rate.
  const std::uint64_t effective_burst = std::max(burst, request_bytes);

  Status status = TokenBucket::try_consume(entry.bucket, now_monotonic_ms, request_bytes, rate, effective_burst);
  result.bucket = entry.bucket;
  if (status.is_ok()) {
    result.outcome = AdmissionOutcome::kGranted;
    result.reasons.add("admission_granted",
                       request_bytes > burst
                           ? "the requested volume exceeds the class burst ceiling, so the bucket depth was "
                             "raised to one request and this first release was admitted"
                           : "token bucket released the requested volume at the admitted rate",
                       decision.flow_group);
    return result;
  }
  if (status.code() == ErrorCode::kAdmissionClockRegression) {
    result.outcome = AdmissionOutcome::kClockRegression;
    result.code = status.code();
    result.reasons.add("admission_clock_regression",
                       "the supplied monotonic clock moved backwards; tokens were not granted",
                       decision.flow_group);
    return result;
  }
  result.outcome = AdmissionOutcome::kRateLimited;
  result.code = ErrorCode::kAdmissionRateLimited;
  result.wait_ms = TokenBucket::delay_until_available_ms(entry.bucket, request_bytes);
  result.reasons.add("admission_rate_limited",
                     "the token bucket for this flow group is empty; the flow group is deferred, not denied",
                     decision.flow_group);
  return result;
}

AdmissionResult AdmissionController::peek(const DecisionRecord& decision, std::uint64_t now_monotonic_ms) const {
  AdmissionResult result;
  if (!outcome_grants_authority(decision.outcome) || !decision.flow_group_constructed) {
    result.outcome = AdmissionOutcome::kNotApplicable;
    return result;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  auto existing = buckets_.find(decision.flow_group);
  if (existing == buckets_.end()) {
    result.outcome = AdmissionOutcome::kGranted;
    result.reasons.add("admission_bucket_absent",
                       "no bucket exists yet; the first request initializes it with a full allowance",
                       decision.flow_group);
    return result;
  }
  TokenBucketState copy = existing->second.bucket;
  Status status = TokenBucket::refill(copy, now_monotonic_ms, decision.pacing.admitted_bandwidth_bps,
                                      decision.pacing.burst_bytes == 0 ? 1 : decision.pacing.burst_bytes);
  result.bucket = copy;
  if (!status.is_ok()) {
    result.outcome = AdmissionOutcome::kClockRegression;
    result.code = status.code();
    return result;
  }
  const std::uint64_t bytes = decision.group.total_logical_bytes == 0 ? 1 : decision.group.total_logical_bytes;
  if (bytes <= copy.tokens) {
    result.outcome = AdmissionOutcome::kGranted;
    return result;
  }
  result.outcome = AdmissionOutcome::kRateLimited;
  result.code = ErrorCode::kAdmissionRateLimited;
  result.wait_ms = TokenBucket::delay_until_available_ms(copy, bytes);
  return result;
}

void AdmissionController::forget(FlowGroupId group) {
  std::lock_guard<std::mutex> guard(mutex_);
  buckets_.erase(group);
}

void AdmissionController::clear() {
  std::lock_guard<std::mutex> guard(mutex_);
  buckets_.clear();
}

std::size_t AdmissionController::size() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return buckets_.size();
}

}  // namespace ctf
