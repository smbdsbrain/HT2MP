#pragma once

#include "ht2mp/game/profile.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace ht2mp::bridge {

enum class AutoEnterState : std::uint32_t {
  disabled = 0U,
  armed = 1U,
  dispatching = 2U,
  load_dispatched = 3U,
  world_ready = 4U,
  failed = 5U,
};

enum class AutoEnterFailure : std::uint32_t {
  none = 0U,
  thread_mismatch = 1U,
  main_object_mismatch = 2U,
  single_player_object_mismatch = 3U,
  panel_changed_during_dispatch = 4U,
};

struct AutoEnterStats final {
  AutoEnterState state{AutoEnterState::disabled};
  AutoEnterFailure failure{AutoEnterFailure::none};
  std::uint32_t hook_invocations{};
  std::uint32_t attempts{};
  std::uint32_t thread_id{};
  std::uint32_t thread_mismatches{};
};

// Installs the selected exact profile's menu hooks. Once armed, the bridge dispatches the
// game's own Single Player (0x66) and Load (0x6e) widget events on the menu's
// owning thread.  The hooks become permanent pass-throughs after one attempt.
[[nodiscard]] bool InitializeGogAutoEnter(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    std::string& error) noexcept;

// Called by the already validated post-AI observer.  A real local sample is
// the world-ready acknowledgement; no menu/global heuristic is treated as
// sufficient proof that loading finished.
void NotifyAutoEnterWorldReady() noexcept;
void DisableAutoEnterNoWait() noexcept;
[[nodiscard]] bool AutoEnterReady() noexcept;
[[nodiscard]] AutoEnterStats GetAutoEnterStats() noexcept;

} // namespace ht2mp::bridge
