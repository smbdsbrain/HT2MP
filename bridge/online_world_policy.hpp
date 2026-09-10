#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ht2mp::bridge {

inline constexpr std::size_t kStockActorTypeCount = 10U;
inline constexpr std::uint64_t kBackgroundTickWatchdogLimitMs = 500U;

// A long gap in the bridge worker means the host process (or the entire
// machine) was not scheduled either. Treat that as a new watchdog epoch;
// otherwise a suspend/resume would be indistinguishable from a paused game.
[[nodiscard]] bool ShouldRearmBackgroundTickWatchdog(
    std::uint64_t worker_gap_ms) noexcept;

// WM_ACTIVATEAPP can arrive while a newly-created game window is still hidden
// or changing visibility. The online decision must therefore depend only on
// the enabled online session and the message payload, not on a transient HWND
// query. Escape/close paths do not use this message handler.
[[nodiscard]] bool ShouldSuppressFocusLoss(bool online_active,
                                           bool application_active) noexcept;

enum class OnlineActorOwnership : std::uint8_t {
  local,
  remote_owned,
  stock,
};

[[nodiscard]] OnlineActorOwnership ClassifyOnlineActor(
    std::uint32_t player_id,
    std::uint32_t local_player_id,
    std::span<const std::uint32_t> remote_player_ids) noexcept;

// createPlayersRegular must retain the native type-1 target because that is
// the local player. All stock actor types are temporarily clamped to their
// already-live count and restored after the native function returns.
void ClampStockActorTargets(
    const std::array<std::int32_t, kStockActorTypeCount>& current,
    std::array<std::int32_t, kStockActorTypeCount>& target) noexcept;

struct OnlineWorldLifecycle final {
  bool world_ready{};
  bool sanitization_armed{true};
  std::uint32_t generation{};

  void ObserveWorldReady(bool ready) noexcept;
  void NotifyRegistryReset() noexcept;
  [[nodiscard]] bool BeginSanitization() noexcept;
};

struct OnlineActorNodeView final {
  std::uint32_t player_id{};
  std::uint32_t next{};
  std::uint32_t previous{};
  std::uint32_t self{};
  std::int32_t actor_type{};
};

struct OnlineActorListSummary final {
  std::uint32_t local{};
  std::uint32_t remote_owned{};
  std::uint32_t stock{};
};

// Process-independent mirror of the bounded intrusive-list invariants used by
// the live sanitizer. It exists so cycles, broken backlinks and invalid self
// identities remain regression-tested without proprietary game memory.
[[nodiscard]] bool ValidateOnlineActorList(
    std::span<const OnlineActorNodeView> nodes,
    std::uint32_t sentinel,
    std::uint32_t first,
    std::uint32_t last,
    std::uint32_t local_player_id,
    std::span<const std::uint32_t> remote_player_ids,
    std::uint32_t maximum_nodes,
    OnlineActorListSummary& summary) noexcept;

} // namespace ht2mp::bridge
