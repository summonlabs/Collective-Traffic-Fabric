// Collective Traffic Fabric - participant descriptors, incarnation and
// liveness.  A reachable peer is NOT a current peer: liveness is separate
// authority and it expires.
// Copyright 2026 Summon Software Labs.
#ifndef CTF_PARTICIPANT_HPP
#define CTF_PARTICIPANT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ctf/identity.hpp"

namespace ctf {

enum class ParticipantRole : std::uint8_t {
  kUnspecified = 0,
  kRank = 1,          // ordinary member rank
  kRoot = 2,          // broadcast/gather/scatter root
  kCoordinator = 3,   // collective level coordinator (e.g. NCCL-style bootstrap)
  kProxy = 4,         // in-network or host proxy executing on behalf of a rank
  kObserver = 5,      // receives evidence, holds no traffic authority
  kAccelerator = 6,   // the accelerated endpoint itself
};

[[nodiscard]] std::string_view to_string(ParticipantRole value) noexcept;
[[nodiscard]] bool participant_role_from_string(std::string_view text, ParticipantRole& out) noexcept;

enum class ParticipantState : std::uint8_t {
  kUnspecified = 0,
  kAdvertised = 1,   // durable definition knows about it
  kLive = 2,         // current liveness evidence exists
  kSuspect = 3,      // liveness evidence is aging; authority is withheld
  kExpired = 4,      // liveness evidence is gone; authority is withheld
  kFenced = 5,       // an incarnation change fenced this identity
  kRetired = 6,      // cleanly removed
};

[[nodiscard]] std::string_view to_string(ParticipantState value) noexcept;
[[nodiscard]] bool participant_state_holds_authority(ParticipantState value) noexcept;

// Hardware class of the endpoint.  This is descriptive evidence used for
// synchronisation sensitivity and for honest proof labelling; it never grants
// traffic authority by itself.
enum class EndpointClass : std::uint8_t {
  kUnspecified = 0,
  kCpu = 1,
  kGpu = 2,
  kAcceleratorOther = 3,
  kSmartNic = 4,
  kProxy = 5,
};

[[nodiscard]] std::string_view to_string(EndpointClass value) noexcept;

struct ParticipantDescriptor {
  ParticipantId id{};
  ParticipantRole role = ParticipantRole::kUnspecified;
  NodeId node{};
  RackId rack{};
  EndpointClass endpoint = EndpointClass::kUnspecified;
  std::uint32_t device_index = 0;
  std::string label;
};

// Liveness record.  Absence of this record means absence of authority, never
// default authority.
//
// Boot incarnations are ORDERED: a participant that boots again must mint a
// strictly greater incarnation than every incarnation it has ever published.
// The runtime relies on that ordering to fence an older boot deterministically
// instead of trusting whichever message happened to arrive last.  An incarnation
// that is merely different, and not greater, is refused as a session mismatch
// rather than accepted, because "different" is not evidence of "newer".
struct ParticipantLiveness {
  ParticipantId id{};
  BootIncarnation incarnation{};
  ParticipantState state = ParticipantState::kUnspecified;
  std::uint64_t last_heartbeat_monotonic_ms = 0;
  std::uint64_t expires_at_monotonic_ms = 0;
  NodeId node{};
  SessionId bound_session{};

  [[nodiscard]] bool is_current(std::uint64_t now_monotonic_ms) const noexcept {
    return state == ParticipantState::kLive && now_monotonic_ms < expires_at_monotonic_ms;
  }
};

}  // namespace ctf

#endif  // CTF_PARTICIPANT_HPP
