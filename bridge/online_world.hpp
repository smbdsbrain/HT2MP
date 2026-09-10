#pragma once

#include "ht2mp/game/profile.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ht2mp::bridge {

struct OnlineWorldStats final {
  bool ready{};
  bool safe_mode_requested{};
  bool world_ready{};
  bool sanitized{};
  bool background_tick_healthy{};
  std::uint32_t generation{};
  std::uint32_t local_actors{};
  std::uint32_t remote_owned_actors{};
  std::uint32_t stock_actors{};
  std::uint32_t suppressed_regular{};
  std::uint32_t suppressed_ghosts{};
  std::uint32_t suppressed_dealers{};
  std::uint32_t blocked_assortments{};
  std::uint32_t blocked_orders{};
  std::uint32_t blocked_hires{};
  std::uint32_t focus_loss_events{};
  std::uint32_t focus_loss_suppressed{};
  std::array<std::uint32_t, 10> removed_by_type{};
  std::uint32_t validation_failures{};
  std::uint32_t thread_mismatches{};
  std::uint64_t last_tick_age_ms{};
};

[[nodiscard]] bool InitializeSteamOnlineWorld(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    std::string& error) noexcept;

// Called by the already verified post-AI hook. All registry mutations remain
// on that hook's stable owner thread.
void TickSteamOnlineWorld(bool local_world_ready) noexcept;
void NotifySteamOnlineWorldRegistryReset() noexcept;
void RequestSteamOnlineWorldSafeMode() noexcept;
void RearmSteamBackgroundTickWatchdog(std::uint64_t now_ms) noexcept;

[[nodiscard]] bool SteamOnlineWorldReady() noexcept;
[[nodiscard]] bool SteamBackgroundTickWatchdogExpired(
    std::uint64_t now_ms) noexcept;
[[nodiscard]] OnlineWorldStats GetOnlineWorldStats() noexcept;

} // namespace ht2mp::bridge
