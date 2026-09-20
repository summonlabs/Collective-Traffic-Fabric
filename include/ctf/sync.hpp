// Collective Traffic Fabric - synchronization sensitivity metadata.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_SYNC_HPP
#define CTF_SYNC_HPP

#include <cstdint>
#include <string_view>

#include "ctf/collective.hpp"
#include "ctf/flow_group.hpp"
#include "ctf/traffic.hpp"

namespace ctf {

enum class SyncKind : std::uint8_t {
  kNone = 0,             // no synchronization semantics claimed
  kBarrier = 1,          // every member must arrive
  kFence = 2,            // arrival ordering matters beyond completion
  kDependency = 3,       // producers must finish before consumers start
  kCollectiveStep = 4,   // ordering inside a collective's own step sequence
  kStreamOrder = 5,      // per-stream ordering across collectives
};

[[nodiscard]] std::string_view to_string(SyncKind value) noexcept;

// Synchronization sensitivity is derived from declared semantics, class and
// planned structure.  It always records what it was derived from, so a consumer
// can distinguish an explicit declaration from an inference.
enum class SyncBasis : std::uint8_t {
  kUnspecified = 0,
  kDeclaredBarrierSemantics = 1,
  kDeclaredPhaseSynchronizationPoint = 2,
  kDerivedFromClass = 3,
  kDerivedFromPattern = 4,
  kDerivedFromPolicy = 5,
};

[[nodiscard]] std::string_view to_string(SyncBasis value) noexcept;

struct SyncSensitivity {
  SyncKind kind = SyncKind::kNone;
  SyncBasis basis = SyncBasis::kUnspecified;
  bool latency_critical = false;
  bool all_members_must_arrive = false;
  bool completion_ordering_matters = false;
  std::uint32_t barrier_member_count = 0;
  // Skew the fabric may introduce before the collective's synchronization
  // semantics are compromised.  0 means unspecified.
  std::uint64_t maximum_skew_micros = 0;

  friend bool operator==(const SyncSensitivity& a, const SyncSensitivity& b) {
    return a.kind == b.kind && a.basis == b.basis && a.latency_critical == b.latency_critical &&
           a.all_members_must_arrive == b.all_members_must_arrive &&
           a.completion_ordering_matters == b.completion_ordering_matters &&
           a.barrier_member_count == b.barrier_member_count && a.maximum_skew_micros == b.maximum_skew_micros;
  }
};

// Deterministic derivation.  Consults no clock and no randomness.
[[nodiscard]] SyncSensitivity derive_sync_sensitivity(const CollectiveDefinition& definition,
                                                      const FlowGroup& group,
                                                      const TrafficPolicy& policy);

}  // namespace ctf

#endif  // CTF_SYNC_HPP
