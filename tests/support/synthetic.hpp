// Collective Traffic Fabric - synthetic evidence and workload builders.
// Copyright 2026 Summon Software Labs.
//
// Everything this header produces is SYNTHETIC.  It models a multi-rack fabric
// with oversubscribed uplinks and per-link congestion observations, but no
// physical fabric is measured and no hardware claim is made anywhere.
#ifndef CTF_TESTS_SYNTHETIC_HPP
#define CTF_TESTS_SYNTHETIC_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ctf/collective.hpp"
#include "ctf/evidence.hpp"
#include "ctf/flow_group.hpp"
#include "ctf/identity.hpp"
#include "ctf/snapshot.hpp"
#include "ctf/traffic.hpp"

namespace ctf::test {

// Deterministic identifier source.  Seeded, so a whole topology or workload can
// be reproduced byte for byte from one integer.
class IdFactory {
 public:
  explicit IdFactory(std::uint64_t seed) : minter_(seed) {}
  [[nodiscard]] Identity next() { return minter_.next(); }

 private:
  IdentityMinter minter_;
};

struct RackFabric {
  std::vector<NodeId> nodes;
  std::vector<RackId> racks;
  std::vector<SwitchId> switches;
  std::vector<LinkId> links;
  std::vector<std::uint64_t> link_capacity_bps;
  TopologyEvidence topology;
};

// Builds a racks x nodes_per_rack fabric with one top of rack switch per rack,
// one spine, and uplinks that are deliberately oversubscribed.
[[nodiscard]] inline RackFabric make_fabric(std::uint64_t seed, std::uint32_t rack_count,
                                            std::uint32_t nodes_per_rack,
                                            std::uint64_t link_capacity_bps = 400ull * 1000 * 1000 * 1000) {
  IdFactory ids(seed);
  RackFabric fabric;
  fabric.racks.reserve(rack_count);
  for (std::uint32_t index = 0; index < rack_count; ++index) fabric.racks.push_back(ids.next());
  fabric.switches.reserve(rack_count + 1);
  for (std::uint32_t index = 0; index < rack_count + 1; ++index) fabric.switches.push_back(ids.next());
  fabric.nodes.reserve(static_cast<std::size_t>(rack_count) * nodes_per_rack);
  for (std::uint32_t rack = 0; rack < rack_count; ++rack) {
    for (std::uint32_t node = 0; node < nodes_per_rack; ++node) {
      TopologyNode entry;
      entry.id = ids.next();
      entry.rack = fabric.racks[rack];
      entry.accelerator_attached = true;
      entry.accelerator_count = 8;
      fabric.topology.nodes.push_back(entry);
      fabric.nodes.push_back(entry.id);
    }
  }
  const SwitchId spine = fabric.switches.back();
  for (std::uint32_t rack = 0; rack < rack_count; ++rack) {
    TopologyLink downlink;
    downlink.id = ids.next();
    downlink.a = fabric.switches[rack];
    downlink.b = spine;
    downlink.link_class = LinkClass::kInterRack;
    downlink.capacity_bps = link_capacity_bps;
    downlink.lanes = 4;
    downlink.shared_oversubscribed = true;
    fabric.topology.links.push_back(downlink);
    fabric.links.push_back(downlink.id);
    fabric.link_capacity_bps.push_back(downlink.capacity_bps);
  }
  fabric.topology.generation = ids.next();
  fabric.topology.captured_monotonic_ms = 0;
  fabric.topology.synthetic = true;
  fabric.topology.canonicalize();
  return fabric;
}

[[nodiscard]] inline CapacityEvidence make_capacity(const TopologyEvidence& topology, std::uint64_t seed,
                                                    std::uint32_t utilization_bps,
                                                    std::uint64_t available_fraction_per_mille = 1000) {
  IdFactory ids(seed);
  CapacityEvidence evidence;
  evidence.generation = ids.next();
  evidence.topology_generation = topology.generation;
  evidence.captured_monotonic_ms = topology.captured_monotonic_ms;
  evidence.synthetic = true;
  for (const TopologyLink& link : topology.links) {
    LinkCapacity entry;
    entry.link = link.id;
    entry.provisioned_bps = link.capacity_bps;
    entry.available_bps = (link.capacity_bps / 1000ull) * available_fraction_per_mille;
    entry.utilization_bps = utilization_bps;
    entry.oversubscribed = link.shared_oversubscribed;
    entry.oversubscription_ratio_per_mille = 4000;
    evidence.links.push_back(entry);
  }
  evidence.canonicalize();
  return evidence;
}

[[nodiscard]] inline CongestionEvidence make_congestion(const TopologyEvidence& topology, std::uint64_t seed,
                                                        std::uint32_t utilization_bps,
                                                        std::uint64_t valid_for_ms = 2000) {
  IdFactory ids(seed);
  CongestionEvidence evidence;
  evidence.generation = ids.next();
  evidence.topology_generation = topology.generation;
  evidence.captured_monotonic_ms = 0;
  evidence.valid_for_ms = valid_for_ms;
  evidence.synthetic = true;
  for (const TopologyLink& link : topology.links) {
    LinkCongestion entry;
    entry.link = link.id;
    entry.utilization_bps = utilization_bps;
    entry.queue_depth_bytes = utilization_bps / 10;
    entry.ecn_marks_per_mille = utilization_bps / 100;
    entry.pfc_pause_per_mille = 0;
    entry.level = congestion_level_from_utilization_bps(utilization_bps);
    evidence.links.push_back(entry);
  }
  evidence.canonicalize();
  return evidence;
}

[[nodiscard]] inline CollectiveDefinition make_definition(CollectiveId id, CollectiveClass collective_class,
                                                          const std::vector<ParticipantId>& participants,
                                                          const std::string& label = "collective",
                                                          std::uint64_t logical_bytes = 1ull << 30) {
  CollectiveDefinition definition;
  definition.id = id;
  definition.collective_class = collective_class;
  definition.label = label;
  definition.participants = participants;
  canonicalize_participants(definition.participants);
  definition.logical_bytes = logical_bytes;
  return definition;
}

// Ring plan: participant i sends to participant i+1 modulo the ring size.
[[nodiscard]] inline FlowGroupPlan make_ring_plan(const CollectiveInstance& instance,
                                                  const std::vector<ParticipantId>& participants,
                                                  std::uint64_t bytes_per_edge, Sequence step = Sequence(1)) {
  FlowGroupPlan plan;
  plan.instance = instance;
  plan.collective_class = CollectiveClass::kAllReduce;
  plan.pattern = FlowPattern::kRing;
  plan.direction = FlowDirection::kUnidirectional;
  plan.plan_sequence = Sequence(1);
  plan.requires_simultaneous_start = true;
  for (std::size_t index = 0; index < participants.size(); ++index) {
    FlowEdge edge;
    edge.source = participants[index];
    edge.destination = participants[(index + 1) % participants.size()];
    edge.logical_bytes = bytes_per_edge;
    edge.step_index = step;
    edge.hint = AlgorithmHint::kRing;
    edge.crosses_rack = true;
    plan.edges.push_back(edge);
  }
  return plan;
}

// Full mesh plan: every ordered pair exactly once.
[[nodiscard]] inline FlowGroupPlan make_full_mesh_plan(const CollectiveInstance& instance,
                                                       const std::vector<ParticipantId>& participants,
                                                       std::uint64_t bytes_per_edge) {
  FlowGroupPlan plan;
  plan.instance = instance;
  plan.pattern = FlowPattern::kFullMesh;
  plan.direction = FlowDirection::kUnidirectional;
  plan.plan_sequence = Sequence(1);
  for (const ParticipantId& source : participants) {
    for (const ParticipantId& destination : participants) {
      if (source == destination) continue;
      FlowEdge edge;
      edge.source = source;
      edge.destination = destination;
      edge.logical_bytes = bytes_per_edge;
      edge.step_index = Sequence(1);
      edge.hint = AlgorithmHint::kDirect;
      plan.edges.push_back(edge);
    }
  }
  return plan;
}

// Tree (gather toward the first participant, then broadcast back).
[[nodiscard]] inline FlowGroupPlan make_tree_plan(const CollectiveInstance& instance,
                                                  const std::vector<ParticipantId>& participants,
                                                  std::uint64_t bytes_per_edge) {
  FlowGroupPlan plan;
  plan.instance = instance;
  plan.pattern = FlowPattern::kTreeUp;
  plan.direction = FlowDirection::kBidirectional;
  plan.plan_sequence = Sequence(1);
  for (std::size_t index = 1; index < participants.size(); ++index) {
    const std::size_t parent = (index - 1) / 2;
    FlowEdge up;
    up.source = participants[index];
    up.destination = participants[parent];
    up.logical_bytes = bytes_per_edge;
    up.step_index = Sequence(1);
    up.hint = AlgorithmHint::kTree;
    plan.edges.push_back(up);
    FlowEdge down;
    down.source = participants[parent];
    down.destination = participants[index];
    down.logical_bytes = bytes_per_edge;
    down.step_index = Sequence(2);
    down.hint = AlgorithmHint::kTree;
    plan.edges.push_back(down);
  }
  return plan;
}

// A snapshot in which everything is current: policy, topology, capacity,
// congestion and every participant live.  Tests start from this and then break
// exactly one thing, which keeps each proof honest about what it demonstrates.
struct SyntheticWorld {
  IdFactory ids;
  RackFabric fabric;
  TrafficPolicy policy;
  CapacityEvidence capacity;
  CongestionEvidence congestion;
  std::vector<ParticipantId> participants;
  std::vector<ParticipantId> nodes;
  EvaluationSnapshot snapshot;

  explicit SyntheticWorld(std::uint64_t seed, std::uint32_t rack_count = 2,
                          std::uint32_t nodes_per_rack = 2, std::uint32_t utilization_bps = 1000)
      : ids(seed),
        fabric(make_fabric(seed, rack_count, nodes_per_rack)),
        capacity(make_capacity(fabric.topology, seed + 1, utilization_bps)),
        congestion(make_congestion(fabric.topology, seed + 2, utilization_bps)) {
    policy = TrafficPolicy::standard(ids.next());
    fabric.topology.captured_monotonic_ms = 0;
    capacity.captured_monotonic_ms = 0;
    congestion.captured_monotonic_ms = 0;
    for (std::size_t index = 0; index < fabric.nodes.size(); ++index) {
      const ParticipantId participant = ids.next();
      participants.push_back(participant);
      PeerLiveness liveness;
      liveness.participant = participant;
      liveness.incarnation = ids.next();
      liveness.state = ParticipantState::kLive;
      liveness.node = fabric.nodes[index];
      liveness.last_seen_monotonic_ms = 0;
      liveness.expires_at_monotonic_ms = 100000;
      snapshot.peers.push_back(liveness);
    }
    std::sort(snapshot.peers.begin(), snapshot.peers.end(),
              [](const PeerLiveness& a, const PeerLiveness& b) { return a.participant < b.participant; });
    rebuild();
  }

  // Rebuilds the frozen snapshot from the current world state.  Called after a
  // test mutates something so the change is visible to the engine.
  void rebuild(std::uint64_t now_monotonic_ms = 100) {
    if (epoch.is_none()) epoch = ids.next();
    snapshot.epoch = epoch;
    snapshot.policy_generation = policy.generation;
    snapshot.topology_generation = fabric.topology.generation;
    snapshot.capacity_generation = capacity.generation;
    snapshot.congestion_generation = congestion.generation;
    snapshot.topology = std::make_shared<const TopologyEvidence>(fabric.topology);
    snapshot.capacity = std::make_shared<const CapacityEvidence>(capacity);
    snapshot.congestion = std::make_shared<const CongestionEvidence>(congestion);
    snapshot.policy = std::make_shared<const TrafficPolicy>(policy);
    snapshot.now_monotonic_ms = now_monotonic_ms;
    snapshot.capacity_observation_available = !capacity.links.empty();
    snapshot.congestion_observation_available = !congestion.links.empty();
    snapshot.topology_available = true;
    snapshot.policy_available = true;
  }

  // The coordinator epoch this synthetic world presents.  Assigned once and
  // reused by every rebuild so that a rebuild alone never stales a request.
  Identity epoch{};
};

[[nodiscard]] inline DecisionRequest make_request(const CollectiveInstance& instance,
                                                  const EvaluationSnapshot& snapshot,
                                                  const FlowGroupPlan& plan) {
  DecisionRequest request;
  request.instance = instance;
  request.observed_epoch = snapshot.epoch;
  request.observed_policy_generation = snapshot.policy_generation;
  request.observed_topology_generation = snapshot.topology_generation;
  request.observed_capacity_generation = snapshot.capacity_generation;
  request.observed_congestion_generation = snapshot.congestion_generation;
  request.has_flow_group_plan = true;
  request.plan = plan;
  return request;
}

}  // namespace ctf::test

#endif  // CTF_TESTS_SYNTHETIC_HPP
