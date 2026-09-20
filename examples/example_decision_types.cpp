// Collective Traffic Fabric - decision types example.
// Copyright 2026 Summon Software Labs.
//
// This example runs a complete, real decision path in process: a SYNTHETIC
// multi-rack fabric, the standard traffic policy, synthetic capacity and
// congestion observations, one registered all_reduce collective, one attempt
// and one ring flow group.  It then shows two deterministic refusals: a request
// bound to a superseded collective generation, and a collective whose class has
// no semantics this runtime understands.
//
// What repeats between runs is the STRUCTURAL output: outcome, authority axis,
// error code, traffic class, priority, isolation, the pacing numbers, the reason
// codes and the generation relationships.  Identities and the coordinator epoch
// are minted when the program runs, exactly as a deployment mints them, so the
// library text printed underneath them is this run's text and no more.
//
// Nothing here is measured on real hardware: every fabric, capacity and
// congestion claim is labelled SYNTHETIC, and one fixed synthetic clock value
// drives every call so that the decisions themselves are identical run to run.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ctf/catalog.hpp"
#include "ctf/collective.hpp"
#include "ctf/decision.hpp"
#include "ctf/evidence.hpp"
#include "ctf/flow_group.hpp"
#include "ctf/identity.hpp"
#include "ctf/participant.hpp"
#include "ctf/service.hpp"
#include "ctf/traffic.hpp"

namespace {

using ctf::CollectiveClass;
using ctf::CollectiveDefinition;
using ctf::CollectiveInstance;
using ctf::DecisionRecord;
using ctf::DecisionRequest;
using ctf::FlowEdge;
using ctf::FlowGroupPlan;
using ctf::FlowPattern;
using ctf::Identity;
using ctf::NodeId;
using ctf::ParticipantId;
using ctf::ParticipantState;
using ctf::PolicyGeneration;
using ctf::ReasonChain;
using ctf::RegistrationOutcome;
using ctf::Sequence;
using ctf::Status;
using ctf::to_string;

// The single synthetic clock the whole example runs on.  A deployment would
// supply ctf::transport::monotonic_now_ms() here; a fixed value keeps the
// decisions reproducible without reading any wall clock.
inline constexpr std::uint64_t kSyntheticClockMs = 1000;

// SYNTHETIC fabric shape: two racks of two nodes joined by two oversubscribed
// inter-rack uplinks, with the availability and congestion below modelled
// rather than measured.
inline constexpr std::uint64_t kLinkCapacityBps = 400ull * 1000 * 1000 * 1000ull;
inline constexpr std::uint32_t kRackCount = 2;
inline constexpr std::uint32_t kNodesPerRack = 2;
inline constexpr std::uint32_t kUtilizationBps = 2000;
inline constexpr std::uint32_t kAvailablePerMille = 750;
inline constexpr std::uint64_t kCongestionWindowMs = 2000;
inline constexpr std::uint64_t kCollectiveBytes = 1ull << 30;
inline constexpr std::uint64_t kEdgeBytes = 1ull << 28;

void emit(const std::string& line) {
  std::fputs(line.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

int fail(const std::string& message) {
  std::fputs(("example: " + message + "\n").c_str(), stderr);
  std::fflush(stderr);
  return 1;
}

[[nodiscard]] const char* yes_no(bool value) { return value ? "yes" : "no"; }

// A generation relationship, never a generation value: two runs mint different
// identities, so the readable fact is whether a decision bound what the request
// observed.
[[nodiscard]] std::string relationship(Identity bound, Identity observed) {
  if (bound.is_none()) return "not_bound";
  if (observed.is_none()) return "no_observation_to_compare";
  return bound == observed ? "matches_the_snapshot" : "differs_from_the_snapshot";
}

// The reason codes alone, in order.  Codes are stable strings; the subjects the
// full chain carries are identities and are therefore not part of this line.
[[nodiscard]] std::string reason_codes(const ReasonChain& chain) {
  std::string out;
  for (const ctf::Reason& reason : chain.reasons()) {
    if (!out.empty()) out += " -> ";
    out += reason.code;
  }
  return out.empty() ? std::string("none") : out;
}

struct SyntheticFabric {
  ctf::TopologyEvidence topology;
  ctf::CapacityEvidence capacity;
  ctf::CongestionEvidence congestion;
  std::vector<NodeId> nodes;
};

// Builds the SYNTHETIC fabric.  Every identity is minted here, as a fabric
// observer would mint the identities it learned from the hardware.
[[nodiscard]] SyntheticFabric make_synthetic_fabric(std::uint64_t now_ms) {
  SyntheticFabric fabric;
  std::vector<Identity> racks;
  for (std::uint32_t index = 0; index < kRackCount; ++index) racks.push_back(ctf::mint_identity());
  std::vector<Identity> switches;
  for (std::uint32_t index = 0; index < kRackCount + 1; ++index) {
    switches.push_back(ctf::mint_identity());
  }
  for (std::uint32_t rack = 0; rack < kRackCount; ++rack) {
    for (std::uint32_t node = 0; node < kNodesPerRack; ++node) {
      ctf::TopologyNode entry;
      entry.id = ctf::mint_identity();
      entry.rack = racks[rack];
      entry.accelerator_attached = true;
      entry.accelerator_count = 8;
      fabric.topology.nodes.push_back(entry);
      fabric.nodes.push_back(entry.id);
    }
  }
  for (std::uint32_t rack = 0; rack < kRackCount; ++rack) {
    ctf::TopologyLink link;
    link.id = ctf::mint_identity();
    link.a = switches[rack];
    link.b = switches.back();
    link.link_class = ctf::LinkClass::kInterRack;
    link.capacity_bps = kLinkCapacityBps;
    link.lanes = 4;
    link.shared_oversubscribed = true;
    fabric.topology.links.push_back(link);
  }
  fabric.topology.generation = ctf::mint_identity();
  fabric.topology.captured_monotonic_ms = now_ms;
  fabric.topology.synthetic = true;
  fabric.topology.canonicalize();

  fabric.capacity.generation = ctf::mint_identity();
  fabric.capacity.topology_generation = fabric.topology.generation;
  fabric.capacity.captured_monotonic_ms = now_ms;
  fabric.capacity.synthetic = true;
  for (const ctf::TopologyLink& link : fabric.topology.links) {
    ctf::LinkCapacity entry;
    entry.link = link.id;
    entry.provisioned_bps = link.capacity_bps;
    entry.available_bps = (link.capacity_bps / 1000ull) * kAvailablePerMille;
    entry.utilization_bps = kUtilizationBps;
    entry.oversubscribed = true;
    entry.oversubscription_ratio_per_mille = 4000;
    fabric.capacity.links.push_back(entry);
  }
  fabric.capacity.canonicalize();

  fabric.congestion.generation = ctf::mint_identity();
  fabric.congestion.topology_generation = fabric.topology.generation;
  fabric.congestion.captured_monotonic_ms = now_ms;
  fabric.congestion.valid_for_ms = kCongestionWindowMs;
  fabric.congestion.synthetic = true;
  for (const ctf::TopologyLink& link : fabric.topology.links) {
    ctf::LinkCongestion entry;
    entry.link = link.id;
    entry.utilization_bps = kUtilizationBps;
    entry.queue_depth_bytes = 0;
    entry.ecn_marks_per_mille = 0;
    entry.pfc_pause_per_mille = 0;
    entry.level = ctf::congestion_level_from_utilization_bps(kUtilizationBps);
    fabric.congestion.links.push_back(entry);
  }
  fabric.congestion.canonicalize();
  return fabric;
}

// Ring plan: member i sends to member i + 1, closing the cycle.  Edges carry the
// volume the planner declared, and the pattern is stated rather than inferred.
[[nodiscard]] FlowGroupPlan make_ring_plan(const CollectiveInstance& instance,
                                           CollectiveClass collective_class,
                                           const std::vector<ParticipantId>& participants) {
  FlowGroupPlan plan;
  plan.instance = instance;
  plan.collective_class = collective_class;
  plan.pattern = FlowPattern::kRing;
  plan.direction = ctf::FlowDirection::kUnidirectional;
  plan.plan_sequence = Sequence(1);
  plan.requires_simultaneous_start = true;
  for (std::size_t index = 0; index < participants.size(); ++index) {
    FlowEdge edge;
    edge.source = participants[index];
    edge.destination = participants[(index + 1) % participants.size()];
    edge.logical_bytes = kEdgeBytes;
    edge.step_index = Sequence(1);
    edge.hint = ctf::AlgorithmHint::kRing;
    edge.crosses_rack = true;
    plan.edges.push_back(edge);
  }
  return plan;
}

// The reproducible part of a decision: no identity, no clock reading, and
// nothing derived from either.
void print_structure(const char* label, const DecisionRecord& decision, const DecisionRequest& request) {
  emit(std::string("--- ") + label);
  emit(std::string("structural: outcome=") + std::string(to_string(decision.outcome)) +
       " authority_axis=" + std::string(to_string(decision.axis)) +
       " error_code=" + std::string(to_string(decision.code)) +
       " collective_class=" + std::string(to_string(decision.collective_class)) +
       " traffic_class=" + std::string(to_string(decision.traffic_class)) +
       " priority=" + std::to_string(decision.priority) +
       " isolation=" + std::string(to_string(decision.isolation)));
  emit(std::string("pacing: mode=") + std::string(to_string(decision.pacing.mode)) +
       " admitted_bandwidth_bps=" + std::to_string(decision.pacing.admitted_bandwidth_bps) +
       " ceiling_bandwidth_bps=" + std::to_string(decision.pacing.ceiling_bandwidth_bps) +
       " burst_bytes=" + std::to_string(decision.pacing.burst_bytes) +
       " utilization_bps=" + std::to_string(decision.pacing.utilization_bps));
  emit(std::string("evidence: capacity_used=") + yes_no(decision.capacity_evidence_used) +
       " congestion_used=" + yes_no(decision.congestion_evidence_used) +
       " all_members_live=" + yes_no(decision.all_members_live) +
       " flow_group_constructed=" + yes_no(decision.flow_group_constructed) +
       " edges=" + std::to_string(decision.group.edges.size()));
  emit(std::string("generations: collective=") +
       relationship(decision.collective_generation, request.instance.generation) +
       " topology=" + relationship(decision.topology_generation, request.observed_topology_generation) +
       " policy=" + relationship(decision.policy_generation, request.observed_policy_generation) +
       " capacity=" + relationship(decision.capacity_generation, request.observed_capacity_generation) +
       " congestion=" +
       relationship(decision.congestion_generation, request.observed_congestion_generation));
  emit("reason_codes: " + reason_codes(decision.reasons));
}

// Everything between the markers is the library's own text for this run: it
// carries the identities the run minted and the coordinator epoch it booted
// with, so it is printed verbatim and never claimed to repeat.  The markers
// make the reproducible section - everything outside them - unambiguous.
void print_per_run_text(const char* label, const DecisionRecord& decision) {
  emit(std::string("per_run_text_begin: ") + label);
  emit("reasons: " + decision.reasons.to_string());
  emit("library_summary: " + decision.summary());
  emit("library_explanation:");
  const std::string text = decision.explain();
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::size_t length = end == std::string::npos ? std::string::npos : end - start;
    emit(text.substr(start, length));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  emit("per_run_text_end");
}

}  // namespace

int main() {
  emit("Collective Traffic Fabric - decision types example");
  emit("synthetic=yes: every fabric, capacity and congestion claim below is SYNTHETIC");
  emit("identities=per_run: identities and the coordinator epoch are minted when this program");
  emit("  runs, exactly as a deployment mints them.  The structural facts and reason codes");
  emit("  printed for each decision are what repeats; the ids underneath them do not.");
  emit("");

  // ---- SYNTHETIC fabric -----------------------------------------------------
  const SyntheticFabric fabric = make_synthetic_fabric(kSyntheticClockMs);
  emit("fabric SYNTHETIC racks=2 nodes=4 inter_rack_links=2 link_capacity_bps=" +
       std::to_string(kLinkCapacityBps));
  emit("capacity SYNTHETIC available_per_mille=" + std::to_string(kAvailablePerMille) +
       " congestion SYNTHETIC utilization_bps=" + std::to_string(kUtilizationBps));
  emit("");

  // ---- coordinator service --------------------------------------------------
  ctf::CoordinatorService service;
  Status status = service.start(kSyntheticClockMs);
  if (!status.is_ok()) return fail(std::string("the coordinator did not start: ") + status.message());

  const PolicyGeneration policy_generation = ctf::mint_identity();
  if (service.authority().install_policy(ctf::TrafficPolicy::standard(policy_generation)) !=
      ctf::InstallResult::kInstalled) {
    return fail("the standard policy was refused");
  }
  emit("policy: standard table installed (generation is this run's)");
  if (service.authority().install_topology(fabric.topology) != ctf::InstallResult::kInstalled) {
    return fail("the SYNTHETIC topology was refused");
  }
  if (service.authority().install_capacity(fabric.capacity) != ctf::InstallResult::kInstalled) {
    return fail("the SYNTHETIC capacity observation was refused");
  }
  if (service.authority().install_congestion(fabric.congestion) != ctf::InstallResult::kInstalled) {
    return fail("the SYNTHETIC congestion observation was refused");
  }
  emit("topology, capacity and congestion evidence installed SYNTHETIC");

  ctf::SessionBinding binding;
  status = service.open_session("example-decision-types", kSyntheticClockMs, binding);
  if (!status.is_ok()) return fail(std::string("no session could be opened: ") + status.message());
  ctf::ServiceRequestContext context;
  context.session = binding.id;
  context.incarnation = binding.incarnation;
  context.epoch = binding.epoch;
  context.now_monotonic_ms = kSyntheticClockMs;
  context.correlation = 1;

  std::vector<ParticipantId> participants;
  for (const NodeId& node : fabric.nodes) {
    const ParticipantId participant = ctf::mint_identity();
    status =
        service.publish_participant(context, participant, ctf::mint_identity(), node,
                                    ParticipantState::kLive);
    if (!status.is_ok()) return fail(std::string("a participant was refused: ") + status.message());
    participants.push_back(participant);
  }
  emit("participants_live=" + std::to_string(participants.size()) +
       " each on its own SYNTHETIC node");
  emit("");

  // ---- the supported path ---------------------------------------------------
  CollectiveDefinition definition;
  definition.id = ctf::mint_identity();
  definition.collective_class = CollectiveClass::kAllReduce;
  definition.algorithm_hint = ctf::AlgorithmHint::kRing;
  definition.label = "synthetic-all-reduce";
  definition.participants = participants;
  // Canonical order, so two peers that state the same membership state the same
  // definition.
  ctf::canonicalize_participants(definition.participants);
  definition.logical_bytes = kCollectiveBytes;

  RegistrationOutcome registration;
  status = service.register_collective(context, definition, registration);
  if (!status.is_ok()) {
    return fail(std::string("the collective was not registered: ") + status.message());
  }
  if (registration.generation.is_none()) {
    return fail("the collective was registered without a generation");
  }
  emit("collective registered class=all_reduce declared_bytes=" + std::to_string(kCollectiveBytes));

  const Identity attempt = ctf::mint_identity();
  ctf::AttemptOutcome attempt_outcome;
  status = service.begin_attempt(context, definition.id, registration.generation, attempt,
                                 kCollectiveBytes, attempt_outcome);
  if (!status.is_ok()) return fail(std::string("the attempt did not begin: ") + status.message());
  emit("attempt begun state=" + std::string(to_string(attempt_outcome.state)));

  DecisionRequest request;
  request.instance.id = definition.id;
  request.instance.generation = registration.generation;
  request.instance.attempt = attempt;
  request.observed_epoch = service.authority().epoch();
  request.observed_policy_generation = service.authority().policy_generation();
  request.observed_topology_generation = service.authority().topology_generation();
  request.observed_capacity_generation = service.authority().capacity_generation();
  request.observed_congestion_generation = service.authority().congestion_generation();
  request.has_flow_group_plan = true;
  request.plan = make_ring_plan(request.instance, definition.collective_class, definition.participants);

  DecisionRecord decision;
  status = service.plan_flow_group(context, request, decision);
  if (!status.is_ok()) return fail(std::string("the decision was not issued: ") + status.message());
  emit("");
  emit("admission: all_reduce over a ring on the SYNTHETIC fabric");
  print_structure("admitted_decision", decision, request);
  print_per_run_text("admitted_decision", decision);
  emit("");

  // ---- refusal 1: a superseded generation -----------------------------------
  // Changing the declared volume mints a new generation, which stales every
  // decision and every request bound to the previous one.  The refusal below is
  // therefore about the generation, not about the plan.
  CollectiveDefinition changed = definition;
  changed.logical_bytes = definition.logical_bytes + kEdgeBytes;
  RegistrationOutcome advanced;
  status = service.register_collective(context, changed, advanced);
  if (!status.is_ok()) {
    return fail(std::string("the changed definition was refused: ") + status.message());
  }
  if (!advanced.advanced) return fail("a changed definition did not advance the generation");

  DecisionRequest stale = request;
  stale.instance.generation = registration.generation;
  stale.plan = make_ring_plan(stale.instance, definition.collective_class, definition.participants);
  DecisionRecord stale_decision;
  status = service.plan_flow_group(context, stale, stale_decision);
  if (!status.is_ok()) {
    return fail(std::string("the stale request was not decided: ") + status.message());
  }
  emit("refusal 1: a request bound to the generation the changed definition superseded");
  print_structure("refusal_stale_generation", stale_decision, stale);
  print_per_run_text("refusal_stale_generation", stale_decision);
  emit("");

  // ---- refusal 2: a collective class with no known semantics ----------------
  CollectiveDefinition unknown;
  unknown.id = ctf::mint_identity();
  unknown.collective_class = CollectiveClass::kUnknownVendor;
  unknown.label = "synthetic-vendor-specific";
  unknown.participants = definition.participants;
  unknown.logical_bytes = kCollectiveBytes;
  RegistrationOutcome unknown_registration;
  status = service.register_collective(context, unknown, unknown_registration);
  if (!status.is_ok()) {
    return fail(std::string("the vendor collective was refused: ") + status.message());
  }
  const Identity unknown_attempt = ctf::mint_identity();
  ctf::AttemptOutcome unknown_attempt_outcome;
  status = service.begin_attempt(context, unknown.id, unknown_registration.generation, unknown_attempt,
                                 kCollectiveBytes, unknown_attempt_outcome);
  if (!status.is_ok()) {
    return fail(std::string("the vendor attempt did not begin: ") + status.message());
  }

  DecisionRequest unknown_request = request;
  unknown_request.instance.id = unknown.id;
  unknown_request.instance.generation = unknown_registration.generation;
  unknown_request.instance.attempt = unknown_attempt;
  unknown_request.plan =
      make_ring_plan(unknown_request.instance, unknown.collective_class, unknown.participants);
  DecisionRecord unknown_decision;
  status = service.plan_flow_group(context, unknown_request, unknown_decision);
  if (!status.is_ok()) {
    return fail(std::string("the vendor request was not decided: ") + status.message());
  }
  emit("refusal 2: a collective whose class has no semantics this runtime understands");
  print_structure("refusal_unknown_semantics", unknown_decision, unknown_request);
  print_per_run_text("refusal_unknown_semantics", unknown_decision);
  emit("");

  // ---- verdict --------------------------------------------------------------
  const bool admitted = decision.outcome == ctf::Outcome::kAdmitted &&
                        decision.traffic_class == ctf::TrafficClass::kBulkData;
  const bool refusals_hold = ctf::outcome_is_rejection(stale_decision.outcome) &&
                             ctf::outcome_is_rejection(unknown_decision.outcome);
  const Status stopped = service.stop(kSyntheticClockMs);
  if (!stopped.is_ok()) return fail(std::string("the coordinator did not stop: ") + stopped.message());
  if (decision.outcome != ctf::Outcome::kAdmitted) {
    return fail(std::string("the supported path was not ADMITTED: ") +
                std::string(to_string(decision.outcome)));
  }
  if (decision.traffic_class != ctf::TrafficClass::kBulkData) {
    return fail(std::string("all_reduce was admitted without bulk_data treatment: ") +
                std::string(to_string(decision.traffic_class)));
  }
  if (!refusals_hold) {
    return fail("a refusal granted authority; stale and unknown must never be admitted");
  }
  emit("result ADMITTED with bulk_data for the supported path; REJECTED for the stale generation "
       "and the unknown semantics collective");
  static_cast<void>(admitted);
  return 0;
}
