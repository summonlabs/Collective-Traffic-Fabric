// Collective Traffic Fabric - freshness policy and the frozen evaluation
// snapshot handed to the decision engine.
// Copyright 2026 Summon Software Labs.
//
// This header exists separately from evidence.hpp so that the include graph
// stays acyclic: evidence describes observations, policy describes treatment,
// and the snapshot is the single frozen view of both that the engine consumes.
#ifndef CTF_SNAPSHOT_HPP
#define CTF_SNAPSHOT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ctf/evidence.hpp"
#include "ctf/identity.hpp"
#include "ctf/participant.hpp"
#include "ctf/traffic.hpp"

namespace ctf {

// ---------------------------------------------------------------------------
// Freshness policy.  Each evidence kind declares how old it may be before it
// stops supporting a decision, and whether absence is tolerable when the
// collective genuinely has no flows on that kind of link.
// ---------------------------------------------------------------------------
enum class EvidenceKind : std::uint8_t {
  kTopology = 0,
  kCapacity = 1,
  kCongestion = 2,
  kPeerLiveness = 3,
};

[[nodiscard]] std::string_view to_string(EvidenceKind value) noexcept;

struct FreshnessPolicy {
  std::uint64_t topology_max_age_ms = 60000;
  std::uint64_t capacity_max_age_ms = 5000;
  std::uint64_t congestion_max_age_ms = 2000;
  std::uint64_t peer_liveness_max_age_ms = 10000;
  // When true, evidence older than the bound is reported as stale and the
  // decision is withheld; when false the evidence is still refused but the
  // reason chain records a revalidation requirement instead.
  bool stale_requires_revalidation = true;

  [[nodiscard]] std::uint64_t max_age_ms(EvidenceKind kind) const noexcept;
};

// A complete, internally consistent view of everything the decision engine may
// consult.  The engine is a pure function of this snapshot plus its request:
// it holds no locks, reads no clock, and touches no global state.
struct EvaluationSnapshot {
  CoordinatorEpoch epoch{};
  PolicyGeneration policy_generation{};
  TopologyGeneration topology_generation{};
  EvidenceGeneration capacity_generation{};
  EvidenceGeneration congestion_generation{};

  // Owned copies.  A snapshot is a frozen view: nothing another thread does
  // afterwards can change what this snapshot says, so the engine can evaluate
  // it without holding any lock.
  std::shared_ptr<const TopologyEvidence> topology;
  std::shared_ptr<const CapacityEvidence> capacity;
  std::shared_ptr<const CongestionEvidence> congestion;
  std::shared_ptr<const TrafficPolicy> policy;

  std::vector<PeerLiveness> peers;   // canonical ascending by participant id
  FreshnessPolicy freshness{};
  std::uint64_t now_monotonic_ms = 0;
  bool capacity_observation_available = false;
  bool congestion_observation_available = false;
  bool topology_available = false;
  bool policy_available = false;

  [[nodiscard]] const PeerLiveness* find_peer(ParticipantId id) const noexcept;
};

}  // namespace ctf

#endif  // CTF_SNAPSHOT_HPP
