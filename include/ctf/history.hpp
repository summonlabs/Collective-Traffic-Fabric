// Collective Traffic Fabric - bounded decision history with stable cursors.
// Copyright 2026 Summon Software Labs.
//
// History is bounded by construction: a ring buffer with a fixed capacity, so
// no sequence of externally driven operations can grow it without limit.
// Cursors are monotonic insertion ordinals; evicted entries return
// kHistoryEvicted rather than an empty result that could be mistaken for
// "no decision was ever made".
#ifndef CTF_HISTORY_HPP
#define CTF_HISTORY_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ctf/decision.hpp"
#include "ctf/error.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"

namespace ctf {

enum class HistoryStatus : std::uint8_t {
  kOk = 0,
  kEvicted = 1,   // the requested ordinal has been dropped from the ring
  kFuture = 2,    // the ordinal has not been inserted yet
  kEmpty = 3,
};

[[nodiscard]] std::string_view to_string(HistoryStatus value) noexcept;

// Thread safe, allocation bounded (capacity x sizeof(record)), no callbacks.
class DecisionHistory {
 public:
  explicit DecisionHistory(std::size_t capacity = limits::kDecisionHistoryDefaultCapacity);

  void set_capacity(std::size_t capacity);
  [[nodiscard]] std::size_t capacity() const;
  [[nodiscard]] std::size_t size() const;

  // Appends a decision and returns its ordinal.
  std::uint64_t record(const DecisionRecord& decision);

  // Most recent decisions, newest first.
  [[nodiscard]] std::vector<DecisionRecord> latest(std::size_t limit) const;
  // Stable page by insertion ordinal.
  [[nodiscard]] std::vector<DecisionRecord> page(std::uint64_t offset_ordinal, std::size_t limit) const;
  [[nodiscard]] HistoryStatus get(std::uint64_t ordinal, DecisionRecord& out) const;

  // The newest decision whose bound instance matches exactly.  This is the
  // query an operator uses to ask "what authority was issued for attempt A of
  // collective C at generation G".
  [[nodiscard]] HistoryStatus find_instance(const CollectiveInstance& instance, DecisionRecord& out) const;
  // Newest retained decision for a collective identity and attempt, ignoring
  // the generation.  Used by explain(), where the caller knows which attempt it
  // is asking about but not which generation was in force at the time.
  [[nodiscard]] HistoryStatus find_attempt(CollectiveId id, CollectiveAttemptId attempt,
                                           DecisionRecord& out) const;
  // Associates a caller supplied correlation value with the ordinal of a
  // recorded decision.  Bounded by capacity_ so it can never grow without
  // limit, and cleared whenever the retained window is cleared.
  void bind_correlation(std::uint64_t correlation, std::uint64_t ordinal);

  [[nodiscard]] std::uint64_t first_ordinal() const;
  [[nodiscard]] std::uint64_t next_ordinal() const;
  [[nodiscard]] std::uint64_t evicted_count() const;
  [[nodiscard]] HistoryStatus find_correlation(std::uint64_t correlation, DecisionRecord& out) const;

  // Counts by outcome over the retained window.  Deterministic and bounded.
  [[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> outcome_histogram() const;

  void clear();

 private:
  mutable std::mutex mutex_;
  std::size_t capacity_;
  std::deque<DecisionRecord> records_;
  IdentityMap<std::uint64_t> correlations_;
  std::uint64_t first_ordinal_ = 1;
  std::uint64_t next_ordinal_ = 1;
  std::uint64_t evicted_ = 0;
};

}  // namespace ctf

#endif  // CTF_HISTORY_HPP
