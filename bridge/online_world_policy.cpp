#include "online_world_policy.hpp"

#include <algorithm>

namespace ht2mp::bridge {

bool ShouldRearmBackgroundTickWatchdog(
    const std::uint64_t worker_gap_ms) noexcept {
  return worker_gap_ms > kBackgroundTickWatchdogLimitMs;
}

bool ShouldSuppressFocusLoss(const bool online_active,
                             const bool application_active) noexcept {
  return online_active && !application_active;
}

OnlineActorOwnership ClassifyOnlineActor(
    const std::uint32_t player_id,
    const std::uint32_t local_player_id,
    const std::span<const std::uint32_t> remote_player_ids) noexcept {
  if (player_id == local_player_id) return OnlineActorOwnership::local;
  if (std::find(remote_player_ids.begin(), remote_player_ids.end(), player_id) !=
      remote_player_ids.end()) {
    return OnlineActorOwnership::remote_owned;
  }
  return OnlineActorOwnership::stock;
}

void ClampStockActorTargets(
    const std::array<std::int32_t, kStockActorTypeCount>& current,
    std::array<std::int32_t, kStockActorTypeCount>& target) noexcept {
  for (std::size_t type = 0U; type < target.size(); ++type) {
    if (type != 1U) target[type] = current[type];
  }
}

void OnlineWorldLifecycle::ObserveWorldReady(const bool ready) noexcept {
  if (!ready) {
    world_ready = false;
    sanitization_armed = true;
    return;
  }
  if (!world_ready) {
    world_ready = true;
    sanitization_armed = true;
    ++generation;
  }
}

void OnlineWorldLifecycle::NotifyRegistryReset() noexcept {
  world_ready = false;
  sanitization_armed = true;
}

bool OnlineWorldLifecycle::BeginSanitization() noexcept {
  if (!world_ready || !sanitization_armed) return false;
  sanitization_armed = false;
  return true;
}

bool ValidateOnlineActorList(
    const std::span<const OnlineActorNodeView> nodes,
    const std::uint32_t sentinel,
    const std::uint32_t first,
    const std::uint32_t last,
    const std::uint32_t local_player_id,
    const std::span<const std::uint32_t> remote_player_ids,
    const std::uint32_t maximum_nodes,
    OnlineActorListSummary& summary) noexcept {
  summary = {};
  if (sentinel == 0U || first == 0U || last == 0U ||
      local_player_id == 0U || local_player_id == sentinel ||
      maximum_nodes == 0U) {
    return false;
  }
  std::uint32_t current = first;
  std::uint32_t previous = sentinel;
  std::uint32_t visited{};
  while (current != sentinel) {
    if (++visited > maximum_nodes) return false;
    const auto found = std::find_if(
        nodes.begin(), nodes.end(), [current](const auto& value) {
          return value.player_id == current;
        });
    if (found == nodes.end() || found->previous != previous ||
        found->next == 0U || found->self != current || found->actor_type < 0 ||
        found->actor_type >=
            static_cast<std::int32_t>(kStockActorTypeCount)) {
      return false;
    }
    const auto ownership = ClassifyOnlineActor(
        current, local_player_id, remote_player_ids);
    if (ownership == OnlineActorOwnership::local) {
      if (found->actor_type != 1) return false;
      ++summary.local;
    } else if (ownership == OnlineActorOwnership::remote_owned) {
      if (found->actor_type != 2) return false;
      ++summary.remote_owned;
    } else {
      ++summary.stock;
    }
    previous = current;
    current = found->next;
  }
  return previous == last && summary.local == 1U;
}

} // namespace ht2mp::bridge
