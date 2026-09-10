#include "auto_enter.hpp"

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
#include <limits>
#include <span>
#include <string>

namespace ht2mp::bridge {
namespace {

using MainMenuActivate = void(__thiscall*)(void*);
using MainMenuInput = int(__thiscall*)(void*, const void*);
using MenuEvent = void(__thiscall*)(void*, std::uint32_t);

MainMenuActivate g_original_main_menu_activate{};
MainMenuInput g_original_main_menu_input{};
MenuEvent g_main_menu_event{};
MenuEvent g_single_player_event{};
GogAutoEnterPlan g_plan{};
HookThreadGuard g_thread_guard;
volatile LONG g_ready{};
volatile LONG g_state{static_cast<LONG>(AutoEnterState::disabled)};
volatile LONG g_failure{static_cast<LONG>(AutoEnterFailure::none)};
volatile LONG g_hook_invocations{};
volatile LONG g_attempts{};

const ht2mp::game::SymbolDescriptor* find_descriptor(
    const ht2mp::game::GameProfile& profile,
    const std::string_view name) noexcept {
  const auto found = std::find_if(
      profile.symbols.begin(), profile.symbols.end(),
      [name](const auto& value) { return value.name == name; });
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

bool copy_bytes_seh(const std::uintptr_t address, void* destination,
                    const std::size_t size) noexcept {
  if (address == 0U || destination == nullptr || size == 0U ||
      address > std::numeric_limits<std::uintptr_t>::max() - size) {
    return false;
  }
  MEMORY_BASIC_INFORMATION memory{};
  if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                   sizeof(memory)) != sizeof(memory) ||
      memory.State != MEM_COMMIT ||
      (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
    return false;
  }
  const auto begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
  if (address < begin || address - begin > memory.RegionSize ||
      size > memory.RegionSize - (address - begin)) {
    return false;
  }
  __try {
    std::memcpy(destination, reinterpret_cast<const void*>(address), size);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool live_symbol_matches(const ht2mp::game::GameProfile& profile,
                         const std::string_view name,
                         const std::uintptr_t address,
                         std::string& error) {
  const auto* descriptor = find_descriptor(profile, name);
  if (descriptor == nullptr) {
    error = "auto-enter descriptor is unavailable: ";
    error += name;
    return false;
  }
  ht2mp::game::MaskedPattern pattern;
  if (!ht2mp::game::ParseMaskedPattern(descriptor->ida_pattern, pattern,
                                        error) ||
      pattern.empty() || pattern.size() > 96U) {
    if (error.empty()) error = "auto-enter pattern has an invalid size";
    return false;
  }
  if (!executable_page(address, pattern.size())) {
    error = "auto-enter target is not executable: ";
    error += name;
    return false;
  }
  std::array<std::uint8_t, 96> live{};
  if (!copy_bytes_seh(address, live.data(), pattern.size())) {
    error = "cannot copy live auto-enter bytes: ";
    error += name;
    return false;
  }
  for (std::size_t index = 0U; index < pattern.size(); ++index) {
    if (pattern.mask[index] != 0U && live[index] != pattern.bytes[index]) {
      error = "live auto-enter bytes differ from verified king.exe: ";
      error += name;
      return false;
    }
  }
  return true;
}

bool object_has_vtables(const void* object, const std::uintptr_t primary,
                        const std::uintptr_t event) noexcept {
  if (object == nullptr || g_plan.event_subobject_offset == 0U) return false;
  const auto address = reinterpret_cast<std::uintptr_t>(object);
  if (address > std::numeric_limits<std::uintptr_t>::max() -
                    g_plan.event_subobject_offset - sizeof(std::uintptr_t)) {
    return false;
  }
  std::uintptr_t live_primary{};
  std::uintptr_t live_event{};
  return copy_bytes_seh(address, &live_primary, sizeof(live_primary)) &&
         copy_bytes_seh(address + g_plan.event_subobject_offset, &live_event,
                        sizeof(live_event)) &&
         live_primary == primary && live_event == event;
}

void fail(const AutoEnterFailure failure) noexcept {
  InterlockedExchange(&g_failure, static_cast<LONG>(failure));
  InterlockedExchange(&g_state, static_cast<LONG>(AutoEnterState::failed));
}

void try_dispatch(void* main_menu) {
  if (InterlockedCompareExchange(
          &g_state, static_cast<LONG>(AutoEnterState::armed),
          static_cast<LONG>(AutoEnterState::armed)) !=
      static_cast<LONG>(AutoEnterState::armed)) {
    return;
  }
  if (!g_thread_guard.Observe(GetCurrentThreadId())) {
    fail(AutoEnterFailure::thread_mismatch);
    return;
  }
  if (!object_has_vtables(main_menu, g_plan.main_menu_vtable,
                          g_plan.main_menu_event_vtable)) {
    fail(AutoEnterFailure::main_object_mismatch);
    return;
  }
  std::uintptr_t single_player{};
  if (!copy_bytes_seh(g_plan.single_player_panel_cell, &single_player,
                      sizeof(single_player)) ||
      single_player == 0U) {
    // During early activation the global child may not exist yet.  Leave the
    // one-shot armed; the input fallback will retry on the same UI thread.
    return;
  }
  if (!object_has_vtables(reinterpret_cast<void*>(single_player),
                          g_plan.single_player_vtable,
                          g_plan.single_player_event_vtable)) {
    fail(AutoEnterFailure::single_player_object_mismatch);
    return;
  }
  if (InterlockedCompareExchange(
          &g_state, static_cast<LONG>(AutoEnterState::dispatching),
          static_cast<LONG>(AutoEnterState::armed)) !=
      static_cast<LONG>(AutoEnterState::armed)) {
    return;
  }
  InterlockedIncrement(&g_attempts);

  auto* main_event_this = reinterpret_cast<void*>(
      reinterpret_cast<std::uintptr_t>(main_menu) +
      g_plan.event_subobject_offset);
  g_main_menu_event(main_event_this, g_plan.single_player_widget_id);

  std::uintptr_t selected_panel{};
  if (!copy_bytes_seh(g_plan.single_player_panel_cell, &selected_panel,
                      sizeof(selected_panel)) ||
      selected_panel != single_player ||
      !object_has_vtables(reinterpret_cast<void*>(selected_panel),
                          g_plan.single_player_vtable,
                          g_plan.single_player_event_vtable)) {
    fail(AutoEnterFailure::panel_changed_during_dispatch);
    return;
  }
  auto* single_event_this = reinterpret_cast<void*>(
      selected_panel + g_plan.event_subobject_offset);
  g_single_player_event(single_event_this, g_plan.load_widget_id);
  InterlockedExchange(&g_state,
                      static_cast<LONG>(AutoEnterState::load_dispatched));
}

void __fastcall MainMenuActivateDetour(void* self, void*) {
  const auto original = g_original_main_menu_activate;
  if (original != nullptr) original(self);
  InterlockedIncrement(&g_hook_invocations);
  try_dispatch(self);
}

int __fastcall MainMenuInputDetour(void* self, void*, const void* message) {
  const auto original = g_original_main_menu_input;
  const auto result = original == nullptr ? 0 : original(self, message);
  InterlockedIncrement(&g_hook_invocations);
  try_dispatch(self);
  return result;
}

bool initialize_impl(const ht2mp::game::ProfileVerification& verification,
                     const std::string_view requested_profile_id,
                     const std::uintptr_t module_base,
                     std::string& error) {
  GogAutoEnterPlan plan;
  if (!BuildGogAutoEnterPlan(verification, requested_profile_id, module_base,
                             plan, error)) {
    return false;
  }
  const auto& profile = *verification.profile;
  if (!live_symbol_matches(profile, profile.auto_enter_world.main_menu_activate_symbol,
                           plan.main_menu_activate, error) ||
      !live_symbol_matches(profile, profile.auto_enter_world.main_menu_input_symbol,
                           plan.main_menu_input, error) ||
      !live_symbol_matches(profile, profile.auto_enter_world.main_menu_event_symbol,
                           plan.main_menu_event, error) ||
      !live_symbol_matches(profile, profile.auto_enter_world.single_player_event_symbol,
                           plan.single_player_event, error)) {
    return false;
  }

  LPVOID original_activate{};
  auto status = MH_CreateHook(reinterpret_cast<LPVOID>(plan.main_menu_activate),
                              reinterpret_cast<LPVOID>(&MainMenuActivateDetour),
                              &original_activate);
  if (status != MH_OK) {
    error = MH_StatusToString(status);
    return false;
  }
  LPVOID original_input{};
  status = MH_CreateHook(reinterpret_cast<LPVOID>(plan.main_menu_input),
                         reinterpret_cast<LPVOID>(&MainMenuInputDetour),
                         &original_input);
  if (status != MH_OK) {
    (void)MH_RemoveHook(reinterpret_cast<LPVOID>(plan.main_menu_activate));
    error = MH_StatusToString(status);
    return false;
  }

  g_plan = plan;
  g_original_main_menu_activate =
      reinterpret_cast<MainMenuActivate>(original_activate);
  g_original_main_menu_input = reinterpret_cast<MainMenuInput>(original_input);
  g_main_menu_event = reinterpret_cast<MenuEvent>(plan.main_menu_event);
  g_single_player_event = reinterpret_cast<MenuEvent>(plan.single_player_event);
  g_thread_guard.Reset();
  InterlockedExchange(&g_failure,
                      static_cast<LONG>(AutoEnterFailure::none));
  InterlockedExchange(&g_state, static_cast<LONG>(AutoEnterState::armed));

  status = MH_QueueEnableHook(reinterpret_cast<LPVOID>(plan.main_menu_activate));
  if (status == MH_OK) {
    status = MH_QueueEnableHook(reinterpret_cast<LPVOID>(plan.main_menu_input));
  }
  if (status == MH_OK) status = MH_ApplyQueued();
  if (status != MH_OK) {
    InterlockedExchange(&g_state,
                        static_cast<LONG>(AutoEnterState::disabled));
    (void)MH_RemoveHook(reinterpret_cast<LPVOID>(plan.main_menu_input));
    (void)MH_RemoveHook(reinterpret_cast<LPVOID>(plan.main_menu_activate));
    g_original_main_menu_activate = nullptr;
    g_original_main_menu_input = nullptr;
    g_main_menu_event = nullptr;
    g_single_player_event = nullptr;
    g_plan = {};
    error = MH_StatusToString(status);
    return false;
  }
  InterlockedExchange(&g_ready, 1);
  return true;
}

} // namespace

bool InitializeGogAutoEnter(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base, std::string& error) noexcept {
  try {
    return initialize_impl(verification, requested_profile_id, module_base,
                           error);
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  } catch (...) {
    error = "unknown exception during auto-enter initialization";
    return false;
  }
}

void NotifyAutoEnterWorldReady() noexcept {
  const auto current = InterlockedCompareExchange(&g_state, 0, 0);
  if (current == static_cast<LONG>(AutoEnterState::armed) ||
      current == static_cast<LONG>(AutoEnterState::dispatching) ||
      current == static_cast<LONG>(AutoEnterState::load_dispatched)) {
    InterlockedExchange(&g_state,
                        static_cast<LONG>(AutoEnterState::world_ready));
  }
}

void DisableAutoEnterNoWait() noexcept {
  const auto current = InterlockedCompareExchange(&g_state, 0, 0);
  if (current == static_cast<LONG>(AutoEnterState::armed)) {
    InterlockedExchange(&g_state,
                        static_cast<LONG>(AutoEnterState::disabled));
  }
}

bool AutoEnterReady() noexcept {
  return InterlockedCompareExchange(&g_ready, 0, 0) != 0;
}

AutoEnterStats GetAutoEnterStats() noexcept {
  AutoEnterStats result;
  result.state = static_cast<AutoEnterState>(
      InterlockedCompareExchange(&g_state, 0, 0));
  result.failure = static_cast<AutoEnterFailure>(
      InterlockedCompareExchange(&g_failure, 0, 0));
  result.hook_invocations = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_hook_invocations, 0, 0));
  result.attempts = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_attempts, 0, 0));
  const auto thread = g_thread_guard.Snapshot();
  result.thread_id = thread.thread_id;
  result.thread_mismatches = thread.mismatches;
  return result;
}

} // namespace ht2mp::bridge
