// Collective Traffic Fabric - synchronization sensitivity derivation.
// Copyright 2026 Summon Software Labs.
#include "ctf/sync.hpp"

namespace ctf {
namespace {

bool class_is_barrier_like(CollectiveClass value) noexcept {
  switch (value) {
    case CollectiveClass::kAllReduce:
    case CollectiveClass::kReduceScatter:
    case CollectiveClass::kBarrierOnly:
    case CollectiveClass::kScan:
      return true;
    default:
      return false;
  }
}

bool pattern_is_barrier_like(FlowPattern value) noexcept {
  switch (value) {
    case FlowPattern::kRing:
    case FlowPattern::kPipeline:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string_view to_string(SyncKind value) noexcept {
  switch (value) {
    case SyncKind::kNone: return "none";
    case SyncKind::kBarrier: return "barrier";
    case SyncKind::kFence: return "fence";
    case SyncKind::kDependency: return "dependency";
    case SyncKind::kCollectiveStep: return "collective_step";
    case SyncKind::kStreamOrder: return "stream_order";
  }
  return "none";
}

std::string_view to_string(SyncBasis value) noexcept {
  switch (value) {
    case SyncBasis::kUnspecified: return "unspecified";
    case SyncBasis::kDeclaredBarrierSemantics: return "declared_barrier_semantics";
    case SyncBasis::kDeclaredPhaseSynchronizationPoint: return "declared_phase_synchronization_point";
    case SyncBasis::kDerivedFromClass: return "derived_from_class";
    case SyncBasis::kDerivedFromPattern: return "derived_from_pattern";
    case SyncBasis::kDerivedFromPolicy: return "derived_from_policy";
  }
  return "unspecified";
}

SyncSensitivity derive_sync_sensitivity(const CollectiveDefinition& definition, const FlowGroup& group,
                                        const TrafficPolicy& policy) {
  SyncSensitivity sensitivity;
  sensitivity.barrier_member_count = static_cast<std::uint32_t>(
      definition.participants.size() > 0xFFFFFFFFull ? 0xFFFFFFFFull : definition.participants.size());

  const CollectiveClassPolicy* collective_policy = policy.find_collective(definition.collective_class);
  const TrafficClass traffic_class =
      collective_policy != nullptr ? collective_policy->traffic_class : TrafficClass::kUnclassified;
  const TrafficClassPolicy* class_policy = policy.find_class(traffic_class);
  if (class_policy != nullptr) {
    sensitivity.latency_critical = class_policy->priority >= 12 ||
                                   class_policy->traffic_class == TrafficClass::kLatencyCritical;
  }

  // Basis precedence is fixed and documented: an explicit declaration wins over
  // a declared phase synchronization point, which wins over structural
  // inference.  The chosen basis is always reported, so an inference is never
  // presented as a declaration.
  if (definition.declares_barrier_semantics) {
    sensitivity.kind = SyncKind::kBarrier;
    sensitivity.basis = SyncBasis::kDeclaredBarrierSemantics;
    sensitivity.all_members_must_arrive = true;
    sensitivity.completion_ordering_matters = true;
    sensitivity.maximum_skew_micros = 1000;
    return sensitivity;
  }

  for (const CollectivePhase& phase : definition.phases) {
    if (phase.synchronization_point) {
      sensitivity.kind = SyncKind::kBarrier;
      sensitivity.basis = SyncBasis::kDeclaredPhaseSynchronizationPoint;
      sensitivity.all_members_must_arrive = true;
      sensitivity.completion_ordering_matters = true;
      sensitivity.maximum_skew_micros = 5000;
      return sensitivity;
    }
  }

  if (definition.collective_class == CollectiveClass::kBarrierOnly) {
    sensitivity.kind = SyncKind::kBarrier;
    sensitivity.basis = SyncBasis::kDerivedFromClass;
    sensitivity.all_members_must_arrive = true;
    sensitivity.completion_ordering_matters = true;
    sensitivity.maximum_skew_micros = 1000;
    return sensitivity;
  }

  if (class_is_barrier_like(definition.collective_class)) {
    sensitivity.kind = SyncKind::kDependency;
    sensitivity.basis = SyncBasis::kDerivedFromClass;
    sensitivity.completion_ordering_matters = true;
    sensitivity.maximum_skew_micros = 20000;
  } else if (pattern_is_barrier_like(group.pattern)) {
    sensitivity.kind = SyncKind::kCollectiveStep;
    sensitivity.basis = SyncBasis::kDerivedFromPattern;
    sensitivity.maximum_skew_micros = 50000;
  }

  if (group.requires_simultaneous_start && sensitivity.kind == SyncKind::kNone) {
    sensitivity.kind = SyncKind::kDependency;
    sensitivity.basis = SyncBasis::kDerivedFromPolicy;
    sensitivity.completion_ordering_matters = true;
  }
  if (sensitivity.latency_critical && sensitivity.kind == SyncKind::kNone) {
    sensitivity.kind = SyncKind::kFence;
    sensitivity.basis = SyncBasis::kDerivedFromPolicy;
  }
  return sensitivity;
}

}  // namespace ctf
