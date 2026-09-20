// Collective Traffic Fabric - coordinator authority bookkeeping.
// Copyright 2026 Summon Software Labs.
#include "ctf/authority.hpp"

#include <algorithm>
#include <memory>

#include "ctf/traffic.hpp"

namespace ctf {

std::string_view to_string(InstallResult value) noexcept {
  switch (value) {
    case InstallResult::kInstalled: return "installed";
    case InstallResult::kRejectedOlderGeneration: return "rejected_older_generation";
    case InstallResult::kRejectedInvalid: return "rejected_invalid";
    case InstallResult::kRejectedOversized: return "rejected_oversized";
    case InstallResult::kRejectedSameGeneration: return "rejected_same_generation";
  }
  return "rejected_invalid";
}

// Bounded record of the generations already accepted on one axis.  Bounded on
// purpose: the guarantee needed is that a replay of a recently superseded
// generation is refused, not an unbounded history of every generation ever seen.
class GenerationHistory {
 public:
  void remember(Identity generation) {
    if (generation.is_none()) return;
    seen_.push_back(generation);
    if (seen_.size() > kCapacity) {
      seen_.erase(seen_.begin());
    }
  }
  [[nodiscard]] bool contains(Identity generation) const {
    for (const Identity& entry : seen_) {
      if (entry == generation) return true;
    }
    return false;
  }
  void clear() { seen_.clear(); }

 private:
  static constexpr std::size_t kCapacity = 64;
  std::vector<Identity> seen_;
};

struct AuthorityState::Impl {
  mutable std::mutex mutex;
  CoordinatorEpoch epoch{};
  CoordinatorEpoch last_durable_epoch{};
  PolicyGeneration policy_generation{};
  TopologyGeneration topology_generation{};
  EvidenceGeneration capacity_generation{};
  EvidenceGeneration congestion_generation{};

  std::shared_ptr<const TrafficPolicy> policy;
  std::shared_ptr<const TopologyEvidence> topology;
  std::shared_ptr<const CapacityEvidence> capacity;
  std::shared_ptr<const CongestionEvidence> congestion;
  GenerationHistory policy_history;
  GenerationHistory topology_history;
  GenerationHistory capacity_history;
  GenerationHistory congestion_history;

  // Liveness is keyed by participant.  An entry is authoritative only while its
  // incarnation is the highest seen and its expiry is in the future.
  IdentityMap<PeerLiveness> peers;
  IdentityMap<BootIncarnation> highest_incarnation;
  FreshnessPolicy freshness{};
  std::uint64_t boot_monotonic_ms = 0;
};

AuthorityState::AuthorityState() : impl_(std::make_unique<Impl>()) {}
AuthorityState::~AuthorityState() = default;

CoordinatorEpoch AuthorityState::begin_boot(CoordinatorEpoch last_durable_epoch) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->last_durable_epoch = last_durable_epoch;
  // The epoch advances on every boot, even when the durable state is identical.
  // This is the mechanism that makes a pre-restart decision stale.
  CoordinatorEpoch candidate = mint_identity();
  while (candidate == last_durable_epoch || candidate == impl_->epoch) {
    candidate = mint_identity();
  }
  impl_->epoch = candidate;
  // Nothing dynamic survives a boot: liveness is cleared here rather than at
  // the call site so that no caller can forget.
  impl_->peers.clear();
  impl_->highest_incarnation.clear();
  impl_->capacity.reset();
  impl_->congestion.reset();
  impl_->capacity_generation = EvidenceGeneration{};
  impl_->congestion_generation = EvidenceGeneration{};
  return impl_->epoch;
}

CoordinatorEpoch AuthorityState::epoch() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->epoch;
}

bool AuthorityState::epoch_is_current(CoordinatorEpoch candidate) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return candidate.is_some() && candidate == impl_->epoch;
}

InstallResult AuthorityState::install_policy(TrafficPolicy policy) {
  Status status = policy.validate();
  if (!status.is_ok()) return InstallResult::kRejectedInvalid;
  if (policy.generation.is_none()) return InstallResult::kRejectedInvalid;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->policy != nullptr && impl_->policy_generation == policy.generation) {
    // An equal generation would make two different policies indistinguishable to
    // every stored decision, so it is refused.
    return InstallResult::kRejectedSameGeneration;
  }
  if (impl_->policy_history.contains(policy.generation)) {
    return InstallResult::kRejectedOlderGeneration;
  }
  impl_->policy_history.remember(policy.generation);
  impl_->policy_generation = policy.generation;
  impl_->policy = std::make_shared<const TrafficPolicy>(std::move(policy));
  return InstallResult::kInstalled;
}

PolicyGeneration AuthorityState::policy_generation() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->policy_generation;
}

TrafficPolicy AuthorityState::policy_snapshot() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->policy == nullptr) return TrafficPolicy{};
  return *impl_->policy;
}

InstallResult AuthorityState::install_topology(TopologyEvidence topology) {
  topology.canonicalize();
  Status status = topology.validate();
  if (!status.is_ok()) return InstallResult::kRejectedInvalid;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->topology != nullptr && impl_->topology_generation == topology.generation) {
    return InstallResult::kRejectedSameGeneration;
  }
  if (impl_->topology_history.contains(topology.generation)) {
    return InstallResult::kRejectedOlderGeneration;
  }
  impl_->topology_history.remember(topology.generation);
  impl_->topology_generation = topology.generation;
  impl_->topology = std::make_shared<const TopologyEvidence>(std::move(topology));
  // Capacity and congestion observations describe the previous topology; they
  // are dropped rather than reinterpreted against a topology they never saw.
  impl_->capacity.reset();
  impl_->congestion.reset();
  impl_->capacity_generation = EvidenceGeneration{};
  impl_->congestion_generation = EvidenceGeneration{};
  return InstallResult::kInstalled;
}

TopologyGeneration AuthorityState::topology_generation() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->topology_generation;
}

TopologyEvidence AuthorityState::topology_snapshot() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->topology == nullptr) return TopologyEvidence{};
  return *impl_->topology;
}

InstallResult AuthorityState::install_capacity(CapacityEvidence capacity) {
  capacity.canonicalize();
  Status status = capacity.validate();
  if (!status.is_ok()) return InstallResult::kRejectedInvalid;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->topology == nullptr || capacity.topology_generation != impl_->topology_generation) {
    // Capacity measured against a topology that is not current is not evidence
    // about this fabric; it is refused rather than stored.
    return InstallResult::kRejectedInvalid;
  }
  if (impl_->capacity != nullptr && impl_->capacity_generation == capacity.generation) {
    return InstallResult::kRejectedSameGeneration;
  }
  if (impl_->capacity_history.contains(capacity.generation)) {
    return InstallResult::kRejectedOlderGeneration;
  }
  impl_->capacity_history.remember(capacity.generation);
  impl_->capacity_generation = capacity.generation;
  impl_->capacity = std::make_shared<const CapacityEvidence>(std::move(capacity));
  return InstallResult::kInstalled;
}

EvidenceGeneration AuthorityState::capacity_generation() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->capacity_generation;
}

CapacityEvidence AuthorityState::capacity_snapshot() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->capacity == nullptr) return CapacityEvidence{};
  return *impl_->capacity;
}

bool AuthorityState::capacity_available() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->capacity != nullptr;
}

InstallResult AuthorityState::install_congestion(CongestionEvidence congestion) {
  congestion.canonicalize();
  Status status = congestion.validate();
  if (!status.is_ok()) return InstallResult::kRejectedInvalid;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->topology == nullptr || congestion.topology_generation != impl_->topology_generation) {
    return InstallResult::kRejectedInvalid;
  }
  if (impl_->congestion != nullptr && impl_->congestion_generation == congestion.generation) {
    return InstallResult::kRejectedSameGeneration;
  }
  if (impl_->congestion_history.contains(congestion.generation)) {
    return InstallResult::kRejectedOlderGeneration;
  }
  impl_->congestion_history.remember(congestion.generation);
  impl_->congestion_generation = congestion.generation;
  impl_->congestion = std::make_shared<const CongestionEvidence>(std::move(congestion));
  return InstallResult::kInstalled;
}

EvidenceGeneration AuthorityState::congestion_generation() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->congestion_generation;
}

CongestionEvidence AuthorityState::congestion_snapshot() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->congestion == nullptr) return CongestionEvidence{};
  return *impl_->congestion;
}

bool AuthorityState::congestion_available() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->congestion != nullptr;
}

void AuthorityState::invalidate_dynamic_evidence() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->capacity.reset();
  impl_->congestion.reset();
  impl_->capacity_generation = EvidenceGeneration{};
  impl_->congestion_generation = EvidenceGeneration{};
}

AuthorityState::PublishResult AuthorityState::publish_liveness(const PeerLiveness& liveness) {
  if (liveness.participant.is_none() || liveness.incarnation.is_none()) {
    return PublishResult::kRejectedInvalid;
  }
  if (liveness.state == ParticipantState::kUnspecified) {
    return PublishResult::kRejectedInvalid;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto highest = impl_->highest_incarnation.find(liveness.participant);
  if (highest != impl_->highest_incarnation.end()) {
    if (liveness.incarnation < highest->second) {
      // A strictly older incarnation is fenced: the boot that produced it is
      // gone, and nothing it says may be believed again.
      return PublishResult::kFencedStaleIncarnation;
    }
    if (liveness.incarnation == highest->second) {
      // The same boot may refresh its own liveness only while that boot is still
      // the current one.  Once the record has been fenced, expired or otherwise
      // stopped being live, republishing the same incarnation is an attempt to
      // resurrect a boot that has ended, and it is refused permanently: only a
      // strictly greater incarnation, which is what a real reboot produces, wins.
      auto stored = impl_->peers.find(liveness.participant);
      if (stored == impl_->peers.end() || stored->second.state == ParticipantState::kLive ||
          stored->second.state == ParticipantState::kSuspect ||
          stored->second.state == ParticipantState::kAdvertised) {
        impl_->peers[liveness.participant] = liveness;
        return PublishResult::kAccepted;
      }
      return PublishResult::kFencedStaleIncarnation;
    }
  }
  if (impl_->peers.size() >= limits::kPeersMax) {
    auto existing = impl_->peers.find(liveness.participant);
    if (existing == impl_->peers.end()) {
      return PublishResult::kRejectedCapacity;
    }
  }
  impl_->highest_incarnation[liveness.participant] = liveness.incarnation;
  impl_->peers[liveness.participant] = liveness;
  return PublishResult::kAccepted;
}

bool AuthorityState::heartbeat(ParticipantId participant, BootIncarnation incarnation,
                              std::uint64_t now_monotonic_ms, std::uint64_t ttl_ms) {
  if (participant.is_none() || incarnation.is_none()) return false;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto highest = impl_->highest_incarnation.find(participant);
  if (highest != impl_->highest_incarnation.end() && incarnation < highest->second) {
    return false;
  }
  if (highest != impl_->highest_incarnation.end() && incarnation == highest->second) {
    // A heartbeat reasserts an existing boot, so it is only accepted while that
    // boot is still current.  After fencing or expiry the incarnation is spent.
    auto stored = impl_->peers.find(participant);
    if (stored == impl_->peers.end() || stored->second.state == ParticipantState::kFenced ||
        stored->second.state == ParticipantState::kExpired ||
        stored->second.state == ParticipantState::kRetired) {
      return false;
    }
  }
  impl_->highest_incarnation[participant] = incarnation;
  PeerLiveness& stored = impl_->peers[participant];
  const bool same_boot = stored.incarnation == incarnation;
  stored.participant = participant;
  stored.incarnation = incarnation;
  stored.state = ParticipantState::kLive;
  stored.last_seen_monotonic_ms = now_monotonic_ms;
  stored.expires_at_monotonic_ms = now_monotonic_ms + ttl_ms;
  if (!same_boot) {
    // A new boot incarnation means the previous one is fenced.  Any node or
    // session binding from the previous boot is discarded rather than carried.
    stored.node = NodeId{};
    stored.bound_session = SessionId{};
  }
  return true;
}

std::size_t AuthorityState::expire_session(SessionId session, std::uint64_t now_monotonic_ms) {
  std::size_t affected = 0;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  for (auto& entry : impl_->peers) {
    if (entry.second.bound_session == session && session.is_some()) {
      // The session that published this participant is gone.  The incarnation it
      // used is fenced, not merely expired: an expired record can be revived by
      // republishing the same incarnation, and a boot that has ended must not be
      // able to come back.  Fencing is what makes an abrupt process death a
      // durable fact rather than an observation with a short half life.
      entry.second.state = ParticipantState::kFenced;
      entry.second.expires_at_monotonic_ms = now_monotonic_ms;
      ++affected;
    }
  }
  return affected;
}

std::vector<ParticipantId> AuthorityState::expire_due(std::uint64_t now_monotonic_ms) {
  std::vector<ParticipantId> fenced;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  for (auto& entry : impl_->peers) {
    PeerLiveness& liveness = entry.second;
    if (liveness.state == ParticipantState::kLive && now_monotonic_ms >= liveness.expires_at_monotonic_ms) {
      // The same reasoning as session teardown: a participant whose liveness
      // evidence ran out has stopped being current, and the incarnation it last
      // published must not be reusable.  A strictly greater incarnation, which
      // is what a real reboot produces, is still accepted.
      liveness.state = ParticipantState::kFenced;
      liveness.expires_at_monotonic_ms = now_monotonic_ms;
      fenced.push_back(liveness.participant);
    }
  }
  std::sort(fenced.begin(), fenced.end());
  return fenced;
}

PeerLiveness AuthorityState::peer_snapshot(ParticipantId participant) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto existing = impl_->peers.find(participant);
  if (existing == impl_->peers.end()) return PeerLiveness{};
  return existing->second;
}

std::vector<PeerLiveness> AuthorityState::peers_snapshot() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<PeerLiveness> out;
  out.reserve(impl_->peers.size());
  for (const auto& entry : impl_->peers) out.push_back(entry.second);
  std::sort(out.begin(), out.end(), [](const PeerLiveness& a, const PeerLiveness& b) {
    return a.participant < b.participant;
  });
  return out;
}

std::size_t AuthorityState::peer_count() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->peers.size();
}

std::size_t AuthorityState::live_peer_count(std::uint64_t now_monotonic_ms) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  std::size_t live = 0;
  for (const auto& entry : impl_->peers) {
    if (entry.second.is_current(now_monotonic_ms)) ++live;
  }
  return live;
}

void AuthorityState::set_freshness_policy(FreshnessPolicy policy) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->freshness = policy;
}

FreshnessPolicy AuthorityState::freshness_policy() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->freshness;
}

DurableAuthority AuthorityState::durable_snapshot() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  DurableAuthority durable;
  durable.last_epoch = impl_->epoch;
  durable.policy_generation = impl_->policy_generation;
  durable.topology_generation = impl_->topology_generation;
  if (impl_->topology != nullptr) durable.topology = *impl_->topology;
  if (impl_->policy != nullptr) durable.policy = *impl_->policy;
  return durable;
}

Status AuthorityState::restore_durable(const DurableAuthority& durable, std::uint64_t now_monotonic_ms) {
  if (durable.topology_generation.is_some()) {
    Status status = durable.topology.validate();
    if (!status.is_ok()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    std::string("durable topology is not usable: ") + status.message());
    }
    if (durable.topology.generation != durable.topology_generation) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    "durable topology generation disagrees with the durable record");
    }
  }
  if (durable.policy_generation.is_some()) {
    Status status = durable.policy.validate();
    if (!status.is_ok()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    std::string("durable policy is not usable: ") + status.message());
    }
    if (durable.policy.generation != durable.policy_generation) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    "durable policy generation disagrees with the durable record");
    }
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->last_durable_epoch = durable.last_epoch;
  if (durable.topology_generation.is_some()) {
    impl_->topology_generation = durable.topology_generation;
    impl_->topology = std::make_shared<const TopologyEvidence>(durable.topology);
  }
  if (durable.policy_generation.is_some()) {
    impl_->policy_generation = durable.policy_generation;
    impl_->policy = std::make_shared<const TrafficPolicy>(durable.policy);
  }
  // Dynamic state is cleared on restore, always.  A restart must not resurrect
  // capacity, congestion or liveness, even when the caller asks for a restore
  // of state that was persisted moments earlier.
  impl_->capacity.reset();
  impl_->congestion.reset();
  impl_->capacity_generation = EvidenceGeneration{};
  impl_->congestion_generation = EvidenceGeneration{};
  impl_->peers.clear();
  impl_->highest_incarnation.clear();
  impl_->boot_monotonic_ms = now_monotonic_ms;
  return Status::ok();
}

EvaluationSnapshot AuthorityState::snapshot(std::uint64_t now_monotonic_ms) const {
  EvaluationSnapshot out;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    out.epoch = impl_->epoch;
    out.policy_generation = impl_->policy_generation;
    out.topology_generation = impl_->topology_generation;
    out.capacity_generation = impl_->capacity_generation;
    out.congestion_generation = impl_->congestion_generation;
    out.topology = impl_->topology;
    out.capacity = impl_->capacity;
    out.congestion = impl_->congestion;
    out.policy = impl_->policy;
    out.peers.reserve(impl_->peers.size());
    for (const auto& entry : impl_->peers) out.peers.push_back(entry.second);
    out.freshness = impl_->freshness;
    out.topology_available = impl_->topology != nullptr;
    out.policy_available = impl_->policy != nullptr;
    out.capacity_observation_available = impl_->capacity != nullptr;
    out.congestion_observation_available = impl_->congestion != nullptr;
  }
  std::sort(out.peers.begin(), out.peers.end(),
            [](const PeerLiveness& a, const PeerLiveness& b) { return a.participant < b.participant; });
  out.now_monotonic_ms = now_monotonic_ms;
  return out;
}

}  // namespace ctf
