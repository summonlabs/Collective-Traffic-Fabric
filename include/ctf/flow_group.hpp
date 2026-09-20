// Collective Traffic Fabric - flow groups constructed from planned
// communication edges.  A flow group is the unit that carries traffic
// authority; it is never inferred from a participant list alone.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_FLOW_GROUP_HPP
#define CTF_FLOW_GROUP_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ctf/collective.hpp"
#include "ctf/error.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"

namespace ctf {

enum class FlowPattern : std::uint8_t {
  kUnspecified = 0,
  kRing = 1,
  kChain = 2,
  kTreeUp = 3,        // leaves toward root
  kTreeDown = 4,      // root toward leaves
  kStar = 5,          // one hub to/from many
  kFullMesh = 6,
  kNeighborExchange = 7,   // all-to-all with distance bound
  kPairwiseDisjoint = 8,   // matched pairs, no shared endpoints
  kPipeline = 9,
  kCustom = 10,       // planned edges that match no catalogued shape
};

[[nodiscard]] std::string_view to_string(FlowPattern value) noexcept;
[[nodiscard]] bool flow_pattern_from_string(std::string_view text, FlowPattern& out) noexcept;

enum class FlowDirection : std::uint8_t {
  kUnknown = 0,
  kUnidirectional = 1,
  kBidirectional = 2,
};

[[nodiscard]] std::string_view to_string(FlowDirection value) noexcept;

// One directed communication edge.  Endpoints must be members of the owning
// collective's participant set; a plan may not invent participants.
struct FlowEdge {
  ParticipantId source{};
  ParticipantId destination{};
  std::uint64_t logical_bytes = 0;
  Sequence step_index{};
  AlgorithmHint hint = AlgorithmHint::kUnspecified;
  bool crosses_rack = false;

  friend bool operator==(const FlowEdge& a, const FlowEdge& b) {
    return a.source == b.source && a.destination == b.destination &&
           a.logical_bytes == b.logical_bytes && a.step_index == b.step_index &&
           a.hint == b.hint && a.crosses_rack == b.crosses_rack;
  }
};

// A request to build a flow group from a plan.  Canonicalised before use; the
// resulting group id is a pure function of the canonical content, so two
// semantically equivalent plans produce the same group identity.
struct FlowGroupPlan {
  CollectiveInstance instance{};
  // The collective class is part of the plan because it is part of what the plan
  // authorises: two groups over the same edges but for different collective
  // semantics are not the same flow group and must not share an identity.
  CollectiveClass collective_class = CollectiveClass::kUnknown;
  FlowPattern pattern = FlowPattern::kUnspecified;
  FlowDirection direction = FlowDirection::kUnidirectional;
  std::vector<FlowEdge> edges;
  Sequence plan_sequence{};
  bool requires_simultaneous_start = false;

  friend bool operator==(const FlowGroupPlan& a, const FlowGroupPlan& b);
};

struct FlowGroup {
  FlowGroupId id{};
  CollectiveInstance instance{};
  CollectiveClass collective_class = CollectiveClass::kUnknown;
  FlowPattern pattern = FlowPattern::kUnspecified;
  FlowDirection direction = FlowDirection::kUnidirectional;
  std::vector<FlowEdge> edges;        // canonical ascending order
  std::vector<ParticipantId> participants;  // canonical ascending order, derived
  std::uint64_t total_logical_bytes = 0;
  std::uint32_t max_fan_out = 0;
  std::uint32_t max_fan_in = 0;
  std::uint32_t cross_rack_edge_count = 0;
  bool requires_simultaneous_start = false;
  Sequence plan_sequence{};

  friend bool operator==(const FlowGroup& a, const FlowGroup& b);
};

enum class FlowGroupState : std::uint8_t {
  kUnspecified = 0,
  kConstructed = 1,
  kAuthorized = 2,
  kDraining = 3,
  kClosed = 4,
  kCancelled = 5,
};

[[nodiscard]] std::string_view to_string(FlowGroupState value) noexcept;

// Structural validation of a plan against the collective's participant set.
// Rejects self loops, duplicates, unset and unknown endpoints, and out of range
// counts.  Never silently drops an edge.
[[nodiscard]] Status validate_flow_group_plan(const FlowGroupPlan& plan,
                                              const std::vector<ParticipantId>& allowed_participants);

// Deterministic construction: sorts edges, derives the participant set, and
// mints the group id from the canonical content.
[[nodiscard]] Status construct_flow_group(const FlowGroupPlan& plan,
                                          const std::vector<ParticipantId>& allowed_participants,
                                          FlowGroup& out);

// Recomputes the canonical id of a group.  Used to detect tampering and to make
// equivalence claims testable.
[[nodiscard]] FlowGroupId canonical_flow_group_id(const FlowGroupPlan& plan);
// Canonical 64 bit content digest used for equivalence proofs.
[[nodiscard]] std::uint64_t flow_group_content_digest(const FlowGroupPlan& plan) noexcept;

}  // namespace ctf

#endif  // CTF_FLOW_GROUP_HPP
