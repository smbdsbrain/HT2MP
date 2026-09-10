#include "online_world_policy.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

int failures{};

void check(const bool condition, const char* expression, const int line) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
  }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void clamps_every_stock_type_and_restores_exactly() {
  const std::array<std::int32_t, ht2mp::bridge::kStockActorTypeCount> current{
      4, 1, 6, 7, 8, 9, 10, 11, 12, 13};
  std::array<std::int32_t, ht2mp::bridge::kStockActorTypeCount> target{
      40, 41, 42, 43, 44, 45, 46, 47, 48, 49};
  const auto saved = target;
  ht2mp::bridge::ClampStockActorTargets(current, target);
  for (std::size_t type = 0U; type < target.size(); ++type) {
    CHECK(target[type] == (type == 1U ? saved[type] : current[type]));
  }
  target = saved;
  CHECK(target == saved);
}

void distinguishes_host_stalls_from_a_paused_game_thread() {
  CHECK(!ht2mp::bridge::ShouldRearmBackgroundTickWatchdog(0U));
  CHECK(!ht2mp::bridge::ShouldRearmBackgroundTickWatchdog(
      ht2mp::bridge::kBackgroundTickWatchdogLimitMs));
  CHECK(ht2mp::bridge::ShouldRearmBackgroundTickWatchdog(
      ht2mp::bridge::kBackgroundTickWatchdogLimitMs + 1U));
  CHECK(ht2mp::bridge::ShouldRearmBackgroundTickWatchdog(60'000U));
}

void suppresses_only_online_focus_loss() {
  CHECK(!ht2mp::bridge::ShouldSuppressFocusLoss(false, false));
  CHECK(!ht2mp::bridge::ShouldSuppressFocusLoss(false, true));
  CHECK(!ht2mp::bridge::ShouldSuppressFocusLoss(true, true));
  CHECK(ht2mp::bridge::ShouldSuppressFocusLoss(true, false));
}

void classifies_only_exact_local_and_owned_ids_as_preserved() {
  constexpr std::uint32_t local = 0x10100U;
  constexpr std::array<std::uint32_t, 2> remote{0x20200U, 0x30300U};
  CHECK(ht2mp::bridge::ClassifyOnlineActor(local, local, remote) ==
        ht2mp::bridge::OnlineActorOwnership::local);
  CHECK(ht2mp::bridge::ClassifyOnlineActor(remote[1], local, remote) ==
        ht2mp::bridge::OnlineActorOwnership::remote_owned);
  CHECK(ht2mp::bridge::ClassifyOnlineActor(0x40400U, local, remote) ==
        ht2mp::bridge::OnlineActorOwnership::stock);
}

void rejects_corrupt_lists_and_rearms_each_world_generation() {
  constexpr std::uint32_t sentinel = 0x1000U;
  constexpr std::uint32_t local = 0x2000U;
  constexpr std::uint32_t remote = 0x3000U;
  constexpr std::uint32_t stock = 0x4000U;
  constexpr std::array<std::uint32_t, 1> remotes{remote};
  std::array nodes{
      ht2mp::bridge::OnlineActorNodeView{local, remote, sentinel, local, 1},
      ht2mp::bridge::OnlineActorNodeView{remote, stock, local, remote, 2},
      ht2mp::bridge::OnlineActorNodeView{stock, sentinel, remote, stock, 8},
  };
  ht2mp::bridge::OnlineActorListSummary summary;
  CHECK(ht2mp::bridge::ValidateOnlineActorList(
      nodes, sentinel, local, stock, local, remotes, 8U, summary));
  CHECK(summary.local == 1U && summary.remote_owned == 1U &&
        summary.stock == 1U);

  nodes[1].previous = sentinel;
  CHECK(!ht2mp::bridge::ValidateOnlineActorList(
      nodes, sentinel, local, stock, local, remotes, 8U, summary));
  nodes[1].previous = local;
  nodes[2].next = remote;
  CHECK(!ht2mp::bridge::ValidateOnlineActorList(
      nodes, sentinel, local, stock, local, remotes, 3U, summary));

  ht2mp::bridge::OnlineWorldLifecycle lifecycle;
  CHECK(!lifecycle.BeginSanitization());
  lifecycle.ObserveWorldReady(true);
  CHECK(lifecycle.generation == 1U && lifecycle.BeginSanitization() &&
        !lifecycle.BeginSanitization());
  lifecycle.ObserveWorldReady(false);
  lifecycle.ObserveWorldReady(true);
  CHECK(lifecycle.generation == 2U && lifecycle.BeginSanitization());
  lifecycle.NotifyRegistryReset();
  lifecycle.ObserveWorldReady(true);
  CHECK(lifecycle.generation == 3U && lifecycle.BeginSanitization());
}

void rearms_on_loading_and_every_registry_generation() {
  ht2mp::bridge::OnlineWorldLifecycle lifecycle;
  CHECK(!lifecycle.BeginSanitization());
  lifecycle.ObserveWorldReady(true);
  CHECK(lifecycle.generation == 1U);
  CHECK(lifecycle.BeginSanitization());
  CHECK(!lifecycle.BeginSanitization());
  lifecycle.ObserveWorldReady(false);
  lifecycle.ObserveWorldReady(true);
  CHECK(lifecycle.generation == 2U);
  CHECK(lifecycle.BeginSanitization());
  lifecycle.NotifyRegistryReset();
  lifecycle.ObserveWorldReady(true);
  CHECK(lifecycle.generation == 3U);
  CHECK(lifecycle.BeginSanitization());
}

} // namespace

int main() {
  clamps_every_stock_type_and_restores_exactly();
  distinguishes_host_stalls_from_a_paused_game_thread();
  suppresses_only_online_focus_loss();
  classifies_only_exact_local_and_owned_ids_as_preserved();
  rejects_corrupt_lists_and_rearms_each_world_generation();
  rearms_on_loading_and_every_registry_generation();
  if (failures != 0) {
    std::cerr << failures << " online-world policy test(s) failed\n";
    return 1;
  }
  std::cout << "online-world policy tests passed\n";
  return 0;
}
