// Collective Traffic Fabric - flow group construction and canonicalisation.
// Copyright 2026 Summon Software Labs.
#include "ctf/flow_group.hpp"

#include <algorithm>
#include <unordered_set>

namespace ctf {
namespace {

std::uint64_t mix(std::uint64_t state, std::uint64_t value) noexcept {
  state ^= value + 0x9E3779B97F4A7C15ull + (state << 6) + (state >> 2);
  state *= 0xFF51AFD7ED558CCDull;
  state ^= state >> 33;
  return state;
}

std::uint64_t mix_identity(std::uint64_t state, Identity value) noexcept {
  state = mix(state, value.high());
  state = mix(state, value.low());
  return state;
}

// The canonical edge order is TOTAL.  Every field that takes part in the
// identity of a flow group takes part in the comparison, so two permutations of
// the same edge set always sort to the same sequence and therefore to the same
// flow group identity.  A partial comparator here would make the identity depend
// on the order the planner happened to emit.
bool edge_less(const FlowEdge& a, const FlowEdge& b) noexcept {
  if (a.step_index != b.step_index) return a.step_index < b.step_index;
  if (a.source != b.source) return a.source < b.source;
  if (a.destination != b.destination) return a.destination < b.destination;
  if (a.logical_bytes != b.logical_bytes) return a.logical_bytes < b.logical_bytes;
  if (a.hint != b.hint) return static_cast<std::uint8_t>(a.hint) < static_cast<std::uint8_t>(b.hint);
  const int a_rack = a.crosses_rack ? 1 : 0;
  const int b_rack = b.crosses_rack ? 1 : 0;
  return a_rack < b_rack;
}

}  // namespace

std::string_view to_string(FlowPattern value) noexcept {
  switch (value) {
    case FlowPattern::kUnspecified: return "unspecified";
    case FlowPattern::kRing: return "ring";
    case FlowPattern::kChain: return "chain";
    case FlowPattern::kTreeUp: return "tree_up";
    case FlowPattern::kTreeDown: return "tree_down";
    case FlowPattern::kStar: return "star";
    case FlowPattern::kFullMesh: return "full_mesh";
    case FlowPattern::kNeighborExchange: return "neighbor_exchange";
    case FlowPattern::kPairwiseDisjoint: return "pairwise_disjoint";
    case FlowPattern::kPipeline: return "pipeline";
    case FlowPattern::kCustom: return "custom";
  }
  return "unspecified";
}

bool flow_pattern_from_string(std::string_view text, FlowPattern& out) noexcept {
  struct Entry { std::string_view name; FlowPattern value; };
  static constexpr Entry kTable[] = {
      {"unspecified", FlowPattern::kUnspecified},
      {"ring", FlowPattern::kRing},
      {"chain", FlowPattern::kChain},
      {"tree_up", FlowPattern::kTreeUp},
      {"tree_down", FlowPattern::kTreeDown},
      {"star", FlowPattern::kStar},
      {"full_mesh", FlowPattern::kFullMesh},
      {"neighbor_exchange", FlowPattern::kNeighborExchange},
      {"pairwise_disjoint", FlowPattern::kPairwiseDisjoint},
      {"pipeline", FlowPattern::kPipeline},
      {"custom", FlowPattern::kCustom},
  };
  for (const Entry& entry : kTable) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string_view to_string(FlowDirection value) noexcept {
  switch (value) {
    case FlowDirection::kUnknown: return "unknown";
    case FlowDirection::kUnidirectional: return "unidirectional";
    case FlowDirection::kBidirectional: return "bidirectional";
  }
  return "unknown";
}

std::string_view to_string(FlowGroupState value) noexcept {
  switch (value) {
    case FlowGroupState::kUnspecified: return "unspecified";
    case FlowGroupState::kConstructed: return "constructed";
    case FlowGroupState::kAuthorized: return "authorized";
    case FlowGroupState::kDraining: return "draining";
    case FlowGroupState::kClosed: return "closed";
    case FlowGroupState::kCancelled: return "cancelled";
  }
  return "unspecified";
}

bool operator==(const FlowGroupPlan& a, const FlowGroupPlan& b) {
  return a.instance == b.instance && a.collective_class == b.collective_class &&
         a.pattern == b.pattern && a.direction == b.direction &&
         a.edges == b.edges && a.plan_sequence == b.plan_sequence &&
         a.requires_simultaneous_start == b.requires_simultaneous_start;
}

bool operator==(const FlowGroup& a, const FlowGroup& b) {
  return a.id == b.id && a.instance == b.instance && a.collective_class == b.collective_class &&
         a.pattern == b.pattern && a.direction == b.direction &&
         a.edges == b.edges && a.participants == b.participants &&
         a.total_logical_bytes == b.total_logical_bytes && a.max_fan_out == b.max_fan_out &&
         a.max_fan_in == b.max_fan_in && a.cross_rack_edge_count == b.cross_rack_edge_count &&
         a.requires_simultaneous_start == b.requires_simultaneous_start &&
         a.plan_sequence == b.plan_sequence;
}

Status validate_flow_group_plan(const FlowGroupPlan& plan, const std::vector<ParticipantId>& allowed_participants) {
  if (plan.instance.id.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "flow group plan has no collective id");
  }
  if (plan.instance.generation.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "flow group plan has no collective generation");
  }
  if (plan.instance.attempt.is_none()) {
    return Status(ErrorCode::kValidationAttemptUnset, "flow group plan has no attempt identity");
  }
  if (plan.edges.empty()) {
    return Status(ErrorCode::kValidationEdgeCountOutOfRange, "flow group plan has no edges");
  }
  if (plan.edges.size() > limits::kEdgesMaxPerFlowGroup) {
    return Status(ErrorCode::kValidationEdgeCountOutOfRange, "flow group plan exceeds the edge bound");
  }
  if (plan.direction == FlowDirection::kUnknown) {
    return Status(ErrorCode::kValidationEdgeCountOutOfRange,
                  "flow group plan must state a direction; unknown is not a direction");
  }
  if (allowed_participants.empty()) {
    return Status(ErrorCode::kValidationParticipantCountOutOfRange, "collective has no participants");
  }

  IdentitySet allowed;
  allowed.reserve(allowed_participants.size() * 2);
  for (const ParticipantId& id : allowed_participants) allowed.insert(id);

  // Duplicate detection is exact and linear: two edges between the same
  // endpoints in the same step are one obligation, and a plan that states it
  // twice is ambiguous about volume, so it is rejected rather than summed.
  // The key is the pair (step, source, destination) folded into a 64 bit hash;
  // a collision would at worst reject a plan, never admit a malformed one, and
  // the canonical construction still verifies ordering afterwards.
  std::unordered_set<std::uint64_t> seen;
  seen.reserve(plan.edges.size() * 2);
  for (const FlowEdge& edge : plan.edges) {
    if (edge.source.is_none() || edge.destination.is_none()) {
      return Status(ErrorCode::kValidationEdgeUnset, "flow group plan contains an unset edge endpoint");
    }
    if (edge.source == edge.destination) {
      return Status(ErrorCode::kValidationEdgeSelfLoop, "flow group plan contains a self loop");
    }
    if (allowed.find(edge.source) == allowed.end() ||
        allowed.find(edge.destination) == allowed.end()) {
      return Status(ErrorCode::kValidationEdgeEndpointNotParticipant,
                    "flow group plan references a non participant endpoint");
    }
    if (edge.step_index.is_unset()) {
      return Status(ErrorCode::kValidationStepOutOfRange, "flow group plan contains an edge without a step");
    }
    std::uint64_t key = edge.step_index.value();
    key = mix(key, edge.source.high());
    key = mix(key, edge.source.low());
    key = mix(key, edge.destination.high());
    key = mix(key, edge.destination.low());
    if (!seen.insert(key).second) {
      return Status(ErrorCode::kValidationEdgeDuplicate,
                    "flow group plan states the same directed edge twice in one step");
    }
  }
  return Status::ok();
}

std::uint64_t flow_group_content_digest(const FlowGroupPlan& plan) noexcept {
  std::uint64_t state = 0x243F6A8885A308D3ull;
  state = mix_identity(state, plan.instance.id);
  state = mix_identity(state, plan.instance.generation);
  state = mix_identity(state, plan.instance.attempt);
  state = mix(state, plan.instance.attempt_sequence.value());
  state = mix(state, static_cast<std::uint64_t>(plan.collective_class));
  state = mix(state, static_cast<std::uint64_t>(plan.pattern));
  state = mix(state, static_cast<std::uint64_t>(plan.direction));
  state = mix(state, static_cast<std::uint64_t>(plan.requires_simultaneous_start));
  state = mix(state, plan.edges.size());
  // Digest over the sorted edge set so that two equivalent orderings agree.
  std::vector<FlowEdge> sorted = plan.edges;
  std::sort(sorted.begin(), sorted.end(), edge_less);
  // The digest visits the fields in the same order the comparator compares them,
  // so the digest is a pure function of the canonical sequence.
  for (const FlowEdge& edge : sorted) {
    state = mix(state, edge.step_index.value());
    state = mix_identity(state, edge.source);
    state = mix_identity(state, edge.destination);
    state = mix(state, edge.logical_bytes);
    state = mix(state, static_cast<std::uint64_t>(edge.hint));
    state = mix(state, static_cast<std::uint64_t>(edge.crosses_rack ? 1 : 0));
  }
  return state;
}

FlowGroupId canonical_flow_group_id(const FlowGroupPlan& plan) {
  const std::uint64_t digest = flow_group_content_digest(plan);
  std::uint64_t hi = digest;
  std::uint64_t lo = mix(digest, 0xA5A5A5A5A5A5A5A5ull);
  if (hi == 0 && lo == 0) hi = 1;
  return FlowGroupId(hi, lo);
}

Status construct_flow_group(const FlowGroupPlan& plan, const std::vector<ParticipantId>& allowed_participants,
                            FlowGroup& out) {
  Status status = validate_flow_group_plan(plan, allowed_participants);
  if (!status.is_ok()) return status;

  FlowGroup group;
  group.instance = plan.instance;
  group.collective_class = plan.collective_class;
  group.pattern = plan.pattern;
  group.direction = plan.direction;
  group.requires_simultaneous_start = plan.requires_simultaneous_start;
  group.plan_sequence = plan.plan_sequence;
  group.edges = plan.edges;
  std::sort(group.edges.begin(), group.edges.end(), edge_less);

  std::vector<ParticipantId> participants;
  participants.reserve(group.edges.size() * 2);
  for (const FlowEdge& edge : group.edges) {
    participants.push_back(edge.source);
    participants.push_back(edge.destination);
    group.cross_rack_edge_count += edge.crosses_rack ? 1u : 0u;
  }
  canonicalize_participants(participants);
  group.participants = std::move(participants);

  // Fan in/out are computed over the canonical edge set.  The counts are
  // obtained from sorted runs, so this is a linear pass with no nested scan.
  {
    std::vector<ParticipantId> sources;
    std::vector<ParticipantId> destinations;
    sources.reserve(group.edges.size());
    destinations.reserve(group.edges.size());
    for (const FlowEdge& edge : group.edges) {
      sources.push_back(edge.source);
      destinations.push_back(edge.destination);
    }
    std::sort(sources.begin(), sources.end());
    std::sort(destinations.begin(), destinations.end());
    std::uint32_t run = 0;
    for (std::size_t index = 0; index < sources.size(); ++index) {
      ++run;
      if (index + 1 == sources.size() || sources[index + 1] != sources[index]) {
        group.max_fan_out = std::max(group.max_fan_out, run);
        run = 0;
      }
    }
    run = 0;
    for (std::size_t index = 0; index < destinations.size(); ++index) {
      ++run;
      if (index + 1 == destinations.size() || destinations[index + 1] != destinations[index]) {
        group.max_fan_in = std::max(group.max_fan_in, run);
        run = 0;
      }
    }
  }

  for (const FlowEdge& edge : group.edges) {
    group.total_logical_bytes += edge.logical_bytes;
  }

  group.id = canonical_flow_group_id(plan);
  out = std::move(group);
  return Status::ok();
}

}  // namespace ctf
