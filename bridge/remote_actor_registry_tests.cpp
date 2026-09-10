#include "remote_actor_registry.hpp"
#include "hook_thread_guard.hpp"

#include <cstdint>
#include <iostream>

namespace {

int failures{};

void Check(const bool condition, const char* expression, const int line) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
  }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

ht2mp::bridge::RemoteActorBinding Binding(const std::uint64_t player,
                                          const std::uint64_t incarnation,
                                          const std::uint32_t index) {
  ht2mp::bridge::RemoteActorBinding value;
  value.remote = {player, incarnation};
  value.game_player_id = 0x00200000U + index * 0x10000U;
  value.vehicle_instance = 0x00300000U + index * 0x10000U;
  value.moving_item = value.vehicle_instance + 0x10U;
  value.physics = 0x00400000U + index * 0x10000U;
  return value;
}

void RegistryRejectsStaleAndAliasedIdentity() {
  ht2mp::bridge::RemoteActorRegistry registry;
  ht2mp::bridge::RemoteActorBinding first;
  CHECK(registry.Insert(Binding(1U, 10U, 1U), first) ==
        ht2mp::bridge::RemoteActorRegistryResult::inserted);
  CHECK(first.generation != 0U && registry.size() == 1U);
  CHECK(registry.Find({1U, 10U}) != nullptr);
  CHECK(registry.FindPlayer(1U) != nullptr);

  ht2mp::bridge::RemoteActorBinding ignored;
  CHECK(registry.Insert(Binding(1U, 10U, 2U), ignored) ==
        ht2mp::bridge::RemoteActorRegistryResult::remote_player_conflict);
  CHECK(registry.Insert(Binding(1U, 11U, 2U), ignored) ==
        ht2mp::bridge::RemoteActorRegistryResult::remote_player_conflict);

  auto aliased = Binding(2U, 20U, 2U);
  aliased.moving_item = first.moving_item;
  CHECK(registry.Insert(aliased, ignored) ==
        ht2mp::bridge::RemoteActorRegistryResult::game_identity_conflict);
  CHECK(registry.Erase({1U, 10U}, first.generation + 1U) ==
        ht2mp::bridge::RemoteActorRegistryResult::stale_generation);
  CHECK(registry.Erase({1U, 11U}, first.generation) ==
        ht2mp::bridge::RemoteActorRegistryResult::stale_incarnation);
  CHECK(registry.Erase({1U, 10U}, first.generation) ==
        ht2mp::bridge::RemoteActorRegistryResult::removed);
  CHECK(registry.empty());

  ht2mp::bridge::RemoteActorBinding replacement;
  CHECK(registry.Insert(Binding(1U, 11U, 1U), replacement) ==
        ht2mp::bridge::RemoteActorRegistryResult::inserted);
  CHECK(replacement.generation != first.generation);
}

void CapacityAndCompactionAreBounded() {
  ht2mp::bridge::RemoteActorRegistry registry;
  ht2mp::bridge::RemoteActorBinding inserted;
  for (std::uint32_t index = 0U;
       index < ht2mp::bridge::kMaximumRemotePlayers; ++index) {
    CHECK(registry.Insert(Binding(index + 1U, 100U + index, index + 1U),
                          inserted) ==
          ht2mp::bridge::RemoteActorRegistryResult::inserted);
  }
  CHECK(registry.size() == ht2mp::bridge::kMaximumRemotePlayers);
  CHECK(registry.Insert(Binding(100U, 200U, 100U), inserted) ==
        ht2mp::bridge::RemoteActorRegistryResult::full);

  const auto middle = *registry.Find({4U, 103U});
  CHECK(registry.Erase(middle.remote, middle.generation) ==
        ht2mp::bridge::RemoteActorRegistryResult::removed);
  CHECK(registry.size() == ht2mp::bridge::kMaximumRemotePlayers - 1U);
  CHECK(registry.Find({4U, 103U}) == nullptr);
  CHECK(registry.Find({7U, 106U}) != nullptr);
  registry.Clear();
  CHECK(registry.empty() && registry.bindings().empty());
  ht2mp::bridge::RemoteActorBinding after_clear;
  CHECK(registry.Insert(Binding(50U, 500U, 50U), after_clear) ==
        ht2mp::bridge::RemoteActorRegistryResult::inserted);
  CHECK(after_clear.generation > inserted.generation);
}

void CollisionIndexPublishesOnlyValidatedBindings() {
  ht2mp::bridge::RemoteActorRegistry registry;
  ht2mp::bridge::RemoteActorBinding one;
  ht2mp::bridge::RemoteActorBinding two;
  CHECK(registry.Insert(Binding(1U, 10U, 1U), one) ==
        ht2mp::bridge::RemoteActorRegistryResult::inserted);
  CHECK(registry.Insert(Binding(2U, 20U, 2U), two) ==
        ht2mp::bridge::RemoteActorRegistryResult::inserted);

  ht2mp::bridge::RemoteCollisionIndex index;
  CHECK(!index.Contains(one.moving_item));
  index.Publish(registry.bindings());
  CHECK(index.Contains(one.moving_item));
  CHECK(index.Contains(two.moving_item));
  CHECK(!index.Contains(0x00700000U));
  CHECK(index.PairTouchesRemote(0x00700000U, two.moving_item));
  CHECK(index.PairTouchesRemote(one.moving_item, 0x00700000U));
  CHECK(!index.PairTouchesRemote(0x00700000U, 0x00710000U));
  index.Clear();
  CHECK(!index.Contains(one.moving_item));
}

void RejectsMalformedBindings() {
  ht2mp::bridge::RemoteActorRegistry registry;
  ht2mp::bridge::RemoteActorBinding inserted;
  auto malformed = Binding(1U, 1U, 1U);
  malformed.remote.player_id = 0U;
  CHECK(registry.Insert(malformed, inserted) ==
        ht2mp::bridge::RemoteActorRegistryResult::invalid);
  malformed = Binding(1U, 1U, 1U);
  malformed.moving_item |= 1U;
  CHECK(registry.Insert(malformed, inserted) ==
        ht2mp::bridge::RemoteActorRegistryResult::invalid);
  malformed = Binding(1U, 1U, 1U);
  malformed.generation = 9U;
  CHECK(registry.Insert(malformed, inserted) ==
        ht2mp::bridge::RemoteActorRegistryResult::invalid);
}

void HookThreadIdentityFailsClosed() {
  ht2mp::bridge::HookThreadGuard guard;
  CHECK(guard.Observe(100U));
  CHECK(guard.Observe(100U));
  auto snapshot = guard.Snapshot();
  CHECK(snapshot.thread_id == 100U && snapshot.mismatches == 0U);
  CHECK(!guard.Observe(200U));
  snapshot = guard.Snapshot();
  CHECK(snapshot.thread_id == 100U && snapshot.mismatches == 1U);
  guard.Reset();
  CHECK(!guard.Observe(0U));
  snapshot = guard.Snapshot();
  CHECK(snapshot.thread_id == 0U && snapshot.mismatches == 1U);
}

}  // namespace

int main() {
  RegistryRejectsStaleAndAliasedIdentity();
  CapacityAndCompactionAreBounded();
  CollisionIndexPublishesOnlyValidatedBindings();
  RejectsMalformedBindings();
  HookThreadIdentityFailsClosed();
  if (failures != 0) {
    std::cerr << failures << " remote actor registry test(s) failed\n";
    return 1;
  }
  std::cout << "remote actor registry tests passed\n";
  return 0;
}
