// Collective Traffic Fabric - downstream consumer of the installed package.
// Copyright 2026 Summon Software Labs.
//
// This program links only against the installed library and the installed
// headers.  It exercises a real supported path and checks a real invariant: an
// admission is bound to the generation set it was evaluated against.  If the
// installed package were missing a header, a symbol or the exported target, this
// file would not build or run.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ctf/collective.hpp"
#include "ctf/decision.hpp"
#include "ctf/engine.hpp"
#include "ctf/evidence.hpp"
#include "ctf/flow_group.hpp"
#include "ctf/identity.hpp"
#include "ctf/snapshot.hpp"
#include "ctf/traffic.hpp"

namespace {

constexpr std::uint64_t kNowMs = 1000;
constexpr std::uint64_t kExpiryMs = 100000;

// A two participant, single rack fabric.  A deployment would feed this from a
// fabric observer; the consumer builds it directly so the program is
// self-contained and deterministic.  It is SYNTHETIC evidence.
struct World {
  std::shared_ptr<ctf::TopologyEvidence> topology;
  std::shared_ptr<ctf::CapacityEvidence> capacity;
  std::shared_ptr<ctf::CongestionEvidence> congestion;
  std::shared_ptr<ctf::TrafficPolicy> policy;
  std::vector<ctf::PeerLiveness> peers;
  ctf::EvaluationSnapshot snapshot;
};

World build_world() {
  World world;
  const ctf::TopologyGeneration topology_generation = ctf::mint_identity();
  world.topology = std::make_shared<ctf::TopologyEvidence>();
  world.topology->generation = topology_generation;
  world.topology->captured_monotonic_ms = 0;
  world.topology->synthetic = true;
  const ctf::NodeId node = ctf::mint_identity();
  ctf::TopologyNode node_entry;
  node_entry.id = node;
  node_entry.rack = ctf::mint_identity();
  node_entry.accelerator_attached = true;
  node_entry.accelerator_count = 8;
  world.topology->nodes.push_back(node_entry);
  ctf::TopologyLink link;
  link.id = ctf::mint_identity();
  link.a = ctf::mint_identity();
  link.b = ctf::mint_identity();
  link.link_class = ctf::LinkClass::kInterRack;
  link.capacity_bps = 400ull * 1000 * 1000 * 1000;
  link.lanes = 4;
  world.topology->links.push_back(link);
  world.topology->canonicalize();

  world.capacity = std::make_shared<ctf::CapacityEvidence>();
  world.capacity->generation = ctf::mint_identity();
  world.capacity->topology_generation = topology_generation;
  world.capacity->captured_monotonic_ms = 0;
  world.capacity->synthetic = true;
  ctf::LinkCapacity capacity_entry;
  capacity_entry.link = link.id;
  capacity_entry.provisioned_bps = link.capacity_bps;
  capacity_entry.available_bps = link.capacity_bps / 2;
  capacity_entry.utilization_bps = 1000;
  world.capacity->links.push_back(capacity_entry);
  world.capacity->canonicalize();

  world.congestion = std::make_shared<ctf::CongestionEvidence>();
  world.congestion->generation = ctf::mint_identity();
  world.congestion->topology_generation = topology_generation;
  world.congestion->captured_monotonic_ms = 0;
  world.congestion->valid_for_ms = 5000;
  world.congestion->synthetic = true;
  ctf::LinkCongestion congestion_entry;
  congestion_entry.link = link.id;
  congestion_entry.utilization_bps = 2000;
  congestion_entry.level = ctf::congestion_level_from_utilization_bps(2000);
  world.congestion->links.push_back(congestion_entry);
  world.congestion->canonicalize();

  world.policy = std::make_shared<ctf::TrafficPolicy>(ctf::TrafficPolicy::standard(ctf::mint_identity()));

  // Two live participants on the same node, both current.
  const ctf::ParticipantId first = ctf::mint_identity();
  const ctf::ParticipantId second = ctf::mint_identity();
  for (const ctf::ParticipantId participant : {first, second}) {
    ctf::PeerLiveness liveness;
    liveness.participant = participant;
    liveness.incarnation = ctf::mint_identity();
    liveness.state = ctf::ParticipantState::kLive;
    liveness.node = node;
    liveness.expires_at_monotonic_ms = kExpiryMs;
    world.peers.push_back(liveness);
  }
  std::sort(world.peers.begin(), world.peers.end(),
            [](const ctf::PeerLiveness& a, const ctf::PeerLiveness& b) {
              return a.participant < b.participant;
            });

  world.snapshot.epoch = ctf::mint_identity();
  world.snapshot.policy_generation = world.policy->generation;
  world.snapshot.topology_generation = topology_generation;
  world.snapshot.capacity_generation = world.capacity->generation;
  world.snapshot.congestion_generation = world.congestion->generation;
  world.snapshot.topology = world.topology;
  world.snapshot.capacity = world.capacity;
  world.snapshot.congestion = world.congestion;
  world.snapshot.policy = world.policy;
  world.snapshot.peers = world.peers;
  world.snapshot.topology_available = true;
  world.snapshot.policy_available = true;
  world.snapshot.capacity_observation_available = true;
  world.snapshot.congestion_observation_available = true;
  world.snapshot.freshness.congestion_max_age_ms = 5000;
  world.snapshot.now_monotonic_ms = kNowMs;
  return world;
}

}  // namespace

int main() {
  const World world = build_world();

  ctf::CollectiveDefinition definition;
  definition.id = ctf::mint_identity();
  definition.generation = ctf::mint_identity();
  definition.collective_class = ctf::CollectiveClass::kAllReduce;
  definition.label = "consumer-all-reduce";
  definition.logical_bytes = 1ull << 20;
  definition.participants = {world.peers[0].participant, world.peers[1].participant};
  ctf::canonicalize_participants(definition.participants);

  ctf::CollectiveInstance instance;
  instance.id = definition.id;
  instance.generation = definition.generation;
  instance.attempt = ctf::mint_identity();

  ctf::FlowGroupPlan plan;
  plan.instance = instance;
  plan.collective_class = definition.collective_class;
  plan.pattern = ctf::FlowPattern::kChain;
  plan.direction = ctf::FlowDirection::kUnidirectional;
  ctf::FlowEdge forward;
  forward.source = definition.participants[0];
  forward.destination = definition.participants[1];
  forward.logical_bytes = 1ull << 20;
  forward.step_index = ctf::Sequence(1);
  ctf::FlowEdge backward = forward;
  backward.source = definition.participants[1];
  backward.destination = definition.participants[0];
  backward.step_index = ctf::Sequence(2);
  plan.edges.push_back(forward);
  plan.edges.push_back(backward);

  ctf::DecisionRequest request;
  request.instance = instance;
  request.observed_epoch = world.snapshot.epoch;
  request.observed_policy_generation = world.snapshot.policy_generation;
  request.observed_topology_generation = world.snapshot.topology_generation;
  request.observed_capacity_generation = world.snapshot.capacity_generation;
  request.observed_congestion_generation = world.snapshot.congestion_generation;
  request.has_flow_group_plan = true;
  request.plan = plan;

  const ctf::DecisionRecord decision = ctf::evaluate(world.snapshot, request, definition);
  std::printf("outcome: %s\n", std::string(ctf::to_string(decision.outcome)).c_str());
  std::printf("traffic_class: %s\n", std::string(ctf::to_string(decision.traffic_class)).c_str());
  std::printf("flow_group: %s\n", decision.flow_group.to_string().c_str());
  std::printf("bound epoch: %s\n", decision.epoch.to_string().c_str());
  std::printf("fingerprint: %llu\n", static_cast<unsigned long long>(decision.fingerprint()));
  std::printf("%s", decision.explain().c_str());

  if (!ctf::outcome_grants_authority(decision.outcome)) {
    std::fprintf(stderr, "consumer: expected an admitting decision\n");
    return 1;
  }
  // The invariant a consumer actually depends on: the decision is bound to the
  // generations it was evaluated against, not to something ambient.
  if (decision.epoch != world.snapshot.epoch ||
      decision.topology_generation != world.snapshot.topology_generation ||
      decision.policy_generation != world.snapshot.policy_generation ||
      decision.collective_generation != definition.generation) {
    std::fprintf(stderr, "consumer: the decision was not bound to the snapshot\n");
    return 1;
  }
  // Identical inputs must produce an identical decision, across evaluations.
  const ctf::DecisionRecord again = ctf::evaluate(world.snapshot, request, definition);
  if (again.fingerprint() != decision.fingerprint()) {
    std::fprintf(stderr, "consumer: the decision was not reproducible\n");
    return 1;
  }
  std::printf("consumer: OK\n");
  return 0;
}
