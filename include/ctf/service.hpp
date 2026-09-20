// Collective Traffic Fabric - coordinator service: authority, decision
// issuance, bounded history, lifecycle, and the resource limits that keep an
// adversarial peer from growing this process without bound.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_SERVICE_HPP
#define CTF_SERVICE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ctf/admission.hpp"
#include "ctf/authority.hpp"
#include "ctf/catalog.hpp"
#include "ctf/decision.hpp"
#include "ctf/engine.hpp"
#include "ctf/error.hpp"
#include "ctf/history.hpp"
#include "ctf/identity.hpp"
#include "ctf/limits.hpp"
#include "ctf/persistence.hpp"

namespace ctf {

// The identity fields of an inbound frame.  The transport layer fills this from
// the frame envelope; the service decides whether it is current.
struct EnvelopeView {
  SessionId session{};
  BootIncarnation incarnation{};
  CoordinatorEpoch epoch{};
  Sequence frame_sequence{};
  std::uint64_t correlation = 0;
};

// One authenticated session.  Every field is minted by the coordinator: the
// peer never supplies an identity that this runtime then believes.
struct SessionBinding {
  SessionId id{};
  BootIncarnation incarnation{};
  CoordinatorEpoch epoch{};
  std::string peer_label;
  std::uint64_t opened_monotonic_ms = 0;
  std::uint64_t last_frame_monotonic_ms = 0;
  Sequence last_frame_sequence{};
  std::uint64_t frames_received = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t accepted_participants = 0;
};

struct AttemptOutcome {
  CollectiveId id{};
  CollectiveAttemptId attempt{};
  CollectiveAttemptId previous_attempt{};
  CollectiveState state = CollectiveState::kUnregistered;
  ReasonChain reasons{};
};

struct ServiceRequestContext {
  SessionId session{};
  BootIncarnation incarnation{};
  CoordinatorEpoch epoch{};
  std::uint64_t now_monotonic_ms = 0;
  // Caller supplied correlation value.  It is echoed back on the response and
  // recorded in the bounded correlation index so a request can be explained
  // later; it is never used as an identity or as authority.
  std::uint64_t correlation = 0;
};

struct ServiceConfig {
  std::size_t decision_history_capacity = limits::kDecisionHistoryDefaultCapacity;
  std::uint64_t participant_ttl_ms = 3000;
  std::uint64_t session_idle_ttl_ms = 30000;
  std::uint64_t congestion_valid_for_ms = 2000;
  bool require_congestion_evidence = true;  // bulk classes need current congestion
  std::string coordinator_label = "ctf-coordinator";
};

// The coordinator core.  Every public method is thread safe and self contained.
// No lock is held while I/O, callbacks or user supplied code runs.
class CoordinatorService {
 public:
  explicit CoordinatorService(ServiceConfig config = {});
  ~CoordinatorService();
  CoordinatorService(const CoordinatorService&) = delete;
  CoordinatorService& operator=(const CoordinatorService&) = delete;

  // ---- lifecycle -----------------------------------------------------------
  Status start(std::uint64_t now_monotonic_ms);
  Status stop(std::uint64_t now_monotonic_ms);
  [[nodiscard]] bool is_running() const;
  [[nodiscard]] CoordinatorEpoch epoch() const;
  // True while a traffic policy is installed.  Probe this rather than comparing
  // generation() against an unset identity: an unset generation is the absence
  // of a policy, and comparing it to nothing is exactly the mistake the
  // authority rules exist to prevent.
  [[nodiscard]] bool has_policy() const;

  // ---- sessions ------------------------------------------------------------
  Status open_session(const std::string& peer_label, std::uint64_t now_monotonic_ms,
                      SessionBinding& out_binding);
  Status close_session(SessionId session, std::uint64_t now_monotonic_ms);
  Status validate_envelope(const EnvelopeView& envelope, std::uint64_t now_monotonic_ms,
                           ReasonChain& reasons);
  [[nodiscard]] std::size_t session_count() const;
  [[nodiscard]] std::size_t sweep_idle_sessions(std::uint64_t now_monotonic_ms);

  // ---- participant liveness ------------------------------------------------
  Status publish_participant(const ServiceRequestContext& context, ParticipantId participant,
                             BootIncarnation incarnation, NodeId node, ParticipantState state);
  Status heartbeat(const ServiceRequestContext& context, ParticipantId participant,
                   BootIncarnation incarnation);
  [[nodiscard]] std::vector<PeerLiveness> peers() const;
  // Participants that hold authority right now, from this service's own clock.
  [[nodiscard]] std::size_t live_peer_count() const;

  // ---- durable collectives -------------------------------------------------
  Status register_collective(const ServiceRequestContext& context, CollectiveDefinition definition,
                             RegistrationOutcome& out);
  Status begin_attempt(const ServiceRequestContext& context, CollectiveId id,
                       CollectiveGeneration generation, CollectiveAttemptId attempt,
                       std::uint64_t transfer_bytes, AttemptOutcome& out);
  Status cancel_collective(const ServiceRequestContext& context, CollectiveId id,
                           CollectiveGeneration generation, CollectiveAttemptId attempt,
                           CollectiveState& out_state, ReasonChain& reasons);
  Status retire_collective(const ServiceRequestContext& context, CollectiveId id,
                           CollectiveGeneration generation, CollectiveAttemptId attempt,
                           CollectiveState& out_state, ReasonChain& reasons);

  // ---- decisions -----------------------------------------------------------
  Status plan_flow_group(const ServiceRequestContext& context, DecisionRequest request,
                         DecisionRecord& out);

  // ---- evidence ------------------------------------------------------------
  Status ingest_capacity(const ServiceRequestContext& context, CapacityEvidence evidence,
                         EvidenceGeneration& out_generation, ReasonChain& reasons);
  Status ingest_congestion(const ServiceRequestContext& context, CongestionEvidence evidence,
                           EvidenceGeneration& out_generation, ReasonChain& reasons);
  Status ingest_topology(const ServiceRequestContext& context, TopologyEvidence evidence,
                         TopologyGeneration& out_generation, ReasonChain& reasons);
  // Installs a traffic policy.  The policy is coordinator owned state, so a peer
  // may propose one but never install it silently: the result is reported with
  // the generation that is now in force, and an equal or superseded generation is
  // refused rather than overwriting the one that made earlier decisions legal.
  Status install_policy(const ServiceRequestContext& context, TrafficPolicy policy,
                        PolicyGeneration& out_generation, ReasonChain& reasons);

  // ---- inspection ----------------------------------------------------------
  [[nodiscard]] std::string inspect_summary() const;
  [[nodiscard]] std::string inspect_collective(CollectiveId id) const;
  [[nodiscard]] std::vector<DecisionRecord> inspect_decisions(std::uint64_t offset, std::size_t limit) const;
  [[nodiscard]] std::string inspect_decisions_text(std::uint64_t offset, std::size_t limit) const;
  [[nodiscard]] std::string inspect_peers() const;
  [[nodiscard]] std::string inspect_policy() const;
  [[nodiscard]] std::string inspect_topology() const;
  [[nodiscard]] bool explain(CollectiveId id, CollectiveAttemptId attempt, DecisionRecord& out_decision,
                             std::string& out_text) const;
  [[nodiscard]] bool explain_correlation(std::uint64_t correlation, DecisionRecord& out_decision,
                                         std::string& out_text) const;

  // ---- persistence ---------------------------------------------------------
  Status configure_persistence(const std::string& path, std::uint64_t now_monotonic_ms);
  Status restore(const std::string& path, std::uint64_t now_monotonic_ms, LoadDisposition& disposition);
  Status flush(std::uint64_t now_monotonic_ms);
  [[nodiscard]] bool persistence_configured() const;
  [[nodiscard]] std::string persistence_path() const;
  [[nodiscard]] SnapshotContents snapshot_contents(std::uint64_t now_monotonic_ms) const;

  // ---- accounting ----------------------------------------------------------
  struct Counters {
    std::uint64_t sessions_opened = 0;  // counters are monotonic since the last reset
    std::uint64_t sessions_closed = 0;
    std::uint64_t registrations = 0;
    std::uint64_t generation_advances = 0;
    std::uint64_t decisions = 0;
    std::uint64_t admissions = 0;
    std::uint64_t rejections = 0;
    std::uint64_t rate_limited = 0;
    std::uint64_t deferred = 0;
    std::uint64_t replans = 0;
    std::uint64_t fences = 0;
    std::uint64_t evidence_ingests = 0;
    std::uint64_t rejections_stale = 0;
    std::uint64_t cancellations = 0;
    std::uint64_t retirements = 0;
    std::uint64_t stale_completions_rejected = 0;
    std::uint64_t persists = 0;
    std::uint64_t restores = 0;
  };
  [[nodiscard]] Counters counters() const;
  void reset_counters();

  // Direct access for tests and for the in-process driver.  Callers must
  // respect the threading contract documented on each type.
  [[nodiscard]] AuthorityState& authority() noexcept { return authority_; }
  [[nodiscard]] const AuthorityState& authority() const noexcept { return authority_; }
  [[nodiscard]] CollectiveCatalog& catalog() noexcept { return catalog_; }
  [[nodiscard]] const CollectiveCatalog& catalog() const noexcept { return catalog_; }
  [[nodiscard]] DecisionHistory& history() noexcept { return history_; }
  [[nodiscard]] const DecisionHistory& history() const noexcept { return history_; }
  [[nodiscard]] ServiceConfig config() const;
  void set_config(const ServiceConfig& config);

 private:
  Status check_session_locked(const EnvelopeView& envelope, std::uint64_t now_monotonic_ms,
                              ReasonChain& reasons);
  // Verifies that a caller is a current session on the current epoch before it
  // is allowed to mutate authority owned state.  Called with the state lock
  // already held.
  Status check_context_locked(const ServiceRequestContext& context, ReasonChain* reasons) const;
  // Records a durable mutation.  Called with the state lock already held.
  void mark_durable_mutation() noexcept {
    ++mutation_sequence_;
    persistence_dirty_ = true;
  }

  mutable std::mutex mutex_;
  ServiceConfig config_;
  AuthorityState authority_;
  CollectiveCatalog catalog_;
  DecisionHistory history_;
  AdmissionController admission_;
  Counters counters_;
  IdentityMap<SessionBinding> sessions_;
  // Incremented by every durable mutation.  A flush captures the sequence with
  // the contents and only clears the dirty flag when the sequence has not moved,
  // so a mutation that lands during a write is never silently forgotten.
  std::uint64_t mutation_sequence_ = 0;
  bool persistence_dirty_ = false;
  bool running_ = false;
  std::string persistence_path_;
  std::uint64_t last_flush_monotonic_ms_ = 0;
};

// Returns the identities of participants that are not currently authorized.
// Deterministic order; used by the engine and by tests.
[[nodiscard]] std::vector<ParticipantId> participants_without_authority(
    const std::vector<ParticipantId>& participants, const EvaluationSnapshot& snapshot);

}  // namespace ctf

#endif  // CTF_SERVICE_HPP
