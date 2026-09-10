#pragma once

#include "ht2mp/game/profile.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace ht2mp::bridge {

struct ActorBoundaryStats final {
  std::uint32_t invocations{};
  std::uint32_t thread_id{};
  std::uint32_t thread_mismatches{};
};

struct ActorDiagnosticStats final {
  bool ready{};
  ActorBoundaryStats moving_item_resolver{};
  ActorBoundaryStats pair_collision{};
  ActorBoundaryStats hit_player{};
  ActorBoundaryStats pre_registry_reset{};
  ActorBoundaryStats pre_save{};
};

// Installs exact-GOG, ABI-preserving pass-through detours. The detours only
// increment bridge-owned counters and tail-call the original trampoline; they
// never suppress a call, inspect arguments, or write game-owned data.
[[nodiscard]] bool InitializeGogActorDiagnostics(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    std::string& error) noexcept;

[[nodiscard]] bool ActorDiagnosticsReady() noexcept;
[[nodiscard]] ActorDiagnosticStats GetActorDiagnosticStats() noexcept;

} // namespace ht2mp::bridge
