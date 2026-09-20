// Collective Traffic Fabric - the deterministic decision engine.
// Copyright 2026 Summon Software Labs.
//
// The engine is a pure function: it receives a frozen evaluation snapshot and a
// request, and returns a decision.  It takes no locks, reads no clock, performs
// no I/O and mutates nothing.  That property is what makes every decision in
// this runtime reproducible and independently explainable.
#ifndef CTF_ENGINE_HPP
#define CTF_ENGINE_HPP

#include <string>

#include "ctf/decision.hpp"
#include "ctf/error.hpp"
#include "ctf/evidence.hpp"
#include "ctf/flow_group.hpp"
#include "ctf/snapshot.hpp"
#include "ctf/traffic.hpp"

namespace ctf {

// Accounting for one evaluation.  Counters, not timing: they make accidental
// O(N^2) behaviour observable in tests without measuring wall time.
struct EngineCounters {
  std::uint64_t lookups = 0;             // keyed container probes
  std::uint64_t participant_scans = 0;   // linear participant passes
  std::uint64_t edge_scans = 0;          // linear edge passes
  std::uint64_t link_lookups = 0;        // capacity/congestion probes
};

// Evaluates one flow-group request against a snapshot.
//
// Precondition: the caller has already validated the request with
// validate_decision_request() and has checked that the collective definition
// exists, is current and is not terminal.  Those checks depend on mutable
// catalog state that is deliberately not part of the snapshot; they are
// performed by the coordinator, which then reports the corresponding outcome
// itself.  The engine never assumes authority it was not handed.
[[nodiscard]] DecisionRecord evaluate(const EvaluationSnapshot& snapshot, const DecisionRequest& request,
                                      const CollectiveDefinition& definition, EngineCounters* counters = nullptr);

// The deterministic rate computation behind every pacing intent.  Exposed
// separately so that the congestion response table and the capacity clamp can
// be tested directly, without constructing a whole decision.
struct PacingComputation {
  std::uint64_t available_bps = 0;       // capacity observation used (0 -> none)
  std::uint64_t proposed_bps = 0;        // after the congestion response
  std::uint64_t admitted_bps = 0;        // after the class floor and ceiling
  std::uint64_t ceiling_bps = 0;         // class ceiling actually applied
  std::uint64_t burst_bytes = 0;
  std::uint64_t utilization_bps = 0;     // congestion observation used (0 -> none)
  std::uint32_t response_per_mille = 1000;
  bool capacity_used = false;
  bool congestion_used = false;
  ErrorCode code = ErrorCode::kOk;       // kAdmissionRateLimited when the floor cannot be met
  std::string reason_code;
  std::string reason_detail;
};

[[nodiscard]] PacingComputation compute_pacing(const EvaluationSnapshot& snapshot,
                                               const CollectiveDefinition& definition,
                                               const TrafficClassPolicy& class_policy,
                                               const CollectiveClassPolicy& collective_policy,
                                               PacingMode mode);

// Convenience wrapper used by the engine after the decision has been built.
[[nodiscard]] PacingIntent compute_pacing(const EvaluationSnapshot& snapshot, const DecisionRecord& decision,
                                          const TrafficPolicy& policy);

}  // namespace ctf

#endif  // CTF_ENGINE_HPP
