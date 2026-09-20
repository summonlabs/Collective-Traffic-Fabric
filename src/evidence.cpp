// Collective Traffic Fabric - evidence validation and lookup.
// Copyright 2026 Summon Software Labs.
#include "ctf/evidence.hpp"

#include <algorithm>
#include <unordered_set>

#include "ctf/snapshot.hpp"
#include "ctf/traffic.hpp"

namespace ctf {
namespace {

template <typename T, typename Key>
const T* binary_find(const std::vector<T>& items, const Key& key, Key (*project)(const T&)) {
  auto position = std::lower_bound(items.begin(), items.end(), key,
                                   [project](const T& item, const Key& value) { return project(item) < value; });
  if (position == items.end() || !(project(*position) == key)) return nullptr;
  return &(*position);
}

}  // namespace

std::string_view to_string(LinkClass value) noexcept {
  switch (value) {
    case LinkClass::kUnspecified: return "unspecified";
    case LinkClass::kIntraNode: return "intra_node";
    case LinkClass::kInterNode: return "inter_node";
    case LinkClass::kInterRack: return "inter_rack";
    case LinkClass::kInterPod: return "inter_pod";
    case LinkClass::kOversubscribed: return "oversubscribed";
  }
  return "unspecified";
}

std::string_view to_string(CongestionLevel value) noexcept {
  switch (value) {
    case CongestionLevel::kUnknown: return "unknown";
    case CongestionLevel::kNone: return "none";
    case CongestionLevel::kLow: return "low";
    case CongestionLevel::kModerate: return "moderate";
    case CongestionLevel::kHigh: return "high";
    case CongestionLevel::kSevere: return "severe";
  }
  return "unknown";
}

CongestionLevel congestion_level_from_utilization_bps(std::uint32_t utilization_bps) noexcept {
  if (utilization_bps >= 9500) return CongestionLevel::kSevere;
  if (utilization_bps >= 8500) return CongestionLevel::kHigh;
  if (utilization_bps >= 7500) return CongestionLevel::kModerate;
  if (utilization_bps >= 6000) return CongestionLevel::kLow;
  return CongestionLevel::kNone;
}

std::string_view to_string(EvidenceKind value) noexcept {
  switch (value) {
    case EvidenceKind::kTopology: return "topology";
    case EvidenceKind::kCapacity: return "capacity";
    case EvidenceKind::kCongestion: return "congestion";
    case EvidenceKind::kPeerLiveness: return "peer_liveness";
  }
  return "topology";
}

std::uint64_t FreshnessPolicy::max_age_ms(EvidenceKind kind) const noexcept {
  switch (kind) {
    case EvidenceKind::kTopology: return topology_max_age_ms;
    case EvidenceKind::kCapacity: return capacity_max_age_ms;
    case EvidenceKind::kCongestion: return congestion_max_age_ms;
    case EvidenceKind::kPeerLiveness: return peer_liveness_max_age_ms;
  }
  return 0;
}

// ---- topology --------------------------------------------------------------
void TopologyEvidence::canonicalize() {
  std::sort(nodes.begin(), nodes.end(), [](const TopologyNode& a, const TopologyNode& b) { return a.id < b.id; });
  std::sort(links.begin(), links.end(), [](const TopologyLink& a, const TopologyLink& b) { return a.id < b.id; });
}

const TopologyNode* TopologyEvidence::find_node(NodeId id) const noexcept {
  return binary_find<TopologyNode, NodeId>(nodes, id, [](const TopologyNode& node) { return node.id; });
}

const TopologyLink* TopologyEvidence::find_link(LinkId id) const noexcept {
  return binary_find<TopologyLink, LinkId>(links, id, [](const TopologyLink& link) { return link.id; });
}

Status TopologyEvidence::validate() const {
  if (generation.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "topology evidence has no generation");
  }
  if (nodes.size() > limits::kTopologyNodesMax) {
    return Status(ErrorCode::kValidationNodeUnknown, "topology exceeds the node bound");
  }
  if (links.size() > limits::kTopologyLinksMax) {
    return Status(ErrorCode::kValidationNodeUnknown, "topology exceeds the link bound");
  }
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (nodes[index].id.is_none()) {
      return Status(ErrorCode::kValidationNodeUnknown, "topology contains an unset node identity");
    }
    if (index > 0 && !(nodes[index - 1].id < nodes[index].id)) {
      return Status(ErrorCode::kDecodeNonCanonical, "topology nodes are not strictly ascending");
    }
  }
  for (std::size_t index = 0; index < links.size(); ++index) {
    const TopologyLink& link = links[index];
    if (link.id.is_none()) {
      return Status(ErrorCode::kValidationNodeUnknown, "topology contains an unset link identity");
    }
    if (index > 0 && !(links[index - 1].id < links[index].id)) {
      return Status(ErrorCode::kDecodeNonCanonical, "topology links are not strictly ascending");
    }
    if (link.capacity_bps == 0) {
      return Status(ErrorCode::kValidationCapacityNegative, "topology link has no capacity");
    }
    if (link.lanes == 0) {
      return Status(ErrorCode::kValidationCapacityNegative, "topology link has zero lanes");
    }
  }
  return Status::ok();
}

// ---- capacity --------------------------------------------------------------
void CapacityEvidence::canonicalize() {
  std::sort(links.begin(), links.end(), [](const LinkCapacity& a, const LinkCapacity& b) { return a.link < b.link; });
}

const LinkCapacity* CapacityEvidence::find(LinkId id) const noexcept {
  return binary_find<LinkCapacity, LinkId>(links, id, [](const LinkCapacity& entry) { return entry.link; });
}

Status CapacityEvidence::validate() const {
  if (generation.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "capacity evidence has no generation");
  }
  if (topology_generation.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "capacity evidence has no topology generation");
  }
  if (links.size() > limits::kTopologyLinksMax) {
    return Status(ErrorCode::kValidationCapacityNegative, "capacity evidence exceeds the link bound");
  }
  for (std::size_t index = 0; index < links.size(); ++index) {
    const LinkCapacity& entry = links[index];
    if (entry.link.is_none()) {
      return Status(ErrorCode::kValidationNodeUnknown, "capacity evidence contains an unset link identity");
    }
    if (index > 0 && !(links[index - 1].link < entry.link)) {
      return Status(ErrorCode::kDecodeNonCanonical, "capacity entries are not strictly ascending");
    }
    if (entry.utilization_bps > 10000) {
      return Status(ErrorCode::kValidationCapacityNegative, "capacity utilization exceeds 100 percent");
    }
    if (entry.provisioned_bps == 0) {
      return Status(ErrorCode::kValidationCapacityNegative, "capacity entry claims zero provisioned bandwidth");
    }
    if (entry.available_bps > entry.provisioned_bps) {
      return Status(ErrorCode::kValidationCapacityNegative,
                    "capacity entry claims more available bandwidth than provisioned");
    }
    if (entry.oversubscription_ratio_per_mille == 0) {
      return Status(ErrorCode::kValidationCapacityNegative, "capacity entry claims an impossible oversubscription ratio");
    }
  }
  return Status::ok();
}

// ---- congestion ------------------------------------------------------------
void CongestionEvidence::canonicalize() {
  std::sort(links.begin(), links.end(),
            [](const LinkCongestion& a, const LinkCongestion& b) { return a.link < b.link; });
}

const LinkCongestion* CongestionEvidence::find(LinkId id) const noexcept {
  return binary_find<LinkCongestion, LinkId>(links, id, [](const LinkCongestion& entry) { return entry.link; });
}

Status CongestionEvidence::validate() const {
  if (generation.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "congestion evidence has no generation");
  }
  if (topology_generation.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "congestion evidence has no topology generation");
  }
  if (valid_for_ms == 0) {
    return Status(ErrorCode::kValidationRateInvalid, "congestion evidence states no validity window");
  }
  if (valid_for_ms > 3600000ull) {
    return Status(ErrorCode::kValidationRateInvalid, "congestion evidence claims an implausible validity window");
  }
  if (links.size() > limits::kTopologyLinksMax) {
    return Status(ErrorCode::kValidationCapacityNegative, "congestion evidence exceeds the link bound");
  }
  for (std::size_t index = 0; index < links.size(); ++index) {
    const LinkCongestion& entry = links[index];
    if (entry.link.is_none()) {
      return Status(ErrorCode::kValidationNodeUnknown, "congestion evidence contains an unset link identity");
    }
    if (index > 0 && !(links[index - 1].link < entry.link)) {
      return Status(ErrorCode::kDecodeNonCanonical, "congestion entries are not strictly ascending");
    }
    if (entry.utilization_bps > 10000) {
      return Status(ErrorCode::kValidationCapacityNegative, "congestion utilization exceeds 100 percent");
    }
    if (entry.ecn_marks_per_mille > 1000 || entry.pfc_pause_per_mille > 1000) {
      return Status(ErrorCode::kValidationRateInvalid, "congestion counters exceed their scale");
    }
    if (entry.level == CongestionLevel::kUnknown) {
      return Status(ErrorCode::kValidationClassUnknownRejected,
                    "congestion entry does not state a level; unknown is not evidence");
    }
    if (entry.level != congestion_level_from_utilization_bps(entry.utilization_bps)) {
      return Status(ErrorCode::kDecodeNonCanonical,
                    "congestion entry level contradicts its own utilization");
    }
  }
  return Status::ok();
}

const PeerLiveness* EvaluationSnapshot::find_peer(ParticipantId id) const noexcept {
  auto position = std::lower_bound(peers.begin(), peers.end(), id,
                                   [](const PeerLiveness& entry, ParticipantId value) {
                                     return entry.participant < value;
                                   });
  if (position == peers.end() || !(position->participant == id)) return nullptr;
  return &(*position);
}

}  // namespace ctf
