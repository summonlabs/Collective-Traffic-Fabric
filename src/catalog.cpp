// Collective Traffic Fabric - durable collective catalog.
// Copyright 2026 Summon Software Labs.
#include "ctf/catalog.hpp"

#include <algorithm>

namespace ctf {
namespace {

// A definition is "the same" when every durable field that determines traffic
// treatment agrees.  The generation is deliberately excluded: it is assigned by
// the catalog, not supplied by the caller.
bool definitions_equivalent(const CollectiveDefinition& a, const CollectiveDefinition& b) noexcept {
  return a.id == b.id && a.collective_class == b.collective_class &&
         a.algorithm_hint == b.algorithm_hint && a.scope == b.scope && a.label == b.label &&
         a.origin_node == b.origin_node && a.fabric == b.fabric && a.participants == b.participants &&
         a.phases == b.phases && a.steps == b.steps &&
         a.metadata.entries() == b.metadata.entries() &&
         a.declares_barrier_semantics == b.declares_barrier_semantics &&
         a.logical_bytes == b.logical_bytes;
}

Status validate_definition(const CollectiveDefinition& definition) {
  if (definition.id.is_none()) {
    return Status(ErrorCode::kValidationGenerationUnset, "collective definition has no identity");
  }
  if (!is_known_collective_class(definition.collective_class) &&
      definition.collective_class != CollectiveClass::kUnknown &&
      definition.collective_class != CollectiveClass::kUnknownVendor) {
    return Status(ErrorCode::kValidationClassUnknownRejected, "collective definition has an invalid class");
  }
  if (definition.label.size() > limits::kLabelBytesMax) {
    return Status(ErrorCode::kValidationLabelTooLong, "collective label exceeds the bound");
  }
  if (definition.participants.empty()) {
    return Status(ErrorCode::kValidationParticipantCountOutOfRange, "collective has no participants");
  }
  if (definition.participants.size() > limits::kParticipantsMaxPerCollective) {
    return Status(ErrorCode::kValidationParticipantCountOutOfRange, "collective exceeds the participant bound");
  }
  if (!participants_are_canonical(definition.participants)) {
    return Status(ErrorCode::kDecodeNonCanonical, "collective participants are not canonical");
  }
  for (const ParticipantId& participant : definition.participants) {
    if (participant.is_none()) {
      return Status(ErrorCode::kValidationParticipantUnset, "collective contains an unset participant");
    }
  }
  if (definition.phases.size() > limits::kPhasesMaxPerCollective) {
    return Status(ErrorCode::kValidationPhaseOutOfRange, "collective exceeds the phase bound");
  }
  for (std::size_t index = 0; index < definition.phases.size(); ++index) {
    const CollectivePhase& phase = definition.phases[index];
    if (phase.phase_index.is_unset() || phase.phase_index.value() > limits::kPhasesMaxPerCollective) {
      return Status(ErrorCode::kValidationPhaseOutOfRange, "collective phase index out of range");
    }
    if (phase.label.size() > limits::kLabelBytesMax) {
      return Status(ErrorCode::kValidationLabelTooLong, "collective phase label exceeds the bound");
    }
    if (index > 0 && !(definition.phases[index - 1].phase_index < phase.phase_index)) {
      return Status(ErrorCode::kDecodeNonCanonical, "collective phases are not strictly ascending");
    }
  }
  if (definition.steps.size() > limits::kPhasesMaxPerCollective * limits::kStepsMaxPerPhase) {
    return Status(ErrorCode::kValidationStepOutOfRange, "collective exceeds the step bound");
  }
  for (std::size_t index = 0; index < definition.steps.size(); ++index) {
    const CollectiveStep& step = definition.steps[index];
    if (step.phase_index.is_unset() || step.step_index.is_unset()) {
      return Status(ErrorCode::kValidationStepOutOfRange, "collective step is missing its indices");
    }
    if (step.step_index.value() > limits::kStepsMaxPerPhase) {
      return Status(ErrorCode::kValidationStepOutOfRange, "collective step index out of range");
    }
    if (index > 0) {
      const CollectiveStep& previous = definition.steps[index - 1];
      if (previous.phase_index > step.phase_index) {
        return Status(ErrorCode::kDecodeNonCanonical, "collective steps are not ordered by phase");
      }
      if (previous.phase_index == step.phase_index && !(previous.step_index < step.step_index)) {
        return Status(ErrorCode::kDecodeNonCanonical, "collective steps are not strictly ascending");
      }
    }
  }
  return Status::ok();
}

}  // namespace

std::string_view to_string(RegistrationResult value) noexcept {
  switch (value) {
    case RegistrationResult::kRegistered: return "registered";
    case RegistrationResult::kGenerationAdvanced: return "generation_advanced";
    case RegistrationResult::kUnchanged: return "unchanged";
    case RegistrationResult::kRejectedInvalid: return "rejected_invalid";
    case RegistrationResult::kRejectedTerminal: return "rejected_terminal";
    case RegistrationResult::kRejectedCapacity: return "rejected_capacity";
  }
  return "rejected_invalid";
}

CollectiveCatalog::CollectiveCatalog(std::uint32_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

RegistrationOutcome CollectiveCatalog::register_definition(const CollectiveDefinition& definition,
                                                           std::uint64_t now_monotonic_ms) {
  RegistrationOutcome outcome;
  Status status = validate_definition(definition);
  if (!status.is_ok()) {
    outcome.result = RegistrationResult::kRejectedInvalid;
    outcome.reasons.add("definition_invalid", status.message(), definition.id);
    return outcome;
  }

  auto existing = records_.find(definition.id);
  if (existing == records_.end()) {
    if (records_.size() >= capacity_) {
      outcome.result = RegistrationResult::kRejectedCapacity;
      outcome.reasons.add("catalog_capacity_exhausted",
                          "the live collective bound has been reached; retire definitions first",
                          definition.id);
      return outcome;
    }
    CollectiveRecord record;
    record.definition = definition;
    ++mutation_sequence_;
    record.definition.registration_sequence = mutation_sequence_;
    record.registration_sequence = mutation_sequence_;
    record.last_mutation_sequence = mutation_sequence_;
    record.state = CollectiveState::kRegistered;
    record.registered_monotonic_ms = now_monotonic_ms;
    record.last_mutated_monotonic_ms = now_monotonic_ms;
    record.generation_ancestry_depth = 1;
    CollectiveGeneration generation = definition.generation;
    if (generation.is_none()) {
      IdentityMinter minter(static_cast<std::uint64_t>(mutation_sequence_.value()) ^ 0xC0FFEEull);
      generation = minter.next();
      if (generation.is_none()) generation = Identity(1, 1);
    }
    record.definition.generation = generation;
    outcome.result = RegistrationResult::kRegistered;
    outcome.generation = generation;
    outcome.created = true;
    outcome.reasons.add("registered", "new durable collective definition stored", definition.id);
    records_.emplace(definition.id, std::move(record));
    return outcome;
  }

  CollectiveRecord& record = existing->second;
  if (collective_state_is_terminal(record.state)) {
    outcome.result = RegistrationResult::kRejectedTerminal;
    outcome.reasons.add("definition_terminal",
                        std::string("collective is ") + std::string(to_string(record.state)) +
                            "; a terminal definition is never resurrected",
                        definition.id);
    return outcome;
  }

  if (definitions_equivalent(record.definition, definition)) {
    outcome.result = RegistrationResult::kUnchanged;
    outcome.generation = record.definition.generation;
    outcome.attempt = record.current_attempt;
    outcome.reasons.add("unchanged", "re-registration matched the stored definition exactly", definition.id);
    return outcome;
  }

  // Every membership or semantics change mints a new generation, which
  // immediately stales every decision bound to the previous one.
  CollectiveDefinition updated = definition;
  IdentityMinter minter(static_cast<std::uint64_t>(mutation_sequence_.value() + 1) ^ 0xBEEFULL);
  CollectiveGeneration generation = minter.next();
  if (generation.is_none()) generation = Identity(1, 1);
  updated.generation = generation;
  ++mutation_sequence_;
  updated.registration_sequence = mutation_sequence_;
  record.definition = updated;
  record.state = CollectiveState::kRegistered;
  record.registration_sequence = mutation_sequence_;
  record.last_mutation_sequence = mutation_sequence_;
  record.last_mutated_monotonic_ms = now_monotonic_ms;
  record.current_attempt = CollectiveAttemptId{};
  record.attempt_sequence = Sequence{};
  ++record.generation_ancestry_depth;
  outcome.result = RegistrationResult::kGenerationAdvanced;
  outcome.generation = generation;
  outcome.advanced = true;
  outcome.reasons.add("generation_advanced",
                      "definition changed; a new generation was minted and previous decisions are stale",
                      definition.id);
  return outcome;
}

Status CollectiveCatalog::begin_attempt(CollectiveId id, CollectiveAttemptId attempt,
                                        std::uint64_t now_monotonic_ms, CollectiveAttemptId& out_previous) {
  if (attempt.is_none()) {
    return Status(ErrorCode::kValidationAttemptUnset, "attempt identity is unset");
  }
  auto existing = records_.find(id);
  if (existing == records_.end()) {
    return Status(ErrorCode::kStateCollectiveUnknown, "collective is not registered");
  }
  CollectiveRecord& record = existing->second;
  if (!record.accepts_new_authority()) {
    return Status(ErrorCode::kStateNotLive,
                  std::string("collective is ") + std::string(to_string(record.state)) +
                      " and does not accept a new attempt");
  }
  if (record.current_attempt == attempt) {
    return Status(ErrorCode::kCompletionDuplicate, "attempt identity is already current");
  }
  out_previous = record.current_attempt;
  record.current_attempt = attempt;
  record.attempt_sequence = Sequence(record.attempt_sequence.value() + 1);
  record.state = CollectiveState::kPlanning;
  record.last_mutated_monotonic_ms = now_monotonic_ms;
  ++mutation_sequence_;
  record.last_mutation_sequence = mutation_sequence_;
  return Status::ok();
}

Status CollectiveCatalog::set_state(CollectiveId id, CollectiveState state, std::uint64_t now_monotonic_ms) {
  auto existing = records_.find(id);
  if (existing == records_.end()) {
    return Status(ErrorCode::kStateCollectiveUnknown, "collective is not registered");
  }
  CollectiveRecord& record = existing->second;
  if (collective_state_is_terminal(record.state) && !collective_state_is_terminal(state)) {
    return Status(ErrorCode::kStateCollectiveRetired,
                  "a terminal collective is never moved back to a live state");
  }
  record.state = state;
  record.last_mutated_monotonic_ms = now_monotonic_ms;
  ++mutation_sequence_;
  record.last_mutation_sequence = mutation_sequence_;
  return Status::ok();
}

Status CollectiveCatalog::retire(CollectiveId id, std::uint64_t now_monotonic_ms) {
  return set_state(id, CollectiveState::kRetired, now_monotonic_ms);
}

Status CollectiveCatalog::cancel(CollectiveId id, std::uint64_t now_monotonic_ms) {
  return set_state(id, CollectiveState::kCancelled, now_monotonic_ms);
}

const CollectiveRecord* CollectiveCatalog::find(CollectiveId id) const noexcept {
  auto existing = records_.find(id);
  if (existing == records_.end()) return nullptr;
  return &existing->second;
}

CollectiveRecord* CollectiveCatalog::find_mutable(CollectiveId id) noexcept {
  auto existing = records_.find(id);
  if (existing == records_.end()) return nullptr;
  return &existing->second;
}

bool CollectiveCatalog::contains(CollectiveId id) const noexcept { return records_.find(id) != records_.end(); }

std::vector<CollectiveId> CollectiveCatalog::ids(std::size_t offset, std::size_t limit) const {
  std::vector<CollectiveId> all;
  all.reserve(records_.size());
  for (const auto& entry : records_) all.push_back(entry.first);
  std::sort(all.begin(), all.end());
  std::vector<CollectiveId> page;
  if (offset >= all.size() || limit == 0) return page;
  const std::size_t end = std::min(all.size(), offset + limit);
  page.reserve(end - offset);
  for (std::size_t index = offset; index < end; ++index) page.push_back(all[index]);
  return page;
}

Status CollectiveCatalog::load(const std::vector<CollectiveRecord>& records, std::uint64_t now_monotonic_ms) {
  if (records.size() > capacity_) {
    return Status(ErrorCode::kPersistenceImpossibleState, "snapshot holds more collectives than the live bound");
  }
  // Validate everything before touching the live map: a rejected load must
  // never leave a partially applied state behind.
  IdentityMap<CollectiveRecord> staged;
  staged.reserve(records.size() * 2);
  Sequence highest;
  for (const CollectiveRecord& record : records) {
    Status status = validate_definition(record.definition);
    if (!status.is_ok()) {
      return Status(ErrorCode::kPersistenceImpossibleState,
                    std::string("snapshot holds an invalid definition: ") + status.message());
    }
    if (record.definition.generation.is_none()) {
      return Status(ErrorCode::kPersistenceImpossibleState, "snapshot holds a definition without a generation");
    }
    if (record.state == CollectiveState::kUnregistered) {
      return Status(ErrorCode::kPersistenceImpossibleState, "snapshot holds an unregistered collective");
    }
    if (!staged.emplace(record.definition.id, record).second) {
      return Status(ErrorCode::kPersistenceImpossibleState, "snapshot holds a duplicate collective identity");
    }
    highest = Sequence(std::max(highest.value(), record.registration_sequence.value()));
    highest = Sequence(std::max(highest.value(), record.last_mutation_sequence.value()));
  }
  records_ = std::move(staged);
  mutation_sequence_ = highest;
  static_cast<void>(now_monotonic_ms);
  return Status::ok();
}

std::vector<CollectiveRecord> CollectiveCatalog::export_all() const {
  std::vector<CollectiveRecord> out;
  out.reserve(records_.size());
  for (const auto& entry : records_) out.push_back(entry.second);
  std::sort(out.begin(), out.end(), [](const CollectiveRecord& a, const CollectiveRecord& b) {
    return a.definition.id < b.definition.id;
  });
  return out;
}

void CollectiveCatalog::clear() {
  records_.clear();
  mutation_sequence_ = Sequence{};
}

}  // namespace ctf
