// Collective Traffic Fabric - topology, capacity, congestion and peer liveness
// evidence.  Evidence is observation: it never grants authority by itself and
// it always carries the generation that produced it plus an age bound.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_EVIDENCE_HPP
#define CTF_EVIDENCE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ctf/error.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"
#include "ctf/participant.hpp"
#include "ctf/traffic.hpp"

namespace ctf {

// ---------------------------------------------------------------------------
// Topology evidence.  SYNTHETIC unless a deployment feeds real fabric data.
// ---------------------------------------------------------------------------
enum class LinkClass : std::uint8_t {
  kUnspecified = 0,
  kIntraNode = 1,
  kInterNode = 2,
  kInterRack = 3,
  kInterPod = 4,
  kOversubscribed = 5,
};

[[nodiscard]] std::string_view to_string(LinkClass value) noexcept;

struct TopologyNode {
  NodeId id{};
  RackId rack{};
  bool accelerator_attached = false;
  std::uint32_t accelerator_count = 0;

  friend bool operator==(const TopologyNode& a, const TopologyNode& b) {
    return a.id == b.id && a.rack == b.rack && a.accelerator_attached == b.accelerator_attached &&
           a.accelerator_count == b.accelerator_count;
  }
};

struct TopologyLink {
  LinkId id{};
  SwitchId a{};
  SwitchId b{};
  LinkClass link_class = LinkClass::kUnspecified;
  std::uint64_t capacity_bps = 0;
  std::uint32_t lanes = 1;
  bool shared_oversubscribed = false;

  friend bool operator==(const TopologyLink& a, const TopologyLink& b) {
    return a.id == b.id && a.a == b.a && a.b == b.b && a.link_class == b.link_class &&
           a.capacity_bps == b.capacity_bps && a.lanes == b.lanes &&
           a.shared_oversubscribed == b.shared_oversubscribed;
  }
};

struct TopologyEvidence {
  TopologyGeneration generation{};
  std::vector<TopologyNode> nodes;   // canonical ascending by id
  std::vector<TopologyLink> links;   // canonical ascending by id
  std::uint64_t captured_monotonic_ms = 0;
  bool synthetic = true;             // never claims physical validation otherwise

  [[nodiscard]] const TopologyNode* find_node(NodeId id) const noexcept;
  [[nodiscard]] const TopologyLink* find_link(LinkId id) const noexcept;
  [[nodiscard]] Status validate() const;
  void canonicalize();
};

// ---------------------------------------------------------------------------
// Capacity evidence: fabric supply observation, generation stamped.
// ---------------------------------------------------------------------------
struct LinkCapacity {
  LinkId link{};
  std::uint64_t provisioned_bps = 0;
  std::uint64_t available_bps = 0;   // observation, may be lower than provisioned
  std::uint32_t utilization_bps = 0; // 0..10000 observation
  bool oversubscribed = false;
  std::uint32_t oversubscription_ratio_per_mille = 1000;

  friend bool operator==(const LinkCapacity& a, const LinkCapacity& b) {
    return a.link == b.link && a.provisioned_bps == b.provisioned_bps &&
           a.available_bps == b.available_bps && a.utilization_bps == b.utilization_bps &&
           a.oversubscribed == b.oversubscribed &&
           a.oversubscription_ratio_per_mille == b.oversubscription_ratio_per_mille;
  }
};

struct CapacityEvidence {
  EvidenceGeneration generation{};
  TopologyGeneration topology_generation{};
  std::vector<LinkCapacity> links;   // canonical ascending by link id
  std::uint64_t captured_monotonic_ms = 0;
  bool synthetic = true;

  [[nodiscard]] const LinkCapacity* find(LinkId id) const noexcept;
  [[nodiscard]] Status validate() const;
  void canonicalize();
};

// ---------------------------------------------------------------------------
// Congestion evidence: dynamic, never persisted, never resurrected.
// ---------------------------------------------------------------------------
enum class CongestionLevel : std::uint8_t {
  kUnknown = 0,
  kNone = 1,
  kLow = 2,
  kModerate = 3,
  kHigh = 4,
  kSevere = 5,
};

[[nodiscard]] std::string_view to_string(CongestionLevel value) noexcept;
[[nodiscard]] CongestionLevel congestion_level_from_utilization_bps(std::uint32_t utilization_bps) noexcept;

struct LinkCongestion {
  LinkId link{};
  std::uint32_t utilization_bps = 0;      // 0..10000
  std::uint32_t queue_depth_bytes = 0;
  std::uint32_t ecn_marks_per_mille = 0;
  std::uint32_t pfc_pause_per_mille = 0;
  CongestionLevel level = CongestionLevel::kUnknown;

  friend bool operator==(const LinkCongestion& a, const LinkCongestion& b) {
    return a.link == b.link && a.utilization_bps == b.utilization_bps &&
           a.queue_depth_bytes == b.queue_depth_bytes && a.ecn_marks_per_mille == b.ecn_marks_per_mille &&
           a.pfc_pause_per_mille == b.pfc_pause_per_mille && a.level == b.level;
  }
};

struct CongestionEvidence {
  EvidenceGeneration generation{};
  TopologyGeneration topology_generation{};
  std::vector<LinkCongestion> links;   // canonical ascending by link id
  std::uint64_t captured_monotonic_ms = 0;
  std::uint64_t valid_for_ms = 0;      // age bound asserted by the observer
  bool synthetic = true;

  [[nodiscard]] const LinkCongestion* find(LinkId id) const noexcept;
  [[nodiscard]] Status validate() const;
  void canonicalize();
};

// ---------------------------------------------------------------------------
// Peer liveness evidence.  Reachability is not currency: this record is the
// only source of participant authority, and it expires.
// ---------------------------------------------------------------------------
struct PeerLiveness {
  ParticipantId participant{};
  BootIncarnation incarnation{};
  ParticipantState state = ParticipantState::kUnspecified;
  std::uint64_t last_seen_monotonic_ms = 0;
  std::uint64_t expires_at_monotonic_ms = 0;
  NodeId node{};
  SessionId bound_session{};

  [[nodiscard]] bool is_current(std::uint64_t now_monotonic_ms) const noexcept {
    return state == ParticipantState::kLive && now_monotonic_ms < expires_at_monotonic_ms;
  }
  [[nodiscard]] bool is_present() const noexcept { return state != ParticipantState::kUnspecified; }
};

}  // namespace ctf

#endif  // CTF_EVIDENCE_HPP

