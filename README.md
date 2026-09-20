# Collective Traffic Fabric

Open-source, vendor-neutral C++20 runtime for classifying, governing, scheduling, and
explaining collective communication traffic across distributed AI fabrics with
generation-bound topology, workload, policy, and flow authority.

**Version 1.0.0** - Apache License 2.0 - Copyright 2026 Summon Software Labs - No telemetry transmission.

---

## The question this runtime answers

For a collective communication instance that must traverse the fabric **now**, what traffic
treatment is legally permitted under its collective semantics, participants, topology,
capacity, congestion, priority, and generation-bound authority?

Every admission in this runtime is answered by a pure function over frozen evidence, and every
answer carries the exact generation set that made it legal.

## Exact systems boundary

**Owned by this repository:**

* collective identity, generation and attempt authority;
* phase and step semantics *as they matter to the network*;
* participant sets, participant-set validation and stable canonical ordering;
* flow-group construction from planned communication edges;
* collective-aware traffic classes, priority and isolation policy;
* synchronization-sensitivity metadata;
* phase-aware admission and pacing intents;
* congestion and capacity evidence ingestion;
* traffic policy installation and versioning;
* stale participant, topology, policy, attempt and epoch fencing;
* collective cancellation, retirement and stale-completion refusal;
* deterministic explanations of which evidence and rules produced a decision;
* bounded decision history and inspection queries;
* persistence of durable collective definitions and policy - and never of dynamic liveness or
  current congestion evidence.

The wire protocol exposes exactly one mutating path per kind of state:
`kRegisterCollective`, `kBeginAttempt`, `kPlanFlowGroup`, `kCancelCollective`,
`kRetireCollective`, `kIngestEvidence` (capacity, congestion, topology, policy) and the
explicit `kInstallPolicy`. Every request is answered exactly once — including
`kHeartbeatAck`, which reports the coordinator's own view of the participant and the expiry
instant it recorded, so a peer can never mistake "I sent a message" for "I hold authority".

**Deliberately NOT owned, and consumed only through narrow interfaces:**

* NCCL, RCCL, oneCCL or any other collective library implementation - this runtime classifies and
  governs their traffic, it does not implement a collective;
* GPU kernel execution and stream semantics;
* general route computation;
* generic flow scheduling for non-collective traffic;
* topology discovery;
* congestion-control algorithms owned by adjacent runtimes (ECN, DCQCN, PFC and so on);
* workload and job scheduling.

The runtime consumes those as *inputs*: a communication planner supplies phases, steps and
planned edges; a fabric observer supplies topology, capacity and congestion observations; a
liveness source supplies participant incarnations. None of them is trusted to grant authority.

## Core model

### Strongly typed identities

All identities are opaque 128-bit values (`ctf::Identity` and its aliases `CollectiveId`,
`CollectiveGeneration`, `CollectiveAttemptId`, `ParticipantId`, `FlowGroupId`,
`TopologyGeneration`, `PolicyGeneration`, `CoordinatorEpoch`, `BootIncarnation`,
`EvidenceGeneration`, `SessionId`, `NodeId`, `RackId`, `SwitchId`, `LinkId`, `FabricInstanceId`).
The zero value is the *unset* identity and is never a match. Identities are canonicalised as 32
lowercase hex characters and are only parsed from exactly that spelling.

A **generation** is a minted identity, not a counter. "Older" therefore means *already accepted
on this axis*: the coordinator keeps a bounded record of the generations it has accepted, so a
superseded observation can never be reinstalled. A **boot incarnation** is the exception that is
genuinely ordered: a participant that boots again must publish a strictly greater incarnation,
and that ordering is what fences an older boot deterministically (documented in
`include/ctf/participant.hpp`).

### Collective classes

`all_reduce`, `all_gather`, `reduce_scatter`, `broadcast`, `gather`, `scatter`, `all_to_all`,
`point_to_point`, `barrier_only`, `send_recv_grouped`, `reduce`, `scan`, `send_recv_split`, plus
`unknown` and `unknown_vendor`. Algorithm hints (ring, tree, hierarchical, butterfly, ...) are
**evidence or requested structure, never proof of a physical path** - the physical path is
whatever the planned edges say, and the plan is validated against the participant set.

### The decision record binds everything

A `ctf::DecisionRecord` that grants authority always binds:

| Bound value | Meaning |
| --- | --- |
| `collective_generation` | the definition generation in force |
| `instance.attempt` | the exact attempt, so a phase transition cannot reuse authority |
| `participants` | the canonical participant set the decision is legal for |
| `flow_group` | the canonical flow-group identity derived from the planned edges |
| `topology_generation` | the topology the decision saw |
| `policy_generation` | the policy that produced the traffic class, priority and ceilings |
| `capacity_generation` | the capacity observation used, or unset when none was needed |
| `congestion_generation` | the congestion observation used, or unset when none was needed |
| `coordinator_epoch` | the epoch of the coordinator that issued it |

`DecisionRecord::fingerprint()` is a deterministic digest over all of those fields, so two
processes that evaluate identical inputs produce identical fingerprints.

### Phases and steps

Plans carry phase and step indices on every edge. A plan that tries to reuse flow-group authority
across a phase transition is refused, because the attempt identity is part of the binding: a new
phase means a new attempt, and a decision bound to the previous attempt is stale.

### Outcomes

```
ADMITTED           traffic treatment is permitted for this generation set
DEFERRED           the decision is legal but admission control released nothing yet
RATE_LIMITED       permission stands; the release is paced and has a retry instant
ISOLATED_CLASS     admitted, but only into an isolated class that is never multiplexed
REQUIRES_REPLAN    evidence or structure is missing; the runtime refuses to guess
REJECTED_STALE     an epoch, generation, attempt or participant liveness is not current
REJECTED_CAPACITY  the observed capacity cannot honor the class floor
REJECTED_POLICY    the policy forbids this treatment, or forbids the requested class
UNKNOWN_SEMANTICS  the collective class is not understood, so no class is granted
```

Rejections additionally name an `AuthorityAxis` (`coordinator_epoch`, `topology_generation`,
`policy_generation`, `collective_generation`, `attempt_generation`, `participant_liveness`,
`capacity_evidence`, `congestion_evidence`, `lifecycle`, `semantics`, `plan`) and a stable
`ErrorCode`, and carry an ordered `ReasonChain` of machine-readable codes with human-readable
detail.

**Authority is granted only by `ADMITTED` and `ISOLATED_CLASS`.** `DEFERRED` and `RATE_LIMITED`
are answers about *timing*, not about permission: the permission stands, the release waits.

## Evaluation order

The engine applies a fixed, documented order. It is never data dependent, so the reason chain is
always reproducible:

1. collective generation matches the current definition;
2. a planned flow group exists and is structurally valid;
3. the requester's observed epoch, policy generation, topology generation, capacity generation
   and congestion generation are current (each is reported on its own axis);
4. a policy is installed and a topology is installed;
5. every participant holds current liveness evidence with a current incarnation;
6. every live participant maps to a node in the current topology;
7. the collective class has a defined treatment (unknown semantics are refused here, and a member
   override is honoured or refused by policy - never silently strengthened);
8. the traffic class requires fresh congestion evidence, and it exists and is fresh;
9. the class floor can be honored under the observed capacity and congestion response;
10. admission control releases the volume, or paces it.

## Deterministic treatment table (default policy)

| Collective class | Traffic class | Priority | Isolation | Fresh evidence |
| --- | --- | --- | --- | --- |
| all_reduce, all_gather, reduce_scatter, broadcast, gather, scatter, all_to_all, reduce, scan | bulk_data | 8 | shared | required |
| point_to_point, send_recv_grouped, send_recv_split | best_effort_bulk | 5 | shared | required |
| barrier_only | latency_critical | 15 | class isolated | required |
| unknown, unknown_vendor | unclassified | 0 | shared | not applicable |

`TrafficClass::kUnclassified` has the lowest strength in a total order and can never win
contention. Nothing can be demoted into it and nothing can be promoted out of it.

## Authority rules

The invariants this runtime exists to enforce:

* **observation is not authority** - congestion, capacity and topology are observations and never
  grant permission on their own;
* **eligibility is not authority** - a registered collective is not an admitted one;
* **planning is not authority** - a plan without a decision has no effect;
* **persistence is not currentness** - a durable definition is replayed, but it does not bring
  liveness, capacity or congestion back with it;
* **a reachable peer is not a current peer** - liveness expires, and publishing a liveness record
  is the only thing that creates peer authority;
* **a matching identifier is not a current generation** - a decision bound to a superseded
  generation is stale even when every identity in it still exists;
* **UNKNOWN stays distinct from SUPPORTED and UNSUPPORTED** - an unclassified collective never
  gains a stronger class by default, and never by request either.

**Stale, cancelled, retired, fenced or replayed authority is always refused deterministically.** A
coordinator restart advances the epoch, preserves durable definitions and policy, drops capacity
and congestion, and clears liveness: a decision issued before the restart can never be replayed,
and a plan after the restart returns `REQUIRES_REPLAN` until fresh evidence arrives.

## Persistence semantics

`ctf::SnapshotStore` writes a versioned, integrity-checked snapshot: a 60-byte envelope (magic,
format version, payload length, CRC-32 of the payload, write timestamp, 32-byte payload digest)
followed by a versioned payload.

**Persisted:** durable collective definitions with their generations and lifecycle state, the
traffic policy, the topology description, and the last accepted coordinator epoch.

**Never persisted:** peer liveness, boot incarnations, capacity observations, congestion
observations, sessions, in-flight decisions and decision history.

The snapshot type itself (`ctf::DurableAuthority`) has no field for liveness, capacity or
congestion, so the type system - not a coding convention - is what prevents a restart from
resurrecting them.

Durable mutation follows an explicit order:

```
plan -> validate authority -> prepare -> perform effect -> verify -> durable commit -> publish result
```

An acknowledgement never precedes its durability point. Writing is atomic (temporary sibling,
flush, `_commit`/`fsync`, then rename over the target). Malformed, corrupt, truncated, oversized,
version-incompatible or impossible state is rejected **without partial application**, and
`CollectiveCatalog::load` validates an entire batch before it replaces anything.

## Process, epoch and generation behaviour

* `CoordinatorService::open_session` mints the session identity and boot incarnation; a peer never
  supplies an identity that the coordinator then believes.
* Every frame after the handshake carries the session, the boot incarnation, the epoch and a
  strictly increasing frame sequence. A frame that does not advance the sequence is refused as a
  replay.
* Frames are integrity checked: a 44-byte header (magic, sizes, kind, flags, protocol version,
  payload length, payload CRC-32, header CRC-32, reserved, session identity) plus a 40-byte
  envelope (incarnation, epoch, frame sequence). Payloads are decoded with a strict canonical
  codec: bounded counts, bounded strings, no trailing bytes, no non-minimal encodings.
* A participant identity is bound to the session that published it, so a peer cannot heartbeat on
  behalf of another session.
* `publish_participant` and `heartbeat` fence a lower boot incarnation permanently.
* Closing a session **fences** every participant that session published, and liveness that
  lapses is fenced rather than merely expired: the keeper of a dead boot can never republish the
  incarnation that boot used, while a strictly greater incarnation — what a real reboot produces —
  is still accepted.
* A frame whose envelope names a session or boot incarnation other than the one the connection
  owns is refused with `kEnvelopeSessionMismatch` and the connection is closed, so a peer cannot
  act under another session's authority by editing the envelope.
* **Teardown is ordered so that it cannot deadlock.** The coordinator stops the service first —
  which marks it not running, drops every session, and performs the durable flush with no lock
  held — and only then stops the transport, which releases every blocked read and joins every
  worker thread. Stopping the transport first would let a session thread call back into the
  service while the service held the lock across the waiting part of teardown.
* A receive never blocks indefinitely: it waits in bounded slices, and shutdown both cancels
  pending I/O on the handle and shuts the socket down. Teardown therefore does not depend on the
  platform reliably interrupting a blocking read.

## Supported, SYNTHETIC and UNSUPPORTED proof surfaces

| Dimension | Classification | Note |
| --- | --- | --- |
| Multiprocess authority, fencing, epoch advance | **REAL** | independent OS processes over loopback TCP |
| Framed transport, replay and protocol attacks | **REAL** | real sockets, real framing, 127.0.0.1 |
| Persistence, atomic replacement, torn tails, restart | **REAL** | real files |
| Multi-rack topology, capacity and congestion evidence | **SYNTHETIC** | generated by tests/support/synthetic.hpp |
| Accelerator participation metadata | **SYNTHETIC** | endpoint class and device index are metadata only |
| RDMA, RoCE, InfiniBand, NVLink, SmartNIC/DPU, programmable switches | **UNSUPPORTED** | no such hardware is present, and no code path claims it |
| AddressSanitizer | **UNSUPPORTED on this toolchain** | see *Build, test, install* |

No case in this repository claims physical fabric validation.

## Public API sketch

```cpp
#include "ctf/coordinator.hpp"   // networked coordinator over real TCP
#include "ctf/service.hpp"       // the coordinator core, usable in process
#include "ctf/engine.hpp"        // the pure decision function
#include "ctf/decision.hpp"      // DecisionRequest, DecisionRecord, Outcome
#include "ctf/flow_group.hpp"    // FlowGroupPlan, construct_flow_group
#include "ctf/traffic.hpp"       // TrafficClass, TrafficPolicy, pacing
#include "ctf/persistence.hpp"   // SnapshotStore
#include "ctf/protocol.hpp"      // framing and payload codecs

ctf::CoordinatorService service;                       // or CoordinatorServer for TCP
service.start(ctf::transport::monotonic_now_ms());
ctf::SessionBinding session;
service.open_session("planner", ctf::transport::monotonic_now_ms(), session);

ctf::ServiceRequestContext context;                    // session, incarnation, epoch, clock
ctf::RegistrationOutcome registration;
service.register_collective(context, definition, registration);

ctf::AttemptOutcome attempt;
service.begin_attempt(context, definition.id, registration.generation, mint_identity(), 1u << 20, attempt);

ctf::DecisionRequest request;                          // instance, observed generations, plan
ctf::DecisionRecord decision;
service.plan_flow_group(context, request, decision);

if (ctf::outcome_grants_authority(decision.outcome)) {
  // decision.flow_group, decision.pacing, decision.sync, and every bound generation
}
std::string why = decision.explain();                  // deterministic multi-line explanation
```

## Build, test, install

Requires CMake 3.20 or newer, a C++20 compiler, and a threads implementation. There are no
third-party dependencies.

```console
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure   # or run build/tests/bin/Release/<suite>.exe
cmake --install build --config Release --prefix /some/clean/prefix
```

On Windows with the Visual Studio generator the commands are the same; the configuration is
selected with `--config`. One MSBuild quirk is worth knowing: on a freshly generated build tree a
parallel build can reach a test link before the library archive exists. Run the build a second
time (it is a no-op) or build the library target first; `validate/run-closure-validation.ps1`
does this for you.

Options: `CTF_BUILD_TESTS`, `CTF_BUILD_TOOLS`, `CTF_BUILD_EXAMPLES`, `CTF_WARNINGS_AS_ERRORS`
(default ON), `CTF_ENABLE_SANITIZERS` (default OFF). First-party code builds with zero warnings
under MSVC `/W4 /WX` and under the GCC/Clang warning set in `cmake/CTFWarnings.cmake`.

### AddressSanitizer

The reference build for this release is MSVC 19.44 on Windows x64. `/fsanitize=address` is
accepted by that compiler, but this release does **not** claim sanitizer coverage: the ASan
configuration is provided (`-DCTF_ENABLE_SANITIZERS=ON`) and is **UNSUPPORTED** as a verified
proof surface on this toolchain. No case in this repository claims otherwise.

### Downstream consumption

```cmake
find_package(CollectiveTrafficFabric 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE ctf::collective_traffic_fabric)
```

`validate/downstream` is an independent consumer that does exactly this against an installed
prefix, and `validate/run-closure-validation.ps1` builds and runs it.

### Tools and examples

```console
ctfctl serve --port 7420 --snapshot state.ctfs     # a real coordinator
ctfctl selftest                                    # in-process end-to-end exercise
ctfctl install-policy --port 7420 --standard       # without a policy nothing can be admitted
ctfctl inspect --port 7420 --subject summary
ctfctl register --port 7420 --class all_reduce --bytes 1073741824
ctfctl plan --port 7420 --collective <hex> --generation <hex> --attempt <hex> --pattern ring
ctfctl explain --port 7420 --collective <hex> --attempt <hex>
```

`ctf_example_decision_types` runs the same path in process and prints one admission and two
deterministic refusals with their full reason chains.

## Test suites

| Suite | Proof surface |
| --- | --- |
| `ctf_unit_core` | identities, canonicalisation, flow groups, classification, policy, token bucket, decision semantics, catalog lifecycle, bounded history |
| `ctf_unit_persistence` | snapshot round trip, CRC-32, truncation, single-byte corruption, impossible state, atomic replacement |
| `ctf_property_engine` | seeded randomized decisions with per-step invariants and reproducibility |
| `ctf_adversarial_decoder` | frame and payload mutation, truncation, absurd counts, contradictory evidence |
| `ctf_adversarial_persistence` | random garbage, every-byte corruption, all-or-nothing catalog load |
| `ctf_concurrency_service` | barriers, concurrent registration, concurrent decisions, evidence races, session lifecycle, repeated start/stop |
| `ctf_concurrency_authority` | generation history, expiry, fencing, restart, and the full authority ladder |
| `ctf_protocol_framing` | header layout, multi-frame buffers, envelope integrity, every payload codec |
| `ctf_integration_loopback` | real loopback TCP end to end, session binding, limits, shutdown |
| `ctf_integration_lifecycle` | cancellation, retirement and stale completions over the wire |
| `ctf_persistence_restart` | restart, epoch advance, no resurrection of dynamic evidence, corrupt snapshot handling |
| `ctf_multiprocess_fabric` | independent OS processes: multi-publisher registration, process kill fencing, coordinator restart |
| `ctf_scale_fabric` | thousands of collectives, thousands of edges, bounded history, linear-cost evidence |

Randomized suites print their seed and honour `CTF_TEST_SEED` for exact reproduction. No suite
sets a timeout: a hanging test is a defect to diagnose, not something to skip.

## Limitations actually observed

* **Loopback only.** All transport proofs use `127.0.0.1`. No real RDMA, RoCE, InfiniBand,
  NVLink, SmartNIC/DPU or programmable-switch behaviour is exercised, and none is claimed.
* **Synthetic fabric evidence.** Topology, capacity and congestion in the tests are generated. The
  runtime has no topology discovery of its own by design; a deployment must feed it observations.
* **Rates are logical bytes per second.** The runtime reasons about logical volumes and observed
  capacities. It does not measure wire throughput and it does not implement congestion control: it
  computes a pacing intent, and enforcing that intent belongs to whatever schedules the traffic.
* **Incarnation ordering is a contract, not a check.** A participant that publishes a *lower*
  incarnation is fenced; a participant that publishes a merely different one that is not greater
  is refused as a session mismatch. A deployment that mints fresh random incarnations on every
  boot would be refused, so a monotonic component belongs in the incarnation.
* **The congestion response is a fixed integer table** (five utilization buckets with rate
  numerators 1000/800/600/400/150 per mille), not an adaptive controller.
* **Capacity is treated as an aggregate supply.** The engine clamps the admitted rate by the sum of
  observed available capacity rather than computing a per-path allocation, because it does not own
  route computation. A deployment with strongly heterogeneous paths should supply capacity scoped
  to the links it cares about.
* **No sanitizer coverage** in this release, as described above.
* **Windows x64 with MSVC is the reference platform for this release.** The code is portable
  POSIX/Winsock and the POSIX branches are written, but only this toolchain has been built,
  tested, installed and consumed for the tagged release.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
