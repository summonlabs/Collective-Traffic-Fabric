// Collective Traffic Fabric - admission control and pacing enforcement.
// Copyright 2026 Summon Software Labs.
//
// Admission is a separate authority from permission.  A decision that is
// ADMITTED says "this treatment is legal for this generation set"; admission
// says "this traffic may be released at this instant".  Keeping them separate
// is what makes RATE_LIMITED a timing answer rather than a permission answer.
#ifndef CTF_ADMISSION_HPP
#define CTF_ADMISSION_HPP

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "ctf/decision.hpp"
#include "ctf/error.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"
#include "ctf/traffic.hpp"

namespace ctf {

enum class AdmissionOutcome : std::uint8_t {
  kGranted = 0,
  kRateLimited = 1,
  kClockRegression = 2,
  kNotApplicable = 3,   // the decision grants no authority, so nothing is released
  kBucketTableFull = 4,
};

[[nodiscard]] std::string_view to_string(AdmissionOutcome value) noexcept;

struct AdmissionResult {
  AdmissionOutcome outcome = AdmissionOutcome::kNotApplicable;
  std::uint64_t wait_ms = 0;
  ErrorCode code = ErrorCode::kOk;
  TokenBucketState bucket{};
  ReasonChain reasons{};
};

// Bounded table of token buckets keyed by flow group.  The table never grows
// past kTokenBucketsMax: when it is full, unknown keys are refused with a
// deterministic error rather than evicting an entry that another flow group is
// still relying on.
class AdmissionController {
 public:
  explicit AdmissionController(std::uint32_t capacity = limits::kTokenBucketsMax);

  // Applies admission control to a decision.  Returns kNotApplicable when the
  // decision carries no authority (nothing may be released by any path).
  AdmissionResult admit(const DecisionRecord& decision, std::uint64_t now_monotonic_ms);

  // Observes a decision without consuming tokens.  Used by inspection.
  [[nodiscard]] AdmissionResult peek(const DecisionRecord& decision, std::uint64_t now_monotonic_ms) const;

  void forget(FlowGroupId group);
  void clear();
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

 private:
  struct Entry {
    TokenBucketState bucket{};
    std::uint64_t last_used_monotonic_ms = 0;
  };

  mutable std::mutex mutex_;
  IdentityMap<Entry> buckets_;
  std::uint32_t capacity_;
};

}  // namespace ctf

#endif  // CTF_ADMISSION_HPP
