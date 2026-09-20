// Collective Traffic Fabric - coordinator authority: epoch, generations,
// liveness and the fencing rules that decide which evidence is current.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_AUTHORITY_HPP
#define CTF_AUTHORITY_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ctf/decision.hpp"
#include "ctf/error.hpp"
#include "ctf/evidence.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"
#include "ctf/participant.hpp"
#include "ctf/snapshot.hpp"
#include "ctf/traffic.hpp"

namespace ctf {

// Only durable, non-liveness state may cross a coordinator restart through
// this struct.  Liveness, capacity and congestion evidence are deliberately
// absent: they must be re-established by fresh observation.
struct DurableAuthority {
  CoordinatorEpoch last_epoch{};
  PolicyGeneration policy_generation{};
  TopologyGeneration topology_generation{};
  TopologyEvidence topology{};        // topology is durable description, not liveness
  TrafficPolicy policy{};
  std::uint64_t restart_count = 0;
};

enum class InstallResult : std::uint8_t {
  kInstalled = 0,
  // The generation is one this coordinator has already accepted on this axis.
  // Replaying it is refused so that an observation can never be reinstalled
  // after a newer one replaced it.
  kRejectedOlderGeneration = 1,
  kRejectedInvalid = 2,
  kRejectedOversized = 3,
  // The generation equals the one currently installed, which would make two
  // different observations indistinguishable.
  kRejectedSameGeneration = 4,
};

[[nodiscard]] std::string_view to_string(InstallResult value) noexcept;

// Thread safe authority bookkeeping.  All public methods are self contained:
// none of them calls back into user code and none of them holds a lock while
// invoking another authority method, so no lock ordering hazard exists.  The
// snapshot method releases its lock before returning to the caller.
class AuthorityState {
 public:
  AuthorityState();
  ~AuthorityState();
  AuthorityState(const AuthorityState&) = delete;
  AuthorityState& operator=(const AuthorityState&) = delete;

  // ---- epoch and generation ------------------------------------------------
  // Advances the epoch.  Called on every coordinator boot; the durable epoch is
  // never reused even when the persisted state is the same.
  CoordinatorEpoch begin_boot(CoordinatorEpoch last_durable_epoch);
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] bool epoch_is_current(CoordinatorEpoch candidate) const;

  InstallResult install_policy(TrafficPolicy policy);
  [[nodiscard]] PolicyGeneration policy_generation() const;
  [[nodiscard]] TrafficPolicy policy_snapshot() const;

  InstallResult install_topology(TopologyEvidence topology);
  [[nodiscard]] TopologyGeneration topology_generation() const;
  [[nodiscard]] TopologyEvidence topology_snapshot() const;

  InstallResult install_capacity(CapacityEvidence capacity);
  [[nodiscard]] EvidenceGeneration capacity_generation() const;
  [[nodiscard]] CapacityEvidence capacity_snapshot() const;
  [[nodiscard]] bool capacity_available() const;

  InstallResult install_congestion(CongestionEvidence congestion);
  [[nodiscard]] EvidenceGeneration congestion_generation() const;
  [[nodiscard]] CongestionEvidence congestion_snapshot() const;
  [[nodiscard]] bool congestion_available() const;

  // Drops dynamic evidence.  Used on shutdown and by tests that prove a
  // restart cannot resurrect freshness.
  void invalidate_dynamic_evidence();

  // ---- participants --------------------------------------------------------
  // Publication is authoritative: it either installs a strictly newer
  // incarnation or reports that the caller is fenced.
  enum class PublishResult : std::uint8_t { kAccepted = 0, kFencedStaleIncarnation = 1, kRejectedCapacity = 2, kRejectedInvalid = 3 };
  PublishResult publish_liveness(const PeerLiveness& liveness);
  [[nodiscard]] bool heartbeat(ParticipantId participant, BootIncarnation incarnation,
                              std::uint64_t now_monotonic_ms, std::uint64_t ttl_ms);
  // Marks every participant bound to a session as FENCED.  Called when a
  // publisher process dies, times out, or its session is torn down.  Fencing,
  // rather than merely expiring, is deliberate: the incarnation that the dead
  // boot published can never be republished by anyone, while a strictly greater
  // incarnation - which is what a reboot produces - is still accepted.
  std::size_t expire_session(SessionId session, std::uint64_t now_monotonic_ms);
  // Applies time based expiry and fences what it expires.  Returns the
  // identities fenced by this call.
  std::vector<ParticipantId> expire_due(std::uint64_t now_monotonic_ms);
  [[nodiscard]] PeerLiveness peer_snapshot(ParticipantId participant) const;
  [[nodiscard]] std::vector<PeerLiveness> peers_snapshot() const;
  // Number of recorded participants, whatever their state.
  [[nodiscard]] std::size_t peer_count() const;
  // Number of participants that currently hold authority: live and not expired
  // as of the supplied monotonic clock.
  [[nodiscard]] std::size_t live_peer_count(std::uint64_t now_monotonic_ms) const;

  void set_freshness_policy(FreshnessPolicy policy);
  [[nodiscard]] FreshnessPolicy freshness_policy() const;

  DurableAuthority durable_snapshot() const;
  // Restores durable state only.  Dynamic evidence is cleared and the epoch is
  // advanced, so nothing that was live before a restart is live afterwards.
  Status restore_durable(const DurableAuthority& durable, std::uint64_t now_monotonic_ms);

  [[nodiscard]] EvaluationSnapshot snapshot(std::uint64_t now_monotonic_ms) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ctf

#endif  // CTF_AUTHORITY_HPP
