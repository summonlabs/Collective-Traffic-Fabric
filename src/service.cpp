// Collective Traffic Fabric - coordinator service implementation.
// Copyright 2026 Summon Software Labs.
//
// Threading contract
// ------------------
// One mutex guards catalog, authority-derived decisions, sessions, counters and
// the persistence flags.  It is never held while:
//   * a file is written (flush snapshots its inputs and then releases);
//   * any callback or user supplied code runs (none exists on this path);
//   * another lock owned by this class is acquired (AuthorityState and
//     DecisionHistory have their own mutexes and are always called as leaf
//     operations, never while this class's mutex is held in a way that could
//     invert order - the authority lock is only ever taken after this one).
// Lock order is therefore: service mutex -> authority mutex, and never the
// reverse.  AuthorityState never calls back into the service.
#include "ctf/service.hpp"

#include <algorithm>
#include <sstream>

#include "ctf/snapshot.hpp"
#include "ctf/transport.hpp"

namespace ctf {
namespace {

std::uint64_t age_ms(std::uint64_t now, std::uint64_t then) { return now >= then ? now - then : 0; }

}  // namespace

std::vector<ParticipantId> participants_without_authority(const std::vector<ParticipantId>& participants,
                                                          const EvaluationSnapshot& snapshot) {
  std::vector<ParticipantId> missing;
  for (const ParticipantId& participant : participants) {
    const PeerLiveness* liveness = snapshot.find_peer(participant);
    if (liveness == nullptr || !liveness->is_current(snapshot.now_monotonic_ms)) {
      missing.push_back(participant);
    }
  }
  std::sort(missing.begin(), missing.end());
  return missing;
}

namespace {

std::string render_definition(const CollectiveDefinition& definition) {
  std::ostringstream out;
  out << "  class=" << to_string(definition.collective_class)
      << " hint=" << to_string(definition.algorithm_hint)
      << " scope=" << to_string(definition.scope) << "\n";
  out << "  label=" << (definition.label.empty() ? std::string("-") : definition.label) << "\n";
  out << "  participants=" << definition.participants.size()
      << " phases=" << definition.phases.size() << " steps=" << definition.steps.size()
      << " logical_bytes=" << definition.logical_bytes << "\n";
  out << "  barrier_semantics=" << (definition.declares_barrier_semantics ? "yes" : "no")
      << " fabric=" << (definition.fabric.is_some() ? definition.fabric.to_string() : std::string("-"))
      << "\n";
  return out.str();
}

}  // namespace

CoordinatorService::CoordinatorService(ServiceConfig config) : config_(std::move(config)) {
  history_.set_capacity(config_.decision_history_capacity);
}

CoordinatorService::~CoordinatorService() = default;

Status CoordinatorService::start(std::uint64_t now_monotonic_ms) {
  std::lock_guard<std::mutex> guard(mutex_);
  static_cast<void>(now_monotonic_ms);
  if (running_) {
    return Status(ErrorCode::kStateDuplicateRegistration, "the coordinator is already running");
  }
  // begin_boot mints a fresh epoch and clears liveness, capacity and congestion
  // in one step, so no caller can bring dynamic state back across a boot.
  authority_.begin_boot(authority_.epoch());
  FreshnessPolicy freshness;
  freshness.congestion_max_age_ms = config_.congestion_valid_for_ms;
  authority_.set_freshness_policy(freshness);
  running_ = true;
  return Status::ok();
}

Status CoordinatorService::stop(std::uint64_t now_monotonic_ms) {
  SnapshotContents contents;
  std::string path;
  std::uint64_t captured_sequence = 0;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!running_) {
      return Status(ErrorCode::kStateNotLive, "the coordinator is not running");
    }
    running_ = false;
    // Expire every participant and drop every dynamic observation.  Shutdown
    // must not leave liveness behind that a later process could inherit.
    for (auto& entry : sessions_) {
      authority_.expire_session(entry.first, now_monotonic_ms);
    }
    sessions_.clear();
    history_.clear();
    authority_.invalidate_dynamic_evidence();
    admission_.clear();
    if (persistence_dirty_ && !persistence_path_.empty()) {
      contents.written_monotonic_ms = now_monotonic_ms;
      contents.authority = authority_.durable_snapshot();
      contents.records = catalog_.export_all();
      path = persistence_path_;
      captured_sequence = mutation_sequence_;
    }
  }
  // The flush happens with no lock held: writing a file while holding the state
  // lock would block every other caller on disk latency.
  if (path.empty()) {
    return Status::ok();
  }
  SnapshotStore store(path);
  const Status status = store.save(contents, now_monotonic_ms);
  if (status.is_ok()) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (mutation_sequence_ == captured_sequence) {
      persistence_dirty_ = false;
    }
    ++counters_.persists;
  }
  return status;
}

bool CoordinatorService::is_running() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return running_;
}

CoordinatorEpoch CoordinatorService::epoch() const { return authority_.epoch(); }

bool CoordinatorService::has_policy() const { return authority_.policy_snapshot().generation.is_some(); }

Status CoordinatorService::open_session(const std::string& peer_label, std::uint64_t now_monotonic_ms,
                                        SessionBinding& out_binding) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!running_) {
    return Status(ErrorCode::kStateShuttingDown, "the coordinator is not accepting sessions");
  }
  if (sessions_.size() >= limits::kSessionsMax) {
    return Status(ErrorCode::kTransportSessionLimit, "the session limit has been reached");
  }
  SessionBinding binding;
  binding.id = mint_identity();
  binding.incarnation = mint_identity();
  binding.epoch = authority_.epoch();
  binding.peer_label = peer_label.size() > limits::kLabelBytesMax
                           ? peer_label.substr(0, limits::kLabelBytesMax)
                           : peer_label;
  binding.opened_monotonic_ms = now_monotonic_ms;
  binding.last_frame_monotonic_ms = now_monotonic_ms;
  sessions_.emplace(binding.id, binding);
  ++counters_.sessions_opened;
  out_binding = binding;
  return Status::ok();
}

Status CoordinatorService::check_session_locked(const EnvelopeView& envelope, std::uint64_t now_monotonic_ms,
                                                ReasonChain& reasons) {
  if (envelope.session.is_none()) {
    reasons.add("envelope_session_missing", "the frame carries no session identity");
    return Status(ErrorCode::kEnvelopeSessionUnknown, "the frame carries no session identity");
  }
  auto session = sessions_.find(envelope.session);
  if (session == sessions_.end()) {
    reasons.add("envelope_session_unknown", "the session is not known to this coordinator",
                envelope.session);
    return Status(ErrorCode::kEnvelopeSessionUnknown, "unknown session");
  }
  SessionBinding& binding = session->second;
  if (envelope.epoch.is_none() || envelope.epoch != authority_.epoch()) {
    reasons.add("envelope_epoch_stale",
                "the frame names a coordinator epoch that is not current; a restart invalidates "
                "every earlier authority",
                envelope.epoch);
    return Status(ErrorCode::kEnvelopeEpochStale, "stale coordinator epoch");
  }
  if (envelope.incarnation.is_none()) {
    reasons.add("envelope_incarnation_missing", "the frame carries no boot incarnation");
    return Status(ErrorCode::kEnvelopeIncarnationStale, "missing boot incarnation");
  }
  if (envelope.incarnation != binding.incarnation) {
    if (envelope.incarnation < binding.incarnation) {
      reasons.add("envelope_incarnation_fenced",
                  "the frame comes from a boot incarnation that has been fenced", envelope.incarnation);
      return Status(ErrorCode::kEnvelopeIncarnationStale, "fenced boot incarnation");
    }
    return Status(ErrorCode::kEnvelopeSessionMismatch, "boot incarnation does not match this session");
  }
  if (envelope.frame_sequence.is_unset()) {
    reasons.add("envelope_sequence_missing", "the frame carries no sequence number");
    return Status(ErrorCode::kEnvelopeSequenceRegression, "missing frame sequence");
  }
  if (envelope.frame_sequence.value() <= binding.last_frame_sequence.value()) {
    reasons.add("envelope_sequence_regression",
                "the frame sequence did not advance; replays and reordered frames are refused",
                envelope.session);
    return Status(ErrorCode::kEnvelopeReplayed, "frame sequence did not advance");
  }
  binding.last_frame_sequence = envelope.frame_sequence;
  binding.last_frame_monotonic_ms = now_monotonic_ms;
  ++binding.frames_received;
  return Status::ok();
}

Status CoordinatorService::validate_envelope(const EnvelopeView& envelope, std::uint64_t now_monotonic_ms,
                                             ReasonChain& reasons) {
  std::lock_guard<std::mutex> guard(mutex_);
  return check_session_locked(envelope, now_monotonic_ms, reasons);
}

Status CoordinatorService::close_session(SessionId session, std::uint64_t now_monotonic_ms) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto existing = sessions_.find(session);
  if (existing == sessions_.end()) {
    return Status(ErrorCode::kEnvelopeSessionUnknown, "unknown session");
  }
  const std::size_t expired = authority_.expire_session(session, now_monotonic_ms);
  counters_.fences += expired;
  sessions_.erase(existing);
  ++counters_.sessions_closed;
  return Status::ok();
}

std::size_t CoordinatorService::session_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return sessions_.size();
}

std::size_t CoordinatorService::sweep_idle_sessions(std::uint64_t now_monotonic_ms) {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<SessionId> stale;
  for (const auto& entry : sessions_) {
    if (age_ms(now_monotonic_ms, entry.second.last_frame_monotonic_ms) > config_.session_idle_ttl_ms) {
      stale.push_back(entry.first);
    }
  }
  std::sort(stale.begin(), stale.end());
  for (const SessionId& session : stale) {
    const std::size_t expired = authority_.expire_session(session, now_monotonic_ms);
    counters_.fences += expired;
    sessions_.erase(session);
    ++counters_.sessions_closed;
  }
  return stale.size();
}

Status CoordinatorService::publish_participant(const ServiceRequestContext& context, ParticipantId participant,
                                               BootIncarnation incarnation, NodeId node,
                                               ParticipantState state) {
  if (participant.is_none() || incarnation.is_none()) {
    return Status(ErrorCode::kValidationParticipantUnset, "participant or incarnation identity is unset");
  }
  if (!participant_state_holds_authority(state)) {
    // Only a live publication creates authority.  Any other state is accepted
    // as an observation but never grants traffic permission.
    std::lock_guard<std::mutex> guard(mutex_);
    PeerLiveness liveness;
    liveness.participant = participant;
    liveness.incarnation = incarnation;
    liveness.state = state;
    liveness.node = node;
    liveness.bound_session = context.session;
    liveness.last_seen_monotonic_ms = context.now_monotonic_ms;
    liveness.expires_at_monotonic_ms = context.now_monotonic_ms;
    const auto published = authority_.publish_liveness(liveness);
    if (published == AuthorityState::PublishResult::kFencedStaleIncarnation) {
      ++counters_.fences;
      return Status(ErrorCode::kEnvelopeIncarnationStale,
                    "a newer boot incarnation for this participant has already been observed");
    }
    return Status::ok();
  }

  PeerLiveness liveness;
  liveness.participant = participant;
  liveness.incarnation = incarnation;
  liveness.state = ParticipantState::kLive;
  liveness.node = node;
  liveness.bound_session = context.session;
  liveness.last_seen_monotonic_ms = context.now_monotonic_ms;
  liveness.expires_at_monotonic_ms = context.now_monotonic_ms + config_.participant_ttl_ms;

  std::lock_guard<std::mutex> guard(mutex_);
  const auto published = authority_.publish_liveness(liveness);
  if (published == AuthorityState::PublishResult::kFencedStaleIncarnation) {
    ++counters_.fences;
    return Status(ErrorCode::kEnvelopeIncarnationStale,
                  "a newer boot incarnation for this participant has already been observed");
  }
  if (published == AuthorityState::PublishResult::kRejectedCapacity) {
    return Status(ErrorCode::kStateCapacityExceeded, "the participant table is full");
  }
  auto session = sessions_.find(context.session);
  if (session != sessions_.end()) {
    ++session->second.accepted_participants;
  }
  return Status::ok();
}

Status CoordinatorService::heartbeat(const ServiceRequestContext& context, ParticipantId participant,
                                     BootIncarnation incarnation) {
  if (participant.is_none() || incarnation.is_none()) {
    return Status(ErrorCode::kValidationParticipantUnset, "participant or incarnation identity is unset");
  }
  std::lock_guard<std::mutex> guard(mutex_);
  if (authority_.heartbeat(participant, incarnation, context.now_monotonic_ms, config_.participant_ttl_ms)) {
    return Status::ok();
  }
  ++counters_.fences;
  return Status(ErrorCode::kEnvelopeIncarnationStale,
                "the heartbeat comes from a boot incarnation that has been fenced");
}

std::vector<PeerLiveness> CoordinatorService::peers() const { return authority_.peers_snapshot(); }

std::size_t CoordinatorService::live_peer_count() const {
  return authority_.live_peer_count(transport::monotonic_now_ms());
}

Status CoordinatorService::register_collective(const ServiceRequestContext& context,
                                               CollectiveDefinition definition, RegistrationOutcome& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!running_) {
    return Status(ErrorCode::kStateShuttingDown, "the coordinator is not running");
  }
  if (sessions_.find(context.session) == sessions_.end()) {
    return Status(ErrorCode::kEnvelopeSessionUnknown, "unknown session");
  }
  RegistrationOutcome outcome = catalog_.register_definition(definition, context.now_monotonic_ms);
  out = outcome;
  switch (outcome.result) {
    case RegistrationResult::kRegistered:
      ++counters_.registrations;
      mark_durable_mutation();
      break;
    case RegistrationResult::kGenerationAdvanced:
      ++counters_.registrations;
      ++counters_.generation_advances;
      mark_durable_mutation();
      break;
    case RegistrationResult::kUnchanged:
      break;
    case RegistrationResult::kRejectedCapacity:
      return Status(ErrorCode::kStateCapacityExceeded, "the live collective bound has been reached");
    case RegistrationResult::kRejectedTerminal:
      return Status(ErrorCode::kStateCollectiveRetired,
                    "the collective is terminal; a durable definition is never resurrected");
    case RegistrationResult::kRejectedInvalid:
      return Status(ErrorCode::kValidationClassUnknownRejected, "the definition was rejected");
  }
  return Status::ok();
}

Status CoordinatorService::begin_attempt(const ServiceRequestContext& context, CollectiveId id,
                                         CollectiveGeneration generation, CollectiveAttemptId attempt,
                                         std::uint64_t transfer_bytes, AttemptOutcome& out) {
  AttemptOutcome outcome;
  outcome.id = id;
  outcome.attempt = attempt;

  std::lock_guard<std::mutex> guard(mutex_);
  if (!running_) {
    return Status(ErrorCode::kStateShuttingDown, "the coordinator is not running");
  }
  const CollectiveRecord* record = catalog_.find(id);
  if (record == nullptr) {
    outcome.reasons.add("collective_unknown", "no durable definition exists for this identity", id);
    out = outcome;
    ++counters_.stale_completions_rejected;
    return Status(ErrorCode::kCompletionUnknownCollective, "collective is not registered");
  }
  if (record->definition.generation != generation) {
    outcome.state = record->state;
    outcome.reasons.add("attempt_generation_stale",
                        "the attempt names a collective generation that is not current", id);
    out = outcome;
    ++counters_.stale_completions_rejected;
    return Status(ErrorCode::kCompletionStaleGeneration, "collective generation is stale");
  }
  if (collective_state_is_terminal(record->state)) {
    outcome.state = record->state;
    outcome.reasons.add("collective_terminal",
                        "a cancelled, retired or completed collective never resumes", id);
    out = outcome;
    ++counters_.stale_completions_rejected;
    return Status(record->state == CollectiveState::kCancelled ? ErrorCode::kCompletionCancelled
                                                              : ErrorCode::kCompletionRetired,
                  "collective is terminal");
  }
  CollectiveAttemptId previous;
  Status status = catalog_.begin_attempt(id, attempt, context.now_monotonic_ms, previous);
  if (!status.is_ok()) {
    outcome.reasons.add("attempt_rejected", status.message(), id);
    out = outcome;
    return status;
  }
  if (CollectiveRecord* mutable_record = catalog_.find_mutable(id); mutable_record != nullptr) {
    mutable_record->attempt_transfer_bytes = transfer_bytes;
  }
  const CollectiveRecord* updated = catalog_.find(id);
  outcome.previous_attempt = previous;
  outcome.state = updated != nullptr ? updated->state : CollectiveState::kPlanning;
  outcome.reasons.add("attempt_begun",
                      "a new attempt was opened; every decision bound to the previous attempt is now stale", id);
  out = outcome;
  mark_durable_mutation();
  return Status::ok();
}

Status CoordinatorService::cancel_collective(const ServiceRequestContext& context, CollectiveId id,
                                             CollectiveGeneration generation, CollectiveAttemptId attempt,
                                             CollectiveState& out_state, ReasonChain& reasons) {
  std::lock_guard<std::mutex> guard(mutex_);
  const CollectiveRecord* record = catalog_.find(id);
  if (record == nullptr) {
    reasons.add("collective_unknown", "no durable definition exists for this identity", id);
    return Status(ErrorCode::kCompletionUnknownCollective, "collective is not registered");
  }
  if (record->definition.generation != generation) {
    reasons.add("cancellation_generation_stale",
                "the cancellation names a generation that is not current; it is refused rather than "
                "applied to the wrong generation",
                id);
    ++counters_.stale_completions_rejected;
    return Status(ErrorCode::kCompletionStaleGeneration, "collective generation is stale");
  }
  if (collective_state_is_terminal(record->state)) {
    out_state = record->state;
    reasons.add("cancellation_terminal", "the collective is already terminal", id);
    return Status(record->state == CollectiveState::kCancelled ? ErrorCode::kCompletionDuplicate
                                                              : ErrorCode::kCompletionRetired,
                  "collective is already terminal");
  }
  if (record->current_attempt.is_some() && attempt.is_some() && record->current_attempt != attempt) {
    reasons.add("cancellation_attempt_stale",
                "the cancellation names an attempt that is no longer current", id);
    ++counters_.stale_completions_rejected;
    return Status(ErrorCode::kCompletionStaleAttempt, "attempt is stale");
  }
  Status status = catalog_.cancel(id, context.now_monotonic_ms);
  if (!status.is_ok()) {
    reasons.add("cancellation_failed", status.message(), id);
    return status;
  }
  out_state = CollectiveState::kCancelled;
  reasons.add("cancelled",
              "the collective is cancelled; a later completion for any earlier generation or attempt "
              "cannot restore it",
              id);
  ++counters_.cancellations;
  mark_durable_mutation();
  return Status::ok();
}

Status CoordinatorService::retire_collective(const ServiceRequestContext& context, CollectiveId id,
                                             CollectiveGeneration generation, CollectiveAttemptId attempt,
                                             CollectiveState& out_state, ReasonChain& reasons) {
  std::lock_guard<std::mutex> guard(mutex_);
  const CollectiveRecord* record = catalog_.find(id);
  if (record == nullptr) {
    reasons.add("collective_unknown", "no durable definition exists for this identity", id);
    return Status(ErrorCode::kCompletionUnknownCollective, "collective is not registered");
  }
  if (record->definition.generation != generation) {
    reasons.add("retirement_generation_stale",
                "the retirement names a generation that is not current", id);
    ++counters_.stale_completions_rejected;
    return Status(ErrorCode::kCompletionStaleGeneration, "collective generation is stale");
  }
  if (collective_state_is_terminal(record->state)) {
    out_state = record->state;
    reasons.add("retirement_terminal", "the collective is already terminal", id);
    return Status(ErrorCode::kCompletionDuplicate, "collective is already terminal");
  }
  if (record->current_attempt.is_some() && attempt.is_some() && record->current_attempt != attempt) {
    reasons.add("retirement_attempt_stale", "the retirement names an attempt that is no longer current", id);
    ++counters_.stale_completions_rejected;
    return Status(ErrorCode::kCompletionStaleAttempt, "attempt is stale");
  }
  Status status = catalog_.retire(id, context.now_monotonic_ms);
  if (!status.is_ok()) {
    reasons.add("retirement_failed", status.message(), id);
    return status;
  }
  out_state = CollectiveState::kRetired;
  reasons.add("retired", "the durable definition is retired and never resurrected", id);
  ++counters_.retirements;
  mark_durable_mutation();
  return Status::ok();
}

Status CoordinatorService::plan_flow_group(const ServiceRequestContext& context, DecisionRequest request,
                                           DecisionRecord& out) {
  DecisionRecord decision;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!running_) {
      return Status(ErrorCode::kStateShuttingDown, "the coordinator is not running");
    }
    if (context.session.is_some()) {
      auto session = sessions_.find(context.session);
      if (session == sessions_.end()) {
        return Status(ErrorCode::kEnvelopeSessionUnknown, "unknown session");
      }
      if (session->second.incarnation != context.incarnation) {
        return Status(ErrorCode::kEnvelopeSessionMismatch, "session incarnation does not match");
      }
      if (context.epoch.is_some() && context.epoch != authority_.epoch()) {
        return Status(ErrorCode::kEnvelopeEpochStale, "stale coordinator epoch");
      }
    }
    Status validated = validate_decision_request(request);
    if (!validated.is_ok()) {
      return validated;
    }

    const CollectiveRecord* record = catalog_.find(request.instance.id);
    if (record == nullptr) {
      ++counters_.rejections;
      ++counters_.rejections_stale;
      ++counters_.stale_completions_rejected;
      decision.instance = request.instance;
      decision.outcome = Outcome::kRejectedStale;
      decision.axis = AuthorityAxis::kCollectiveGeneration;
      decision.code = ErrorCode::kCompletionUnknownCollective;
      decision.decided_monotonic_ms = context.now_monotonic_ms;
      decision.reasons.add("collective_unknown",
                           "no durable definition exists for this identity; a decision requires a "
                           "registered collective",
                           request.instance.id);
      const std::uint64_t ordinal = history_.record(decision);
      static_cast<void>(ordinal);
      out = decision;
      return Status::ok();
    }
    if (record->definition.generation != request.instance.generation) {
      ++counters_.rejections;
      ++counters_.rejections_stale;
      ++counters_.stale_completions_rejected;
      decision.instance = request.instance;
      decision.collective_class = record->definition.collective_class;
      decision.outcome = Outcome::kRejectedStale;
      decision.axis = AuthorityAxis::kCollectiveGeneration;
      decision.code = ErrorCode::kCompletionStaleGeneration;
      decision.decided_monotonic_ms = context.now_monotonic_ms;
      decision.reasons.add("collective_generation_stale",
                           "the request names a generation that is not current; membership or semantics "
                           "changed and every earlier decision is stale",
                           request.instance.id);
      const std::uint64_t ordinal = history_.record(decision);
      static_cast<void>(ordinal);
      out = decision;
      return Status::ok();
    }
    if (record->current_attempt.is_none() || record->current_attempt != request.instance.attempt) {
      ++counters_.rejections;
      ++counters_.rejections_stale;
      ++counters_.stale_completions_rejected;
      decision.instance = request.instance;
      decision.collective_class = record->definition.collective_class;
      decision.outcome = Outcome::kRejectedStale;
      decision.axis = AuthorityAxis::kAttemptGeneration;
      decision.code = ErrorCode::kCompletionStaleAttempt;
      decision.decided_monotonic_ms = context.now_monotonic_ms;
      decision.reasons.add("attempt_stale",
                           "the request names an attempt that is not the current attempt of this "
                           "generation; flow group authority cannot be reused across attempts",
                           request.instance.id);
      const std::uint64_t ordinal = history_.record(decision);
      static_cast<void>(ordinal);
      out = decision;
      return Status::ok();
    }
    if (collective_state_is_terminal(record->state)) {
      ++counters_.rejections;
      ++counters_.stale_completions_rejected;
      decision.instance = request.instance;
      decision.collective_class = record->definition.collective_class;
      decision.outcome = Outcome::kRejectedStale;
      decision.axis = AuthorityAxis::kLifecycle;
      decision.code = record->state == CollectiveState::kCancelled ? ErrorCode::kCompletionCancelled
                                                                   : ErrorCode::kCompletionRetired;
      decision.decided_monotonic_ms = context.now_monotonic_ms;
      decision.reasons.add("collective_terminal",
                           std::string("the collective is ") + std::string(to_string(record->state)) +
                               "; it cannot be restored by a later completion",
                           request.instance.id);
      const std::uint64_t ordinal = history_.record(decision);
      static_cast<void>(ordinal);
      out = decision;
      return Status::ok();
    }

    const EvaluationSnapshot snapshot = authority_.snapshot(context.now_monotonic_ms);
    // The attempt may declare a transfer volume that the durable definition did
    // not state (NCCL style count x datatype).  It is used only when the
    // definition says nothing, so a member cannot inflate the volume of a
    // collective whose definition it does not own, and a declared volume is
    // always labelled as declared rather than measured.
    CollectiveDefinition effective = record->definition;
    if (effective.logical_bytes == 0 && record->attempt_transfer_bytes != 0) {
      effective.logical_bytes = record->attempt_transfer_bytes;
    }
    EngineCounters counters;
    DecisionRecord evaluated = evaluate(snapshot, request, effective, &counters);
    evaluated.instance = request.instance;

    if (outcome_grants_authority(evaluated.outcome)) {
      Status advanced = catalog_.set_state(request.instance.id, CollectiveState::kActive,
                                           context.now_monotonic_ms);
      if (!advanced.is_ok()) {
        evaluated.outcome = Outcome::kRejectedStale;
        evaluated.axis = AuthorityAxis::kLifecycle;
        evaluated.code = advanced.code();
        evaluated.reasons.add("activation_failed", advanced.message(), request.instance.id);
      } else {
        AdmissionResult admission = admission_.admit(evaluated, context.now_monotonic_ms);
        for (const Reason& reason : admission.reasons.reasons()) {
          evaluated.reasons.add(reason);
        }
        switch (admission.outcome) {
          case AdmissionOutcome::kGranted:
            ++counters_.admissions;
            break;
          case AdmissionOutcome::kRateLimited:
            // The permission stands; the release does not.  The outcome reports
            // the timing answer without pretending the decision was illegal.
            evaluated.pacing.release_after_monotonic_ms =
                context.now_monotonic_ms + admission.wait_ms;
            evaluated.outcome = Outcome::kRateLimited;
            evaluated.code = ErrorCode::kAdmissionRateLimited;
            ++counters_.rate_limited;
            break;
          case AdmissionOutcome::kClockRegression:
            evaluated.outcome = Outcome::kDeferred;
            evaluated.code = ErrorCode::kAdmissionClockRegression;
            evaluated.pacing.release_after_monotonic_ms = 0;
            ++counters_.deferred;
            break;
          case AdmissionOutcome::kBucketTableFull:
            evaluated.outcome = Outcome::kRejectedCapacity;
            evaluated.axis = AuthorityAxis::kCapacityEvidence;
            evaluated.code = ErrorCode::kAdmissionBucketCapacity;
            ++counters_.rejections;
            break;
          case AdmissionOutcome::kNotApplicable:
          default:
            break;
        }
      }
    } else if (outcome_is_rejection(evaluated.outcome)) {
      ++counters_.rejections;
      if (evaluated.outcome == Outcome::kRejectedStale) {
        ++counters_.rejections_stale;
      }
      if (evaluated.outcome == Outcome::kUnknownSemantics) {
        ++counters_.rejections;
      }
    } else if (outcome_requires_replan(evaluated.outcome)) {
      ++counters_.replans;
    }

    ++counters_.decisions;
    const std::uint64_t ordinal = history_.record(evaluated);
    if (context.session.is_some() && context.correlation != 0) {
      history_.bind_correlation(context.correlation, ordinal);
    }
    decision = std::move(evaluated);
  }
  out = std::move(decision);
  return Status::ok();
}

Status CoordinatorService::check_context_locked(const ServiceRequestContext& context,
                                                ReasonChain* reasons) const {
  auto session = sessions_.find(context.session);
  if (session == sessions_.end()) {
    if (reasons != nullptr) {
      reasons->add("session_unknown",
                   "the request does not come from a session this coordinator owns", context.session);
    }
    return Status(ErrorCode::kEnvelopeSessionUnknown, "unknown session");
  }
  if (context.incarnation.is_none() || session->second.incarnation != context.incarnation) {
    if (reasons != nullptr) {
      reasons->add("session_incarnation_mismatch",
                   "the request carries a boot incarnation that this session does not own",
                   context.incarnation);
    }
    return Status(ErrorCode::kEnvelopeSessionMismatch, "session incarnation does not match");
  }
  if (context.epoch.is_some() && context.epoch != authority_.epoch()) {
    if (reasons != nullptr) {
      reasons->add("epoch_stale",
                   "the request names a coordinator epoch that is not current; a restart invalidates "
                   "every earlier authority",
                   context.epoch);
    }
    return Status(ErrorCode::kEnvelopeEpochStale, "stale coordinator epoch");
  }
  return Status::ok();
}

Status CoordinatorService::ingest_capacity(const ServiceRequestContext& context, CapacityEvidence evidence,
                                           EvidenceGeneration& out_generation, ReasonChain& reasons) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status authority = check_context_locked(context, &reasons);
  if (!authority.is_ok()) return authority;
  if (evidence.generation.is_none()) {
    evidence.generation = mint_identity();
  }
  const InstallResult result = authority_.install_capacity(evidence);
  out_generation = authority_.capacity_generation();
  ++counters_.evidence_ingests;
  switch (result) {
    case InstallResult::kInstalled:
      reasons.add("capacity_installed", "capacity observation installed for the current topology",
                  evidence.generation);
      return Status::ok();
    case InstallResult::kRejectedOlderGeneration:
      reasons.add("capacity_generation_stale",
                  "a newer capacity observation is already installed; an older one is refused", evidence.generation);
      return Status(ErrorCode::kEnvelopeEpochStale, "capacity generation is not newer");
    case InstallResult::kRejectedInvalid:
      reasons.add("capacity_invalid",
                  "the capacity observation does not match the current topology or is malformed",
                  evidence.topology_generation);
      return Status(ErrorCode::kValidationTopologyMissing, "capacity evidence rejected");
    case InstallResult::kRejectedOversized:
    default:
      reasons.add("capacity_oversized", "the capacity observation exceeds the accepted bound", evidence.generation);
      return Status(ErrorCode::kDecodeCountOutOfRange, "capacity evidence too large");
  }
}

Status CoordinatorService::ingest_congestion(const ServiceRequestContext& context, CongestionEvidence evidence,
                                             EvidenceGeneration& out_generation, ReasonChain& reasons) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status authority = check_context_locked(context, &reasons);
  if (!authority.is_ok()) return authority;
  if (evidence.generation.is_none()) {
    evidence.generation = mint_identity();
  }
  const std::uint64_t bound = std::min<std::uint64_t>(evidence.valid_for_ms, config_.congestion_valid_for_ms);
  if (bound != evidence.valid_for_ms) {
    evidence.valid_for_ms = bound;
  }
  const InstallResult result = authority_.install_congestion(evidence);
  out_generation = authority_.congestion_generation();
  ++counters_.evidence_ingests;
  switch (result) {
    case InstallResult::kInstalled:
      reasons.add("congestion_installed", "congestion observation installed for the current topology",
                  evidence.generation);
      return Status::ok();
    case InstallResult::kRejectedOlderGeneration:
      reasons.add("congestion_generation_stale",
                  "a newer congestion observation is already installed; an older one is refused",
                  evidence.generation);
      return Status(ErrorCode::kEnvelopeEpochStale, "congestion generation is not newer");
    case InstallResult::kRejectedInvalid:
      reasons.add("congestion_invalid",
                  "the congestion observation does not match the current topology or is malformed",
                  evidence.topology_generation);
      return Status(ErrorCode::kValidationTopologyMissing, "congestion evidence rejected");
    case InstallResult::kRejectedOversized:
    default:
      reasons.add("congestion_oversized", "the congestion observation exceeds the accepted bound",
                  evidence.generation);
      return Status(ErrorCode::kDecodeCountOutOfRange, "congestion evidence too large");
  }
}

Status CoordinatorService::ingest_topology(const ServiceRequestContext& context, TopologyEvidence evidence,
                                           TopologyGeneration& out_generation, ReasonChain& reasons) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status authority = check_context_locked(context, &reasons);
  if (!authority.is_ok()) return authority;
  if (evidence.generation.is_none()) {
    evidence.generation = mint_identity();
  }
  const InstallResult result = authority_.install_topology(evidence);
  out_generation = authority_.topology_generation();
  ++counters_.evidence_ingests;
  switch (result) {
    case InstallResult::kInstalled:
      // A topology change drops capacity and congestion observations, so any
      // live flow group authority bound to them is stale from this instant.
      reasons.add("topology_installed",
                  "topology installed; capacity and congestion observations were dropped because they "
                  "describe a previous topology",
                  evidence.generation);
      mark_durable_mutation();
      return Status::ok();
    case InstallResult::kRejectedOlderGeneration:
      reasons.add("topology_generation_stale",
                  "a newer topology is already installed; an older one is refused", evidence.generation);
      return Status(ErrorCode::kEnvelopeEpochStale, "topology generation is not newer");
    case InstallResult::kRejectedInvalid:
      reasons.add("topology_invalid", "the topology observation is malformed", evidence.generation);
      return Status(ErrorCode::kValidationNodeUnknown, "topology evidence rejected");
    case InstallResult::kRejectedOversized:
    default:
      reasons.add("topology_oversized", "the topology observation exceeds the accepted bound",
                  evidence.generation);
      return Status(ErrorCode::kDecodeCountOutOfRange, "topology evidence too large");
  }
}

Status CoordinatorService::install_policy(const ServiceRequestContext& context, TrafficPolicy policy,
                                          PolicyGeneration& out_generation, ReasonChain& reasons) {
  std::lock_guard<std::mutex> guard(mutex_);
  const Status authority = check_context_locked(context, &reasons);
  if (!authority.is_ok()) return authority;
  if (policy.generation.is_none()) {
    policy.generation = mint_identity();
  }
  const InstallResult result = authority_.install_policy(std::move(policy));
  out_generation = authority_.policy_generation();
  switch (result) {
    case InstallResult::kInstalled:
      reasons.add("policy_installed", "traffic policy installed; earlier decisions bound to a previous "
                                      "policy generation are now stale",
                  out_generation);
      mark_durable_mutation();
      return Status::ok();
    case InstallResult::kRejectedSameGeneration:
      reasons.add("policy_generation_reused",
                  "the policy generation equals the one already installed; a different policy under the "
                  "same generation would make two incompatible decisions indistinguishable",
                  out_generation);
      return Status(ErrorCode::kEnvelopeEpochStale, "policy generation is already installed");
    case InstallResult::kRejectedOlderGeneration:
      reasons.add("policy_generation_superseded",
                  "this policy generation has already been superseded by a newer one", out_generation);
      return Status(ErrorCode::kEnvelopeEpochStale, "policy generation is superseded");
    case InstallResult::kRejectedOversized:
    case InstallResult::kRejectedInvalid:
    default:
      reasons.add("policy_rejected", "the traffic policy is not internally consistent");
      return Status(ErrorCode::kValidationPolicyMissing, "traffic policy rejected");
  }
}

std::string CoordinatorService::inspect_summary() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::ostringstream out;
  out << "coordinator: " << config_.coordinator_label << "\n";
  out << "running: " << (running_ ? "yes" : "no") << "\n";
  out << "epoch: " << authority_.epoch().to_string() << "\n";
  out << "policy_generation: " << authority_.policy_generation().to_string() << "\n";
  out << "topology_generation: " << authority_.topology_generation().to_string() << "\n";
  out << "capacity_generation: " << authority_.capacity_generation().to_string() << "\n";
  out << "congestion_generation: " << authority_.congestion_generation().to_string() << "\n";
  out << "collectives: " << catalog_.size() << " of " << catalog_.capacity() << "\n";
  out << "peers: " << authority_.live_peer_count(ctf::transport::monotonic_now_ms()) << " live of "
      << authority_.peer_count() << " recorded\n";
  out << "sessions: " << sessions_.size() << " of " << limits::kSessionsMax << "\n";
  out << "decision_history: " << history_.size() << " of " << history_.capacity() << "\n";
  out << "decisions: " << counters_.decisions << " admissions: " << counters_.admissions
      << " rejections: " << counters_.rejections << "\n";
  out << "rate_limited: " << counters_.rate_limited << " deferred: " << counters_.deferred
      << " replans: " << counters_.replans << "\n";
  out << "fences: " << counters_.fences << " stale_completions_rejected: "
      << counters_.stale_completions_rejected << "\n";
  out << "persistence: " << (persistence_path_.empty() ? std::string("disabled") : persistence_path_)
      << (persistence_dirty_ ? " (dirty)" : " (clean)") << "\n";
  return out.str();
}

std::string CoordinatorService::inspect_collective(CollectiveId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const CollectiveRecord* record = catalog_.find(id);
  if (record == nullptr) {
    return std::string("collective ") + id.to_string() + " is not registered\n";
  }
  std::ostringstream out;
  out << "collective: " << record->definition.id.to_string() << "\n";
  out << "  generation: " << record->definition.generation.to_string() << "\n";
  out << "  state: " << to_string(record->state) << "\n";
  out << "  current_attempt: "
      << (record->current_attempt.is_some() ? record->current_attempt.to_string() : std::string("none")) << "\n";
  out << "  attempt_sequence: " << record->attempt_sequence.value() << "\n";
  out << "  registration_sequence: " << record->registration_sequence.value() << "\n";
  out << "  generation_ancestry_depth: " << record->generation_ancestry_depth << "\n";
  out << "  accepts_new_authority: " << (record->accepts_new_authority() ? "yes" : "no") << "\n";
  out << render_definition(record->definition);
  return out.str();
}

std::vector<DecisionRecord> CoordinatorService::inspect_decisions(std::uint64_t offset, std::size_t limit) const {
  return history_.page(offset, limit);
}

std::string CoordinatorService::inspect_decisions_text(std::uint64_t offset, std::size_t limit) const {
  const std::vector<DecisionRecord> page = history_.page(offset, limit);
  std::ostringstream out;
  out << "decisions: " << page.size() << " from ordinal " << offset << "\n";
  for (const DecisionRecord& record : page) {
    out << "  [" << record.decision_sequence.value() << "] " << record.summary() << "\n";
  }
  const auto histogram = history_.outcome_histogram();
  out << "outcomes:";
  for (const auto& entry : histogram) {
    out << " " << entry.first << "=" << entry.second;
  }
  out << "\n";
  return out.str();
}

std::string CoordinatorService::inspect_peers() const {
  const std::vector<PeerLiveness> snapshot = authority_.peers_snapshot();
  std::ostringstream out;
  out << "peers: " << snapshot.size() << "\n";
  for (const PeerLiveness& peer : snapshot) {
    out << "  " << peer.participant.to_string() << " incarnation=" << peer.incarnation.to_string()
        << " state=" << to_string(peer.state) << " node="
        << (peer.node.is_some() ? peer.node.to_string() : std::string("-"))
        << " expires_at=" << peer.expires_at_monotonic_ms << "\n";
  }
  return out.str();
}

std::string CoordinatorService::inspect_policy() const {
  const TrafficPolicy policy = authority_.policy_snapshot();
  std::ostringstream out;
  if (policy.generation.is_none()) {
    out << "policy: none installed\n";
    return out.str();
  }
  out << "policy: " << policy.generation.to_string() << "\n";
  for (const TrafficClassPolicy& entry : policy.class_policies) {
    out << "  class=" << to_string(entry.traffic_class) << " priority=" << entry.priority
        << " floor=" << entry.minimum_bandwidth_bps
        << " ceiling=" << (entry.maximum_bandwidth_bps == kUnlimitedRate
                               ? std::string("unlimited")
                               : std::to_string(entry.maximum_bandwidth_bps))
        << " isolation=" << to_string(entry.isolation)
        << (entry.requires_evidence_freshness ? " fresh-evidence" : " evidence-optional") << "\n";
  }
  for (const CollectiveClassPolicy& entry : policy.collective_policies) {
    out << "  collective=" << to_string(entry.collective_class)
        << " traffic=" << to_string(entry.traffic_class)
        << (entry.admit_when_unknown_semantics ? " admits-unknown" : " refuses-unknown")
        << (entry.allow_member_override ? " override-allowed" : " override-refused")
        << " freshness_ms=" << entry.freshness_requirement_ms << "\n";
  }
  return out.str();
}

std::string CoordinatorService::inspect_topology() const {
  const TopologyEvidence topology = authority_.topology_snapshot();
  std::ostringstream out;
  if (topology.generation.is_none()) {
    out << "topology: none installed\n";
    return out.str();
  }
  out << "topology: " << topology.generation.to_string()
      << (topology.synthetic ? " (SYNTHETIC)" : " (supplied by the deployment)") << "\n";
  out << "  nodes: " << topology.nodes.size() << " links: " << topology.links.size()
      << " captured_monotonic_ms: " << topology.captured_monotonic_ms << "\n";
  std::uint64_t capacity = 0;
  for (const TopologyLink& link : topology.links) capacity += link.capacity_bps;
  out << "  aggregate_link_capacity_bps: " << capacity << "\n";
  return out.str();
}

bool CoordinatorService::explain(CollectiveId id, CollectiveAttemptId attempt, DecisionRecord& out_decision,
                                 std::string& out_text) const {
  DecisionRecord found;
  const HistoryStatus status = history_.find_attempt(id, attempt, found);
  if (status != HistoryStatus::kOk) {
    out_text = std::string("no retained decision exists for collective ") + id.to_string() +
               " attempt " + (attempt.is_some() ? attempt.to_string() : std::string("(latest)")) +
               "; history status: " + std::string(to_string(status)) + "\n";
    return false;
  }
  out_decision = found;
  out_text = std::string("decision_ordinal: ") + std::to_string(found.decision_sequence.value()) + "\n" +
             found.explain();
  return true;
}

bool CoordinatorService::explain_correlation(std::uint64_t correlation, DecisionRecord& out_decision,
                                             std::string& out_text) const {
  DecisionRecord found;
  if (history_.find_correlation(correlation, found) == HistoryStatus::kOk) {
    out_decision = found;
    out_text = std::string("decision_ordinal: ") + std::to_string(found.decision_sequence.value()) + "\n" +
               found.explain();
    return true;
  }
  out_text = "no retained decision matches that correlation\n";
  return false;
}

Status CoordinatorService::configure_persistence(const std::string& path, std::uint64_t now_monotonic_ms) {
  std::lock_guard<std::mutex> guard(mutex_);
  persistence_path_ = path;
  // Configuring persistence is not a durable mutation: nothing has changed yet, so
  // the coordinator does not write a snapshot until something does.  A restart
  // that loads a snapshot and then changes nothing therefore leaves the file
  // exactly as it found it, which is what makes the file auditable.
  persistence_dirty_ = false;
  last_flush_monotonic_ms_ = now_monotonic_ms;
  return Status::ok();
}

Status CoordinatorService::restore(const std::string& path, std::uint64_t now_monotonic_ms,
                                   LoadDisposition& disposition) {
  SnapshotStore store(path);
  SnapshotContents contents;
  std::string diagnostic;
  disposition = store.load(contents, &diagnostic);
  if (disposition == LoadDisposition::kMissing) {
    return Status(ErrorCode::kPersistenceOpenFailed, "no snapshot file exists at the configured path");
  }
  if (disposition != LoadDisposition::kLoaded) {
    return Status(ErrorCode::kPersistenceCorrupt,
                  diagnostic.empty() ? "snapshot rejected" : diagnostic);
  }
  std::lock_guard<std::mutex> guard(mutex_);
  Status status = authority_.restore_durable(contents.authority, now_monotonic_ms);
  if (!status.is_ok()) {
    disposition = LoadDisposition::kRejectedImpossible;
    return status;
  }
  status = catalog_.load(contents.records, now_monotonic_ms);
  if (!status.is_ok()) {
    disposition = LoadDisposition::kRejectedImpossible;
    return status;
  }
  persistence_path_ = path;
  persistence_dirty_ = false;
  ++counters_.restores;
  return Status::ok();
}

Status CoordinatorService::flush(std::uint64_t now_monotonic_ms) {
  SnapshotContents contents;
  std::string path;
  std::uint64_t captured_sequence = 0;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (persistence_path_.empty()) {
      return Status(ErrorCode::kPersistenceDirectoryUnavailable, "no persistence path is configured");
    }
    if (!persistence_dirty_) {
      return Status::ok();
    }
    contents.written_monotonic_ms = now_monotonic_ms;
    contents.authority = authority_.durable_snapshot();
    contents.records = catalog_.export_all();
    path = persistence_path_;
    captured_sequence = mutation_sequence_;
    last_flush_monotonic_ms_ = now_monotonic_ms;
  }
  // The write happens with no lock held, so a concurrent registration never waits
  // on disk latency.
  SnapshotStore store(path);
  const Status status = store.save(contents, now_monotonic_ms);
  if (!status.is_ok()) {
    return status;
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (mutation_sequence_ == captured_sequence) {
      persistence_dirty_ = false;
    }
    // Otherwise a mutation landed while the file was being written: the flag
    // stays set so the next flush captures it, and the write that just completed
    // is simply one revision behind rather than silently lossy.
    ++counters_.persists;
  }
  return Status::ok();
}

bool CoordinatorService::persistence_configured() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return !persistence_path_.empty();
}

std::string CoordinatorService::persistence_path() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return persistence_path_;
}

SnapshotContents CoordinatorService::snapshot_contents(std::uint64_t now_monotonic_ms) const {
  SnapshotContents contents;
  contents.written_monotonic_ms = now_monotonic_ms;
  contents.authority = authority_.durable_snapshot();
  contents.records = catalog_.export_all();
  return contents;
}

CoordinatorService::Counters CoordinatorService::counters() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return counters_;
}

void CoordinatorService::reset_counters() {
  std::lock_guard<std::mutex> guard(mutex_);
  counters_ = Counters{};
}

ServiceConfig CoordinatorService::config() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return config_;
}

void CoordinatorService::set_config(const ServiceConfig& config) {
  std::lock_guard<std::mutex> guard(mutex_);
  config_ = config;
  history_.set_capacity(config_.decision_history_capacity);
}

}  // namespace ctf
