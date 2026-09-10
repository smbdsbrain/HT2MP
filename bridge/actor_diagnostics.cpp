#include "actor_diagnostics.hpp"

#include "hook_thread_guard.hpp"
#include "runtime_validation.hpp"

#include "ht2mp/game/pattern.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <string_view>

namespace ht2mp::bridge {
namespace {

using MovingItemResolver = void*(__cdecl*)(std::uint32_t);
using PairCollision = void(__cdecl*)(void*, void*, std::uint32_t, float);
using HitPlayer = void(__cdecl*)(std::uint32_t, double, int);
using PreRegistryReset = void(__cdecl*)();
using PreSave = void(__cdecl*)(void*);

struct BoundaryCounter final {
  volatile LONG invocations{};
  HookThreadGuard thread;
};

MovingItemResolver g_original_moving_item_resolver{};
PairCollision g_original_pair_collision{};
HitPlayer g_original_hit_player{};
PreRegistryReset g_original_pre_registry_reset{};
PreSave g_original_pre_save{};

BoundaryCounter g_resolver_counter;
BoundaryCounter g_collision_counter;
BoundaryCounter g_hit_counter;
BoundaryCounter g_reset_counter;
BoundaryCounter g_save_counter;
volatile LONG g_ready{};

void observe(BoundaryCounter& counter) noexcept {
  InterlockedIncrement(&counter.invocations);
  (void)counter.thread.Observe(GetCurrentThreadId());
}

void* __cdecl MovingItemResolverDetour(const std::uint32_t handle) {
  observe(g_resolver_counter);
  const auto original = g_original_moving_item_resolver;
  return original == nullptr ? nullptr : original(handle);
}

void __cdecl PairCollisionDetour(void* current, void* contact,
                                 const std::uint32_t flags,
                                 const float sample_time) {
  observe(g_collision_counter);
  const auto original = g_original_pair_collision;
  if (original != nullptr) original(current, contact, flags, sample_time);
}

void __cdecl HitPlayerDetour(const std::uint32_t player, const double impact,
                             const int source) {
  observe(g_hit_counter);
  const auto original = g_original_hit_player;
  if (original != nullptr) original(player, impact, source);
}

void __cdecl PreRegistryResetDetour() {
  observe(g_reset_counter);
  const auto original = g_original_pre_registry_reset;
  if (original != nullptr) original();
}

void __cdecl PreSaveDetour(void* archive_or_storage) {
  observe(g_save_counter);
  const auto original = g_original_pre_save;
  if (original != nullptr) original(archive_or_storage);
}

const ht2mp::game::SymbolDescriptor* find_descriptor(
    const ht2mp::game::GameProfile& profile,
    const std::string_view name) noexcept {
  const auto found = std::find_if(profile.symbols.begin(), profile.symbols.end(),
                                  [name](const auto& value) {
                                    return value.name == name;
                                  });
  return found == profile.symbols.end() ? nullptr : &*found;
}

bool executable_page(const std::uintptr_t address,
                     const std::size_t size) noexcept {
  MEMORY_BASIC_INFORMATION memory{};
  if (address == 0U || size == 0U ||
      VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                   sizeof(memory)) != sizeof(memory) ||
      memory.State != MEM_COMMIT ||
      (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
    return false;
  }
  const DWORD protection = memory.Protect & 0xFFU;
  const bool executable =
      protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
      protection == PAGE_EXECUTE_READWRITE ||
      protection == PAGE_EXECUTE_WRITECOPY;
  const auto begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
  return executable && address >= begin && address - begin <= memory.RegionSize &&
         size <= memory.RegionSize - (address - begin);
}

bool copy_live_bytes(const std::uintptr_t address, std::uint8_t* destination,
                     const std::size_t size) noexcept {
  __try {
    std::memcpy(destination, reinterpret_cast<const void*>(address), size);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool live_target_matches(const ht2mp::game::GameProfile& profile,
                         const std::string_view name,
                         const std::uintptr_t address,
                         std::string& error) {
  const auto* descriptor = find_descriptor(profile, name);
  if (descriptor == nullptr ||
      descriptor->kind != ht2mp::game::SymbolKind::diagnostic_function) {
    error = "actor diagnostic descriptor is unavailable: ";
    error += name;
    return false;
  }
  ht2mp::game::MaskedPattern pattern;
  if (!ht2mp::game::ParseMaskedPattern(descriptor->ida_pattern, pattern, error) ||
      pattern.empty() || pattern.size() > 64U) {
    if (error.empty()) error = "actor diagnostic pattern has an invalid size";
    return false;
  }
  if (!executable_page(address, pattern.size())) {
    error = "actor diagnostic target is not executable: ";
    error += name;
    return false;
  }
  std::array<std::uint8_t, 64> live{};
  if (!copy_live_bytes(address, live.data(), pattern.size())) {
    error = "structured exception while validating actor target: ";
    error += name;
    return false;
  }
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    if (pattern.mask[index] != 0U && live[index] != pattern.bytes[index]) {
      error = "live actor target differs from verified image: ";
      error += name;
      return false;
    }
  }
  return true;
}

void remove_created_hooks(const std::span<const LPVOID> targets) noexcept {
  for (const auto target : targets) {
    (void)MH_DisableHook(target);
    (void)MH_RemoveHook(target);
  }
}

void clear_originals() noexcept {
  g_original_moving_item_resolver = nullptr;
  g_original_pair_collision = nullptr;
  g_original_hit_player = nullptr;
  g_original_pre_registry_reset = nullptr;
  g_original_pre_save = nullptr;
}

bool initialize_impl(const ht2mp::game::ProfileVerification& verification,
                     const std::string_view requested_profile_id,
                     const std::uintptr_t module_base,
                     std::string& error) {
  if (InterlockedCompareExchange(&g_ready, 0, 0) != 0) {
    error = "actor diagnostics are already initialized";
    return false;
  }

  GogActorDiagnosticPlan plan;
  if (!BuildGogActorDiagnosticPlan(verification, requested_profile_id,
                                   module_base, plan, error)) {
    return false;
  }

  struct Target final {
    std::string_view name;
    LPVOID address;
    LPVOID detour;
  };
  const std::array targets{
      Target{"moving_item_handle_resolver_candidate",
             reinterpret_cast<LPVOID>(plan.moving_item_handle_resolver),
             reinterpret_cast<LPVOID>(&MovingItemResolverDetour)},
      Target{"remote_pair_collision_filter_candidate",
             reinterpret_cast<LPVOID>(plan.pair_collision_dispatch),
             reinterpret_cast<LPVOID>(&PairCollisionDetour)},
      Target{"hit_player_filter_candidate",
             reinterpret_cast<LPVOID>(plan.hit_player),
             reinterpret_cast<LPVOID>(&HitPlayerDetour)},
      Target{"pre_actor_registry_reset_candidate",
             reinterpret_cast<LPVOID>(plan.pre_actor_registry_reset),
             reinterpret_cast<LPVOID>(&PreRegistryResetDetour)},
      Target{"pre_save_remote_purge_candidate",
             reinterpret_cast<LPVOID>(plan.pre_save),
             reinterpret_cast<LPVOID>(&PreSaveDetour)},
  };
  for (const auto& target : targets) {
    if (!live_target_matches(*verification.profile, target.name,
                             reinterpret_cast<std::uintptr_t>(target.address),
                             error)) {
      return false;
    }
  }

  std::array<LPVOID, targets.size()> originals{};
  std::array<LPVOID, targets.size()> created_targets{};
  std::size_t created_count{};
  for (std::size_t index = 0; index < targets.size(); ++index) {
    const auto status = MH_CreateHook(targets[index].address,
                                      targets[index].detour,
                                      &originals[index]);
    if (status != MH_OK) {
      remove_created_hooks(
          std::span(created_targets).first(created_count));
      clear_originals();
      error = "cannot create actor diagnostic hook ";
      error += targets[index].name;
      error += ": ";
      error += MH_StatusToString(status);
      return false;
    }
    created_targets[created_count++] = targets[index].address;
  }

  g_original_moving_item_resolver =
      reinterpret_cast<MovingItemResolver>(originals[0]);
  g_original_pair_collision = reinterpret_cast<PairCollision>(originals[1]);
  g_original_hit_player = reinterpret_cast<HitPlayer>(originals[2]);
  g_original_pre_registry_reset =
      reinterpret_cast<PreRegistryReset>(originals[3]);
  g_original_pre_save = reinterpret_cast<PreSave>(originals[4]);

  // Establish the empty observation state before any detour can run.
  g_resolver_counter.thread.Reset();
  g_collision_counter.thread.Reset();
  g_hit_counter.thread.Reset();
  g_reset_counter.thread.Reset();
  g_save_counter.thread.Reset();

  for (const auto& target : targets) {
    const auto status = MH_QueueEnableHook(target.address);
    if (status != MH_OK) {
      remove_created_hooks(std::span(created_targets).first(created_count));
      clear_originals();
      error = "cannot queue actor diagnostic hook ";
      error += target.name;
      error += ": ";
      error += MH_StatusToString(status);
      return false;
    }
  }
  const auto applied = MH_ApplyQueued();
  if (applied != MH_OK) {
    remove_created_hooks(std::span(created_targets).first(created_count));
    clear_originals();
    error = "cannot enable actor diagnostic hooks: ";
    error += MH_StatusToString(applied);
    return false;
  }

  InterlockedExchange(&g_ready, 1);
  return true;
}

ActorBoundaryStats snapshot(BoundaryCounter& counter) noexcept {
  ActorBoundaryStats result;
  result.invocations = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&counter.invocations, 0, 0));
  const auto thread = counter.thread.Snapshot();
  result.thread_id = thread.thread_id;
  result.thread_mismatches = thread.mismatches;
  return result;
}

} // namespace

bool InitializeGogActorDiagnostics(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base,
    std::string& error) noexcept {
  try {
    error.clear();
    return initialize_impl(verification, requested_profile_id, module_base,
                           error);
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  } catch (...) {
    error = "unknown exception while initializing actor diagnostics";
    return false;
  }
}

bool ActorDiagnosticsReady() noexcept {
  return InterlockedCompareExchange(&g_ready, 0, 0) != 0;
}

ActorDiagnosticStats GetActorDiagnosticStats() noexcept {
  ActorDiagnosticStats result;
  result.ready = ActorDiagnosticsReady();
  result.moving_item_resolver = snapshot(g_resolver_counter);
  result.pair_collision = snapshot(g_collision_counter);
  result.hit_player = snapshot(g_hit_counter);
  result.pre_registry_reset = snapshot(g_reset_counter);
  result.pre_save = snapshot(g_save_counter);
  return result;
}

} // namespace ht2mp::bridge
