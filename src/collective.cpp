// Collective Traffic Fabric - collective model implementation.
// Copyright 2026 Summon Software Labs.
#include "ctf/collective.hpp"

#include <algorithm>
#include <unordered_set>

namespace ctf {

std::string_view to_string(CollectiveClass value) noexcept {
  switch (value) {
    case CollectiveClass::kUnknown: return "unknown";
    case CollectiveClass::kAllReduce: return "all_reduce";
    case CollectiveClass::kAllGather: return "all_gather";
    case CollectiveClass::kReduceScatter: return "reduce_scatter";
    case CollectiveClass::kBroadcast: return "broadcast";
    case CollectiveClass::kGather: return "gather";
    case CollectiveClass::kScatter: return "scatter";
    case CollectiveClass::kAllToAll: return "all_to_all";
    case CollectiveClass::kPointToPoint: return "point_to_point";
    case CollectiveClass::kBarrierOnly: return "barrier_only";
    case CollectiveClass::kSendRecvGrouped: return "send_recv_grouped";
    case CollectiveClass::kReduce: return "reduce";
    case CollectiveClass::kScan: return "scan";
    case CollectiveClass::kSendRecvSplit: return "send_recv_split";
    case CollectiveClass::kUnknownVendor: return "unknown_vendor";
  }
  return "unknown";
}

bool collective_class_from_string(std::string_view text, CollectiveClass& out) noexcept {
  struct Entry { std::string_view name; CollectiveClass value; };
  static constexpr Entry kTable[] = {
      {"unknown", CollectiveClass::kUnknown},
      {"all_reduce", CollectiveClass::kAllReduce},
      {"all_gather", CollectiveClass::kAllGather},
      {"reduce_scatter", CollectiveClass::kReduceScatter},
      {"broadcast", CollectiveClass::kBroadcast},
      {"gather", CollectiveClass::kGather},
      {"scatter", CollectiveClass::kScatter},
      {"all_to_all", CollectiveClass::kAllToAll},
      {"point_to_point", CollectiveClass::kPointToPoint},
      {"barrier_only", CollectiveClass::kBarrierOnly},
      {"send_recv_grouped", CollectiveClass::kSendRecvGrouped},
      {"reduce", CollectiveClass::kReduce},
      {"scan", CollectiveClass::kScan},
      {"send_recv_split", CollectiveClass::kSendRecvSplit},
      {"unknown_vendor", CollectiveClass::kUnknownVendor},
  };
  for (const Entry& entry : kTable) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

bool is_known_collective_class(CollectiveClass value) noexcept {
  return value != CollectiveClass::kUnknown && value != CollectiveClass::kUnknownVendor;
}

std::string_view to_string(AlgorithmHint value) noexcept {
  switch (value) {
    case AlgorithmHint::kUnspecified: return "unspecified";
    case AlgorithmHint::kRing: return "ring";
    case AlgorithmHint::kTree: return "tree";
    case AlgorithmHint::kDoubleBinaryTree: return "double_binary_tree";
    case AlgorithmHint::kHierarchical: return "hierarchical";
    case AlgorithmHint::kRecursiveDoubling: return "recursive_doubling";
    case AlgorithmHint::kRabenseifner: return "rabenseifner";
    case AlgorithmHint::kButterfly: return "butterfly";
    case AlgorithmHint::kDirect: return "direct";
    case AlgorithmHint::kPipeline: return "pipeline";
    case AlgorithmHint::kRingChunked: return "ring_chunked";
    case AlgorithmHint::kVendorSpecific: return "vendor_specific";
  }
  return "unspecified";
}

bool algorithm_hint_from_string(std::string_view text, AlgorithmHint& out) noexcept {
  struct Entry { std::string_view name; AlgorithmHint value; };
  static constexpr Entry kTable[] = {
      {"unspecified", AlgorithmHint::kUnspecified},
      {"ring", AlgorithmHint::kRing},
      {"tree", AlgorithmHint::kTree},
      {"double_binary_tree", AlgorithmHint::kDoubleBinaryTree},
      {"hierarchical", AlgorithmHint::kHierarchical},
      {"recursive_doubling", AlgorithmHint::kRecursiveDoubling},
      {"rabenseifner", AlgorithmHint::kRabenseifner},
      {"butterfly", AlgorithmHint::kButterfly},
      {"direct", AlgorithmHint::kDirect},
      {"pipeline", AlgorithmHint::kPipeline},
      {"ring_chunked", AlgorithmHint::kRingChunked},
      {"vendor_specific", AlgorithmHint::kVendorSpecific},
  };
  for (const Entry& entry : kTable) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

bool algorithm_hint_is_specified(AlgorithmHint value) noexcept { return value != AlgorithmHint::kUnspecified; }

std::string_view to_string(CollectiveScope value) noexcept {
  switch (value) {
    case CollectiveScope::kUnspecified: return "unspecified";
    case CollectiveScope::kIntraNode: return "intra_node";
    case CollectiveScope::kInterNode: return "inter_node";
    case CollectiveScope::kCrossRack: return "cross_rack";
    case CollectiveScope::kCrossPod: return "cross_pod";
    case CollectiveScope::kHybrid: return "hybrid";
  }
  return "unspecified";
}

bool collective_scope_from_string(std::string_view text, CollectiveScope& out) noexcept {
  struct Entry { std::string_view name; CollectiveScope value; };
  static constexpr Entry kTable[] = {
      {"unspecified", CollectiveScope::kUnspecified},
      {"intra_node", CollectiveScope::kIntraNode},
      {"inter_node", CollectiveScope::kInterNode},
      {"cross_rack", CollectiveScope::kCrossRack},
      {"cross_pod", CollectiveScope::kCrossPod},
      {"hybrid", CollectiveScope::kHybrid},
  };
  for (const Entry& entry : kTable) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string_view to_string(CollectiveState value) noexcept {
  switch (value) {
    case CollectiveState::kUnregistered: return "unregistered";
    case CollectiveState::kRegistered: return "registered";
    case CollectiveState::kPlanning: return "planning";
    case CollectiveState::kActive: return "active";
    case CollectiveState::kDraining: return "draining";
    case CollectiveState::kCompleted: return "completed";
    case CollectiveState::kCancelled: return "cancelled";
    case CollectiveState::kRetired: return "retired";
    case CollectiveState::kTimedOut: return "timed_out";
  }
  return "unregistered";
}

bool collective_state_is_terminal(CollectiveState value) noexcept {
  switch (value) {
    case CollectiveState::kCompleted:
    case CollectiveState::kCancelled:
    case CollectiveState::kRetired:
    case CollectiveState::kTimedOut:
      return true;
    default:
      return false;
  }
}

bool collective_state_accepts_new_authority(CollectiveState value) noexcept {
  switch (value) {
    case CollectiveState::kRegistered:
    case CollectiveState::kPlanning:
    case CollectiveState::kActive:
    case CollectiveState::kDraining:
      return true;
    default:
      return false;
  }
}

bool Metadata::set(std::string key, std::string value) {
  if (key.empty() || key.size() > limits::kMetadataKeyBytesMax) return false;
  if (value.size() > limits::kMetadataValueBytesMax) return false;
  for (auto& entry : entries_) {
    if (entry.first == key) {
      entry.second = std::move(value);
      return true;
    }
  }
  if (entries_.size() >= limits::kMetadataEntriesMax) return false;
  entries_.emplace_back(std::move(key), std::move(value));
  return true;
}

bool Metadata::has(std::string_view key) const noexcept { return find(key) != nullptr; }

const std::string* Metadata::find(std::string_view key) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

bool operator==(const CollectiveDefinition& a, const CollectiveDefinition& b) {
  return a.id == b.id && a.generation == b.generation && a.collective_class == b.collective_class &&
         a.algorithm_hint == b.algorithm_hint && a.scope == b.scope && a.label == b.label &&
         a.origin_node == b.origin_node && a.fabric == b.fabric && a.participants == b.participants &&
         a.phases == b.phases && a.steps == b.steps && a.metadata.entries() == b.metadata.entries() &&
         a.declares_barrier_semantics == b.declares_barrier_semantics &&
         a.logical_bytes == b.logical_bytes && a.registration_sequence == b.registration_sequence;
}

void canonicalize_participants(std::vector<ParticipantId>& participants) {
  std::sort(participants.begin(), participants.end());
  participants.erase(std::unique(participants.begin(), participants.end()), participants.end());
}

bool participants_are_canonical(const std::vector<ParticipantId>& participants) noexcept {
  for (std::size_t index = 1; index < participants.size(); ++index) {
    if (!(participants[index - 1] < participants[index])) return false;
  }
  return true;
}

bool participant_set_equals(const std::vector<ParticipantId>& a, const std::vector<ParticipantId>& b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (a[index] != b[index]) return false;
  }
  return true;
}

}  // namespace ctf
