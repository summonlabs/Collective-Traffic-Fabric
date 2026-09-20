// Collective Traffic Fabric - durable collective catalog.
// Copyright 2026 Summon Software Labs.
//
// The catalog owns durable definitions and their generation history.  It holds
// NO dynamic state: no liveness, no congestion, no capacity, no currentness.
// Persistence of the catalog therefore cannot resurrect anything dynamic.
#ifndef CTF_CATALOG_HPP
#define CTF_CATALOG_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ctf/collective.hpp"
#include "ctf/error.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"

namespace ctf {

enum class RegistrationResult : std::uint8_t {
  kRegistered = 0,        // first registration of a collective id
  kGenerationAdvanced = 1, // definition changed; a new generation was minted
  kUnchanged = 2,         // identical definition re-registered; no new generation
  kRejectedInvalid = 3,
  kRejectedTerminal = 4,
  kRejectedCapacity = 5,
};

[[nodiscard]] std::string_view to_string(RegistrationResult value) noexcept;

// A definition plus the durable bookkeeping that must survive a restart.
struct CollectiveRecord {
  CollectiveDefinition definition{};
  CollectiveState state = CollectiveState::kUnregistered;
  CollectiveAttemptId current_attempt{};
  // Volume declared for the current attempt.  0 means "not declared", and is
  // never interpreted as "no traffic".
  std::uint64_t attempt_transfer_bytes = 0;
  Sequence attempt_sequence{};
  Sequence registration_sequence{};
  Sequence last_mutation_sequence{};
  std::uint64_t registered_monotonic_ms = 0;
  std::uint64_t last_mutated_monotonic_ms = 0;
  std::uint32_t generation_ancestry_depth = 0;

  [[nodiscard]] bool accepts_new_authority() const noexcept {
    return collective_state_accepts_new_authority(state);
  }
  [[nodiscard]] bool is_live() const noexcept {
    return state != CollectiveState::kUnregistered && !collective_state_is_terminal(state);
  }
};

struct RegistrationOutcome {
  RegistrationResult result = RegistrationResult::kRejectedInvalid;
  CollectiveGeneration generation{};
  CollectiveAttemptId attempt{};
  ReasonChain reasons{};
  bool created = false;
  bool advanced = false;
};

// Bounded, lock free (single threaded by contract) catalog.  All mutation is
// performed by the coordinator while holding its own state lock; the catalog
// itself takes no locks so it can never participate in a lock cycle.
class CollectiveCatalog {
 public:
  explicit CollectiveCatalog(std::uint32_t capacity = limits::kCollectivesMaxLive);

  // Registers or updates a definition.  Participants are canonicalised before
  // comparison, so two semantically equivalent participant orderings produce
  // no generation change.
  RegistrationOutcome register_definition(const CollectiveDefinition& definition,
                                          std::uint64_t now_monotonic_ms);

  // Begins a new attempt for an existing generation.  Fails when the collective
  // is unknown or terminal.
  Status begin_attempt(CollectiveId id, CollectiveAttemptId attempt, std::uint64_t now_monotonic_ms,
                       CollectiveAttemptId& out_previous);
  Status set_state(CollectiveId id, CollectiveState state, std::uint64_t now_monotonic_ms);
  Status retire(CollectiveId id, std::uint64_t now_monotonic_ms);
  Status cancel(CollectiveId id, std::uint64_t now_monotonic_ms);

  [[nodiscard]] const CollectiveRecord* find(CollectiveId id) const noexcept;
  // Mutable access for coordinator bookkeeping that is not part of the durable
  // definition (currently the declared attempt volume).  Never used from the
  // decision path.
  [[nodiscard]] CollectiveRecord* find_mutable(CollectiveId id) noexcept;
  [[nodiscard]] bool contains(CollectiveId id) const noexcept;

  // Stable, ascending-by-id page of every record.  Used by inspection and by
  // persistence; never by the hot decision path.
  [[nodiscard]] std::vector<CollectiveId> ids(std::size_t offset, std::size_t limit) const;
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

  // Bulk load used by the persistence replay path.  Rejects the whole load
  // atomically if any record is impossible; never applies a partial state.
  Status load(const std::vector<CollectiveRecord>& records, std::uint64_t now_monotonic_ms);
  [[nodiscard]] std::vector<CollectiveRecord> export_all() const;

  void clear();

 private:
  IdentityMap<CollectiveRecord> records_;
  std::uint32_t capacity_;
  Sequence mutation_sequence_{};
};

}  // namespace ctf

#endif  // CTF_CATALOG_HPP
