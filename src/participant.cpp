// Collective Traffic Fabric - participant model implementation.
// Copyright 2026 Summon Software Labs.
#include "ctf/participant.hpp"

namespace ctf {

std::string_view to_string(ParticipantRole value) noexcept {
  switch (value) {
    case ParticipantRole::kUnspecified: return "unspecified";
    case ParticipantRole::kRank: return "rank";
    case ParticipantRole::kRoot: return "root";
    case ParticipantRole::kCoordinator: return "coordinator";
    case ParticipantRole::kProxy: return "proxy";
    case ParticipantRole::kObserver: return "observer";
    case ParticipantRole::kAccelerator: return "accelerator";
  }
  return "unspecified";
}

bool participant_role_from_string(std::string_view text, ParticipantRole& out) noexcept {
  struct Entry { std::string_view name; ParticipantRole value; };
  static constexpr Entry kTable[] = {
      {"unspecified", ParticipantRole::kUnspecified},
      {"rank", ParticipantRole::kRank},
      {"root", ParticipantRole::kRoot},
      {"coordinator", ParticipantRole::kCoordinator},
      {"proxy", ParticipantRole::kProxy},
      {"observer", ParticipantRole::kObserver},
      {"accelerator", ParticipantRole::kAccelerator},
  };
  for (const Entry& entry : kTable) {
    if (entry.name == text) {
      out = entry.value;
      return true;
    }
  }
  return false;
}

std::string_view to_string(ParticipantState value) noexcept {
  switch (value) {
    case ParticipantState::kUnspecified: return "unspecified";
    case ParticipantState::kAdvertised: return "advertised";
    case ParticipantState::kLive: return "live";
    case ParticipantState::kSuspect: return "suspect";
    case ParticipantState::kExpired: return "expired";
    case ParticipantState::kFenced: return "fenced";
    case ParticipantState::kRetired: return "retired";
  }
  return "unspecified";
}

bool participant_state_holds_authority(ParticipantState value) noexcept {
  return value == ParticipantState::kLive;
}

std::string_view to_string(EndpointClass value) noexcept {
  switch (value) {
    case EndpointClass::kUnspecified: return "unspecified";
    case EndpointClass::kCpu: return "cpu";
    case EndpointClass::kGpu: return "gpu";
    case EndpointClass::kAcceleratorOther: return "accelerator_other";
    case EndpointClass::kSmartNic: return "smart_nic";
    case EndpointClass::kProxy: return "proxy";
  }
  return "unspecified";
}

}  // namespace ctf
