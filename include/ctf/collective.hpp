// Collective Traffic Fabric - collective identity, class, algorithm evidence,
// phase and step semantics as supplied by a communication planner.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_COLLECTIVE_HPP
#define CTF_COLLECTIVE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cstddef>
#include <utility>

#include "ctf/error.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"

namespace ctf {

// The network-visible collective communication classes owned by this boundary.
enum class CollectiveClass : std::uint8_t {
  kUnknown = 0,
  kAllReduce = 1,
  kAllGather = 2,
  kReduceScatter = 3,
  kBroadcast = 4,
  kGather = 5,
  kScatter = 6,
  kAllToAll = 7,
  kPointToPoint = 8,
  kBarrierOnly = 9,        // synchronization with no modelled payload edges
  kSendRecvGrouped = 10,   // grouped point-to-point send/recv traffic
  kReduce = 11,            // many-to-one reduction without the result broadcast
  kScan = 12,              // prefix/scan variants
  kSendRecvSplit = 13,     // a single directed pair treated as its own group
  kUnknownVendor = 14,     // explicitly vendor specific and NOT interpretable here
};

[[nodiscard]] std::string_view to_string(CollectiveClass value) noexcept;
// Returns false for any spelling not in the canonical table.
[[nodiscard]] bool collective_class_from_string(std::string_view text, CollectiveClass& out) noexcept;

// True when the class is understood well enough to be given traffic treatment.
// kUnknown and kUnknownVendor are the only false cases; callers must not fold
// them together with anything else.
[[nodiscard]] bool is_known_collective_class(CollectiveClass value) noexcept;

// Algorithm hints are EVIDENCE or a REQUESTED structure.  They are never proof
// of the physical path a collective takes.
enum class AlgorithmHint : std::uint8_t {
  kUnspecified = 0,
  kRing = 1,
  kTree = 2,
  kDoubleBinaryTree = 3,
  kHierarchical = 4,
  kRecursiveDoubling = 5,
  kRabenseifner = 6,
  kButterfly = 7,
  kDirect = 8,
  kPipeline = 9,
  kRingChunked = 10,
  kVendorSpecific = 11,
};

[[nodiscard]] std::string_view to_string(AlgorithmHint value) noexcept;
[[nodiscard]] bool algorithm_hint_from_string(std::string_view text, AlgorithmHint& out) noexcept;
// Hints name a structure; only kUnspecified means "the planner said nothing".
[[nodiscard]] bool algorithm_hint_is_specified(AlgorithmHint value) noexcept;

enum class CollectiveScope : std::uint8_t {
  kUnspecified = 0,
  kIntraNode = 1,
  kInterNode = 2,
  kCrossRack = 3,
  kCrossPod = 4,
  kHybrid = 5,
};

[[nodiscard]] std::string_view to_string(CollectiveScope value) noexcept;
[[nodiscard]] bool collective_scope_from_string(std::string_view text, CollectiveScope& out) noexcept;

enum class CollectiveState : std::uint8_t {
  kUnregistered = 0,
  kRegistered = 1,   // durable definition exists, no live evidence yet
  kPlanning = 2,     // a plan was submitted, decisions may be issued
  kActive = 3,       // live traffic treatment in force
  kDraining = 4,     // completion is ending; no new authority is issued
  kCompleted = 5,    // terminal, no authority may be re-issued for this generation
  kCancelled = 6,    // terminal, stale completion must never restore it
  kRetired = 7,      // terminal, definition released
  kTimedOut = 8,     // terminal after liveness loss
};

[[nodiscard]] std::string_view to_string(CollectiveState value) noexcept;
[[nodiscard]] bool collective_state_is_terminal(CollectiveState value) noexcept;
[[nodiscard]] bool collective_state_accepts_new_authority(CollectiveState value) noexcept;

// Planner supplied phase description.  Phases and steps order communication
// within one collective attempt; they are evidence about intent, not about the
// fabric.
struct CollectivePhase {
  Sequence phase_index{};
  std::string label;
  bool synchronization_point = false;
  std::uint32_t step_count = 0;

  friend bool operator==(const CollectivePhase& a, const CollectivePhase& b) {
    return a.phase_index == b.phase_index && a.label == b.label &&
           a.synchronization_point == b.synchronization_point && a.step_count == b.step_count;
  }
  friend bool operator!=(const CollectivePhase& a, const CollectivePhase& b) { return !(a == b); }
};

struct CollectiveStep {
  Sequence phase_index{};
  Sequence step_index{};
  AlgorithmHint hint = AlgorithmHint::kUnspecified;
  bool synchronization_point = false;

  friend bool operator==(const CollectiveStep& a, const CollectiveStep& b) {
    return a.phase_index == b.phase_index && a.step_index == b.step_index && a.hint == b.hint &&
           a.synchronization_point == b.synchronization_point;
  }
  friend bool operator!=(const CollectiveStep& a, const CollectiveStep& b) { return !(a == b); }
};

// Bounded, ordered metadata.  Keys are unique and preserved in insertion order
// so that canonicalisation is deterministic.
class Metadata {
 public:
  Metadata() = default;

  bool set(std::string key, std::string value);
  [[nodiscard]] bool has(std::string_view key) const noexcept;
  [[nodiscard]] const std::string* find(std::string_view key) const noexcept;
  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const noexcept { return entries_; }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  void clear() noexcept { entries_.clear(); }

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

// A durable collective definition.  Immutable once registered for a given
// generation: a membership or semantics change mints a new generation.
struct CollectiveDefinition {
  CollectiveId id{};
  CollectiveGeneration generation{};
  CollectiveClass collective_class = CollectiveClass::kUnknown;
  AlgorithmHint algorithm_hint = AlgorithmHint::kUnspecified;
  CollectiveScope scope = CollectiveScope::kUnspecified;
  std::string label;
  NodeId origin_node{};
  FabricInstanceId fabric{};
  std::vector<ParticipantId> participants;  // canonical ascending order
  std::vector<CollectivePhase> phases;
  std::vector<CollectiveStep> steps;
  Metadata metadata;
  bool declares_barrier_semantics = false;
  std::uint64_t logical_bytes = 0;
  Sequence registration_sequence{};  // coordinator assigned, monotonic

  friend bool operator==(const CollectiveDefinition& a, const CollectiveDefinition& b);
  friend bool operator!=(const CollectiveDefinition& a, const CollectiveDefinition& b) { return !(a == b); }
};

// The full identity of a collective instance: definition generation plus the
// attempt that is actually traversing the fabric.
struct CollectiveInstance {
  CollectiveId id{};
  CollectiveGeneration generation{};
  CollectiveAttemptId attempt{};
  Sequence attempt_sequence{};

  friend bool operator==(const CollectiveInstance& a, const CollectiveInstance& b) {
    return a.id == b.id && a.generation == b.generation && a.attempt == b.attempt;
  }
  friend bool operator!=(const CollectiveInstance& a, const CollectiveInstance& b) { return !(a == b); }
};

// Canonical ordering helpers.  Ordering is total and independent of the order
// in which a peer happened to send its participant list.
void canonicalize_participants(std::vector<ParticipantId>& participants);
[[nodiscard]] bool participants_are_canonical(const std::vector<ParticipantId>& participants) noexcept;
[[nodiscard]] bool participant_set_equals(const std::vector<ParticipantId>& a,
                                         const std::vector<ParticipantId>& b) noexcept;

}  // namespace ctf

#endif  // CTF_COLLECTIVE_HPP
