// Collective Traffic Fabric - bounded decision history.
// Copyright 2026 Summon Software Labs.
#include "ctf/history.hpp"

#include <algorithm>

namespace ctf {

std::string_view to_string(HistoryStatus value) noexcept {
  switch (value) {
    case HistoryStatus::kOk: return "ok";
    case HistoryStatus::kEvicted: return "evicted";
    case HistoryStatus::kFuture: return "future";
    case HistoryStatus::kEmpty: return "empty";
  }
  return "empty";
}

DecisionHistory::DecisionHistory(std::size_t capacity) {
  set_capacity(capacity);
}

void DecisionHistory::set_capacity(std::size_t capacity) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (capacity == 0) capacity = 1;
  if (capacity > limits::kDecisionHistoryCapacityMax) capacity = limits::kDecisionHistoryCapacityMax;
  capacity_ = capacity;
  while (records_.size() > capacity_) {
    records_.pop_front();
    ++first_ordinal_;
    ++evicted_;
  }
}

std::size_t DecisionHistory::capacity() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return capacity_;
}

std::size_t DecisionHistory::size() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return records_.size();
}

std::uint64_t DecisionHistory::record(const DecisionRecord& decision) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::uint64_t ordinal = next_ordinal_;
  ++next_ordinal_;
  DecisionRecord stored = decision;
  stored.decision_sequence = Sequence(ordinal);
  records_.push_back(std::move(stored));
  while (records_.size() > capacity_) {
    records_.pop_front();
    ++first_ordinal_;
    ++evicted_;
  }
  return ordinal;
}

std::vector<DecisionRecord> DecisionHistory::latest(std::size_t limit) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<DecisionRecord> out;
  if (limit > records_.size()) limit = records_.size();
  out.reserve(limit);
  for (std::size_t index = 0; index < limit; ++index) {
    out.push_back(records_[records_.size() - 1 - index]);
  }
  return out;
}

std::vector<DecisionRecord> DecisionHistory::page(std::uint64_t offset_ordinal, std::size_t limit) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<DecisionRecord> out;
  if (records_.empty() || offset_ordinal >= next_ordinal_) return out;
  std::uint64_t start = offset_ordinal < first_ordinal_ ? first_ordinal_ : offset_ordinal;
  std::size_t index = static_cast<std::size_t>(start - first_ordinal_);
  for (; index < records_.size() && out.size() < limit; ++index) {
    out.push_back(records_[index]);
  }
  return out;
}

HistoryStatus DecisionHistory::get(std::uint64_t ordinal, DecisionRecord& out) const {
  std::lock_guard<std::mutex> guard(mutex_);
  if (records_.empty()) return HistoryStatus::kEmpty;
  if (ordinal >= next_ordinal_) return HistoryStatus::kFuture;
  if (ordinal < first_ordinal_) return HistoryStatus::kEvicted;
  out = records_[static_cast<std::size_t>(ordinal - first_ordinal_)];
  return HistoryStatus::kOk;
}

HistoryStatus DecisionHistory::find_instance(const CollectiveInstance& instance, DecisionRecord& out) const {
  std::lock_guard<std::mutex> guard(mutex_);
  if (records_.empty()) return HistoryStatus::kEmpty;
  for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
    if (it->instance == instance) {
      out = *it;
      return HistoryStatus::kOk;
    }
  }
  return HistoryStatus::kEvicted;
}

HistoryStatus DecisionHistory::find_attempt(CollectiveId id, CollectiveAttemptId attempt,
                                              DecisionRecord& out) const {
  std::lock_guard<std::mutex> guard(mutex_);
  if (records_.empty()) return HistoryStatus::kEmpty;
  for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
    const bool id_matches = it->instance.id == id;
    const bool attempt_matches = attempt.is_none() ? true : it->instance.attempt == attempt;
    if (id_matches && attempt_matches) {
      out = *it;
      return HistoryStatus::kOk;
    }
  }
  return HistoryStatus::kEvicted;
}

void DecisionHistory::bind_correlation(std::uint64_t correlation, std::uint64_t ordinal) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (correlation == 0) return;
  if (correlations_.size() >= capacity_) {
    correlations_.clear();
  }
  correlations_[Identity(correlation, 0)] = ordinal;
}

HistoryStatus DecisionHistory::find_correlation(std::uint64_t correlation, DecisionRecord& out) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto existing = correlations_.find(Identity(correlation, 0));
  if (existing == correlations_.end()) return HistoryStatus::kEvicted;
  const std::uint64_t ordinal = existing->second;
  if (ordinal >= next_ordinal_) return HistoryStatus::kFuture;
  if (ordinal < first_ordinal_) return HistoryStatus::kEvicted;
  out = records_[static_cast<std::size_t>(ordinal - first_ordinal_)];
  return HistoryStatus::kOk;
}

std::uint64_t DecisionHistory::first_ordinal() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return first_ordinal_;
}

std::uint64_t DecisionHistory::next_ordinal() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return next_ordinal_;
}

std::uint64_t DecisionHistory::evicted_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evicted_;
}

std::vector<std::pair<std::string, std::uint64_t>> DecisionHistory::outcome_histogram() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<std::pair<std::string, std::uint64_t>> counts;
  for (const DecisionRecord& record : records_) {
    const std::string name(to_string(record.outcome));
    bool found = false;
    for (auto& entry : counts) {
      if (entry.first == name) {
        ++entry.second;
        found = true;
        break;
      }
    }
    if (!found) counts.emplace_back(name, 1);
  }
  std::sort(counts.begin(), counts.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return counts;
}

void DecisionHistory::clear() {
  std::lock_guard<std::mutex> guard(mutex_);
  records_.clear();
  correlations_.clear();
  first_ordinal_ = 1;
  next_ordinal_ = 1;
  evicted_ = 0;
}

}  // namespace ctf
