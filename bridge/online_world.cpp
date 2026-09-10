#include "online_world.hpp"

#include "online_world_policy.hpp"
#include "remote_actor_backend.hpp"
#include "runtime_validation.hpp"

#include "ht2mp/game/pattern.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace ht2mp::bridge {
namespace {

using CreatePlayersRegular = void(__cdecl*)();
using UpdateStockActors = void(__cdecl*)();
using KillPlayer = void(__cdecl*)(std::uint32_t);
using GetAssortment = int(__cdecl*)(std::uint32_t, void**, double*);
using OrderVehicle = int(__cdecl*)(std::uint32_t, void*, const void*);
using GoHireIt = void(__cdecl*)(std::uint32_t);
using FocusLossPauseHandler = void(__thiscall*)(void*, BOOL, DWORD);

SteamOnlineWorldPlan g_plan{};
ht2mp::game::AiPlayerPoolLayout g_ai_layout{};
CreatePlayersRegular g_original_create_players_regular{};
UpdateStockActors g_original_update_ghosts{};
UpdateStockActors g_original_update_dealers{};
KillPlayer g_kill_player{};
GetAssortment g_original_get_assortment{};
OrderVehicle g_original_order_vehicle{};
GoHireIt g_original_go_hire_it{};
FocusLossPauseHandler g_original_focus_loss_pause_handler{};

std::atomic_bool g_ready{};
std::atomic_bool g_safe_mode_requested{};
std::atomic_bool g_world_ready{};
std::atomic_bool g_sanitized{};
std::atomic_bool g_background_tick_healthy{};
std::atomic_bool g_rearm_requested{true};
std::atomic<std::uint32_t> g_game_thread_id{};
std::atomic<std::uint32_t> g_generation{};
std::atomic<std::uint32_t> g_local_actors{};
std::atomic<std::uint32_t> g_remote_owned_actors{};
std::atomic<std::uint32_t> g_stock_actors{};
std::atomic<std::uint32_t> g_suppressed_regular{};
std::atomic<std::uint32_t> g_suppressed_ghosts{};
std::atomic<std::uint32_t> g_suppressed_dealers{};
std::atomic<std::uint32_t> g_blocked_assortments{};
std::atomic<std::uint32_t> g_blocked_orders{};
std::atomic<std::uint32_t> g_blocked_hires{};
std::atomic<std::uint32_t> g_focus_loss_events{};
std::atomic<std::uint32_t> g_focus_loss_suppressed{};
std::array<std::atomic<std::uint32_t>, kStockActorTypeCount>
    g_removed_by_type{};
std::atomic<std::uint32_t> g_validation_failures{};
std::atomic<std::uint32_t> g_thread_mismatches{};
std::atomic<std::uint64_t> g_last_tick_ms{};

bool plausible_pointer(const std::uint32_t value) noexcept {
  return value >= 0x00010000U && value < 0x80000000U &&
         (value & 0x3U) == 0U;
}

bool region_allows(const std::uintptr_t address, const std::size_t size,
                   const bool require_write) noexcept {
  if (address == 0U || size == 0U ||
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
  if (!require_write) return true;
  const DWORD protection = memory.Protect & 0xffU;
  return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
         protection == PAGE_EXECUTE_READWRITE ||
         protection == PAGE_EXECUTE_WRITECOPY;
}

bool read_bytes(const std::uintptr_t address, void* destination,
                const std::size_t size) noexcept {
  if (destination == nullptr || !region_allows(address, size, false)) {
    return false;
  }
  __try {
    std::memcpy(destination, reinterpret_cast<const void*>(address), size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool write_bytes(const std::uintptr_t address, const void* source,
                 const std::size_t size) noexcept {
  if (source == nullptr || !region_allows(address, size, true)) return false;
  __try {
    std::memcpy(reinterpret_cast<void*>(address), source, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool read_u32(const std::uintptr_t address, std::uint32_t& value) noexcept {
  return read_bytes(address, &value, sizeof(value));
}

bool add_address(const std::uint32_t base, const std::uint32_t offset,
                 std::uint32_t& result) noexcept {
  if (base > UINT32_MAX - offset) return false;
  result = base + offset;
  return plausible_pointer(result);
}

bool active() noexcept {
  return g_ready.load(std::memory_order_acquire) &&
         !g_safe_mode_requested.load(std::memory_order_acquire);
}

bool suppression_latched() noexcept {
  // Once the exact-build hooks are enabled, never hand stock spawning or
  // parking mutations back to the game in this process. A runtime failure
  // stops further sanitizer writes and remote actors, but recovery requires a
  // fresh online process so native NPCs cannot silently repopulate the world.
  return g_ready.load(std::memory_order_acquire);
}

void fail_runtime() noexcept {
  g_validation_failures.fetch_add(1U, std::memory_order_relaxed);
  g_sanitized.store(false, std::memory_order_release);
  g_background_tick_healthy.store(false, std::memory_order_release);
  g_safe_mode_requested.store(true, std::memory_order_release);
  RequestRemoteActorSafeMode();
}

bool observe_game_thread() noexcept {
  const auto current = GetCurrentThreadId();
  std::uint32_t expected{};
  if (g_game_thread_id.compare_exchange_strong(
          expected, current, std::memory_order_acq_rel,
          std::memory_order_acquire) || expected == current) {
    return true;
  }
  g_thread_mismatches.fetch_add(1U, std::memory_order_relaxed);
  fail_runtime();
  return false;
}

const ht2mp::game::SymbolDescriptor* descriptor(
    const ht2mp::game::GameProfile& profile,
    const std::string_view name) noexcept {
  const auto found = std::find_if(profile.symbols.begin(), profile.symbols.end(),
                                  [name](const auto& value) {
                                    return value.name == name;
                                  });
  return found == profile.symbols.end() ? nullptr : &*found;
}

bool live_target_matches(const ht2mp::game::GameProfile& profile,
                         const std::string_view name,
                         const std::uintptr_t address,
                         std::string& error) {
  const auto* value = descriptor(profile, name);
  if (value == nullptr) {
    error = "online-world descriptor is unavailable: ";
    error += name;
    return false;
  }
  ht2mp::game::MaskedPattern pattern;
  if (!ht2mp::game::ParseMaskedPattern(value->ida_pattern, pattern, error) ||
      pattern.empty() || pattern.size() > 160U) {
    if (error.empty()) error = "online-world signature has invalid size";
    return false;
  }
  if (!region_allows(address, pattern.size(), false)) {
    error = "online-world target is not committed memory: ";
    error += name;
    return false;
  }
  MEMORY_BASIC_INFORMATION memory{};
  (void)VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                     sizeof(memory));
  const DWORD protection = memory.Protect & 0xffU;
  if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
      protection != PAGE_EXECUTE_READWRITE &&
      protection != PAGE_EXECUTE_WRITECOPY) {
    error = "online-world target page is not executable: ";
    error += name;
    return false;
  }
  std::array<std::uint8_t, 160> live{};
  if (!read_bytes(address, live.data(), pattern.size())) {
    error = "cannot read online-world target: ";
    error += name;
    return false;
  }
  for (std::size_t index = 0U; index < pattern.size(); ++index) {
    if (pattern.mask[index] != 0U && live[index] != pattern.bytes[index]) {
      error = "live bytes differ for online-world target: ";
      error += name;
      return false;
    }
  }
  return true;
}

bool read_count_tables(std::uint32_t& current_address,
                       std::uint32_t& target_address,
                       std::int32_t (&current)[kStockActorTypeCount],
                       std::int32_t (&target)[kStockActorTypeCount]) noexcept {
  if (!read_u32(g_plan.current_counts_cell, current_address) ||
      !read_u32(g_plan.target_counts_cell, target_address) ||
      !plausible_pointer(current_address) ||
      !plausible_pointer(target_address) || current_address == target_address ||
      !read_bytes(current_address, current, sizeof(current)) ||
      !read_bytes(target_address, target, sizeof(target))) {
    return false;
  }
  for (std::size_t type = 0U; type < kStockActorTypeCount; ++type) {
    if (current[type] < 0 || current[type] > 100'000 || target[type] < 0 ||
        target[type] > 100'000) {
      return false;
    }
  }
  return region_allows(target_address, sizeof(target), true);
}

void __cdecl CreatePlayersRegularDetour() {
  const auto original = g_original_create_players_regular;
  if (original == nullptr) {
    fail_runtime();
    return;
  }
  if (!suppression_latched()) {
    original();
    return;
  }
  if (!active()) {
    g_suppressed_regular.fetch_add(1U, std::memory_order_relaxed);
    return;
  }
  if (!observe_game_thread()) {
    return;
  }

  std::uint32_t current_address{};
  std::uint32_t target_address{};
  std::int32_t current[kStockActorTypeCount]{};
  std::int32_t saved[kStockActorTypeCount]{};
  if (!read_count_tables(current_address, target_address, current, saved)) {
    fail_runtime();
    return;
  }
  std::int32_t clamped[kStockActorTypeCount]{};
  std::memcpy(clamped, saved, sizeof(clamped));
  for (std::size_t type = 0U; type < kStockActorTypeCount; ++type) {
    if (type != 1U) clamped[type] = current[type];
  }
  if (!write_bytes(target_address, clamped, sizeof(clamped))) {
    fail_runtime();
    return;
  }
  g_suppressed_regular.fetch_add(1U, std::memory_order_relaxed);
  __try {
    original();
  } __finally {
    if (!write_bytes(target_address, saved, sizeof(saved))) fail_runtime();
  }
}

void __cdecl UpdateGhostsDetour() {
  if (suppression_latched()) {
    g_suppressed_ghosts.fetch_add(1U, std::memory_order_relaxed);
    return;
  }
  const auto original = g_original_update_ghosts;
  if (original != nullptr) original();
}

void __cdecl UpdateDealersDetour() {
  if (suppression_latched()) {
    g_suppressed_dealers.fetch_add(1U, std::memory_order_relaxed);
    return;
  }
  const auto original = g_original_update_dealers;
  if (original != nullptr) original();
}

int __cdecl GetAssortmentDetour(const std::uint32_t parking_id,
                                void** assortment, double* price) {
  if (suppression_latched()) {
    (void)parking_id;
    (void)price;
    if (assortment != nullptr) {
      void* empty{};
      (void)write_bytes(reinterpret_cast<std::uintptr_t>(assortment), &empty,
                        sizeof(empty));
    }
    g_blocked_assortments.fetch_add(1U, std::memory_order_relaxed);
    return 0;
  }
  const auto original = g_original_get_assortment;
  return original == nullptr ? 0 : original(parking_id, assortment, price);
}

int __cdecl OrderVehicleDetour(const std::uint32_t parking_id, void* offer,
                               const void* order) {
  if (suppression_latched()) {
    (void)parking_id;
    (void)offer;
    (void)order;
    g_blocked_orders.fetch_add(1U, std::memory_order_relaxed);
    return 2; // exact-build native failure result, before money/save mutation
  }
  const auto original = g_original_order_vehicle;
  return original == nullptr ? 2 : original(parking_id, offer, order);
}

void __cdecl GoHireItDetour(const std::uint32_t player_id) {
  if (suppression_latched()) {
    (void)player_id;
    g_blocked_hires.fetch_add(1U, std::memory_order_relaxed);
    return;
  }
  const auto original = g_original_go_hire_it;
  if (original != nullptr) original(player_id);
}

struct WindowSearch final {
  DWORD process_id{};
  bool found{};
};

BOOL CALLBACK FindVisibleWindow(HWND window, LPARAM parameter) noexcept {
  auto* search = reinterpret_cast<WindowSearch*>(parameter);
  DWORD process_id{};
  (void)GetWindowThreadProcessId(window, &process_id);
  if (process_id == search->process_id && IsWindowVisible(window) &&
      !IsIconic(window) && GetWindow(window, GW_OWNER) == nullptr) {
    search->found = true;
    return FALSE;
  }
  return TRUE;
}

bool has_visible_unminimized_window() noexcept {
  WindowSearch search{GetCurrentProcessId(), false};
  (void)EnumWindows(&FindVisibleWindow,
                    reinterpret_cast<LPARAM>(&search));
  return search.found;
}

bool process_has_foreground_window() noexcept {
  const auto foreground = GetForegroundWindow();
  if (foreground == nullptr) return false;
  DWORD process_id{};
  (void)GetWindowThreadProcessId(foreground, &process_id);
  return process_id == GetCurrentProcessId();
}

void __fastcall FocusLossPauseHandlerDetour(void* self, void*, BOOL active_app,
                                            const DWORD thread_id) {
  const auto original = g_original_focus_loss_pause_handler;
  if (original == nullptr) {
    fail_runtime();
    return;
  }
  BOOL forwarded = active_app;
  if (active_app == FALSE) {
    g_focus_loss_events.fetch_add(1U, std::memory_order_relaxed);
  }
  if (ShouldSuppressFocusLoss(active(), active_app != FALSE)) {
    // Do not consult window visibility here. During startup WM_ACTIVATEAPP can
    // race the top-level HWND becoming visible, which permanently paused the
    // first of two freshly launched clients. Escape and close use other paths.
    forwarded = TRUE;
    g_focus_loss_suppressed.fetch_add(1U, std::memory_order_relaxed);
  }
  original(self, forwarded, thread_id);
}

struct RegistryScan final {
  std::uint32_t sentinel{};
  std::uint32_t local_player_id{};
  std::uint32_t first_stock{};
  std::int32_t first_stock_type{-1};
  std::uint32_t local_count{};
  std::uint32_t remote_count{};
  std::uint32_t stock_count{};
};

bool scan_registry(RegistryScan& result) noexcept {
  result = {};
  if (!read_u32(g_plan.ai_player_list_sentinel_cell, result.sentinel) ||
      !read_u32(g_plan.local_player_id_cell, result.local_player_id) ||
      !plausible_pointer(result.sentinel) ||
      !plausible_pointer(result.local_player_id) ||
      result.local_player_id == result.sentinel) {
    return false;
  }
  std::uint32_t current{};
  std::uint32_t sentinel_previous{};
  if (!read_u32(result.sentinel + g_ai_layout.node_next_offset, current) ||
      !read_u32(result.sentinel + g_ai_layout.node_previous_offset,
                sentinel_previous) ||
      !plausible_pointer(current) || !plausible_pointer(sentinel_previous)) {
    return false;
  }
  std::uint32_t previous = result.sentinel;
  std::uint32_t visited{};
  while (current != result.sentinel) {
    if (++visited > g_ai_layout.maximum_nodes || !plausible_pointer(current)) {
      return false;
    }
    std::uint32_t next{};
    std::uint32_t back{};
    std::uint32_t player{};
    std::uint32_t self{};
    std::int32_t actor_type{};
    if (!add_address(current, g_ai_layout.node_player_offset, player) ||
        !read_u32(current + g_ai_layout.node_next_offset, next) ||
        !read_u32(current + g_ai_layout.node_previous_offset, back) ||
        !plausible_pointer(next) || back != previous ||
        !read_u32(player + g_ai_layout.player_self_id_offset, self) ||
        !read_bytes(player + g_ai_layout.player_actor_type_offset, &actor_type,
                    sizeof(actor_type)) ||
        self != current || actor_type < 0 ||
        actor_type >= static_cast<std::int32_t>(kStockActorTypeCount)) {
      return false;
    }
    if (current == result.local_player_id) {
      if (actor_type != g_ai_layout.required_local_actor_type) return false;
      ++result.local_count;
    } else if (RemoteActorOwnsPlayerId(current)) {
      if (actor_type != 2) return false;
      ++result.remote_count;
    } else {
      ++result.stock_count;
      if (result.first_stock == 0U) {
        result.first_stock = current;
        result.first_stock_type = actor_type;
      }
    }
    previous = current;
    current = next;
  }
  std::uint32_t sentinel_after{};
  std::uint32_t local_after{};
  return previous == sentinel_previous && result.local_count == 1U &&
         read_u32(g_plan.ai_player_list_sentinel_cell, sentinel_after) &&
         read_u32(g_plan.local_player_id_cell, local_after) &&
         sentinel_after == result.sentinel &&
         local_after == result.local_player_id;
}

bool call_kill_player(const std::uint32_t player_id) noexcept {
  const auto function = g_kill_player;
  if (function == nullptr || !plausible_pointer(player_id)) return false;
  __try {
    function(player_id);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool sanitize_registry() noexcept {
  for (std::uint32_t removed = 0U;
       removed < g_ai_layout.maximum_nodes; ++removed) {
    RegistryScan scan;
    if (!scan_registry(scan)) return false;
    g_local_actors.store(scan.local_count, std::memory_order_relaxed);
    g_remote_owned_actors.store(scan.remote_count, std::memory_order_relaxed);
    g_stock_actors.store(scan.stock_count, std::memory_order_relaxed);
    if (scan.first_stock == 0U) return true;
    if (scan.first_stock_type < 0 ||
        !call_kill_player(scan.first_stock)) {
      return false;
    }
    g_removed_by_type[static_cast<std::size_t>(scan.first_stock_type)]
        .fetch_add(1U, std::memory_order_relaxed);
  }
  // Reaching the hard limit means killPlayer failed to shrink the verified
  // list or stock creation raced the traversal. Either case fails closed.
  return false;
}

void remove_hooks(const std::span<const LPVOID> targets) noexcept {
  for (const auto target : targets) {
    (void)MH_DisableHook(target);
    (void)MH_RemoveHook(target);
  }
}

bool initialize_impl(const ht2mp::game::ProfileVerification& verification,
                     const std::string_view requested_profile_id,
                     const std::uintptr_t module_base, std::string& error) {
  if (g_ready.load(std::memory_order_acquire)) {
    error = "Steam online world is already initialized";
    return false;
  }
  SteamOnlineWorldPlan plan;
  if (!BuildSteamOnlineWorldPlan(verification, requested_profile_id,
                                 module_base, plan, error)) {
    return false;
  }
  const auto& layout = verification.profile->ai_player_pool;
  if (!layout.observer_enabled || layout.node_next_offset != 0U ||
      layout.node_previous_offset != 4U || layout.node_player_offset != 8U ||
      layout.player_self_id_offset == 0U ||
      layout.player_actor_type_offset == 0U || layout.maximum_nodes == 0U ||
      layout.maximum_nodes > 4096U ||
      layout.required_local_actor_type != 1) {
    error = "exact Steam online actor-list layout is invalid";
    return false;
  }

  struct HookTarget final {
    std::string_view name;
    LPVOID address;
    LPVOID detour;
  };
  const std::array hooks{
      HookTarget{"create_players_regular_online",
                 reinterpret_cast<LPVOID>(plan.create_players_regular),
                 reinterpret_cast<LPVOID>(&CreatePlayersRegularDetour)},
      HookTarget{"update_ghosts_online",
                 reinterpret_cast<LPVOID>(plan.update_ghosts),
                 reinterpret_cast<LPVOID>(&UpdateGhostsDetour)},
      HookTarget{"update_dealers_online",
                 reinterpret_cast<LPVOID>(plan.update_dealers),
                 reinterpret_cast<LPVOID>(&UpdateDealersDetour)},
      HookTarget{"parking_get_assortment_online",
                 reinterpret_cast<LPVOID>(plan.get_assortment),
                 reinterpret_cast<LPVOID>(&GetAssortmentDetour)},
      HookTarget{"parking_order_vehicle_online",
                 reinterpret_cast<LPVOID>(plan.order_vehicle),
                 reinterpret_cast<LPVOID>(&OrderVehicleDetour)},
      HookTarget{"parking_go_hire_it_online",
                 reinterpret_cast<LPVOID>(plan.go_hire_it),
                 reinterpret_cast<LPVOID>(&GoHireItDetour)},
      HookTarget{"focus_loss_pause_handler_online",
                 reinterpret_cast<LPVOID>(plan.focus_loss_pause_handler),
                 reinterpret_cast<LPVOID>(&FocusLossPauseHandlerDetour)},
  };
  for (const auto& hook : hooks) {
    if (!live_target_matches(*verification.profile, hook.name,
                             reinterpret_cast<std::uintptr_t>(hook.address),
                             error)) {
      return false;
    }
  }
  if (!live_target_matches(*verification.profile, "kill_player_active",
                           plan.kill_player, error) ||
      !region_allows(plan.current_counts_cell, sizeof(std::uint32_t), false) ||
      !region_allows(plan.target_counts_cell, sizeof(std::uint32_t), false)) {
    if (error.empty()) error = "online-world count-table cells are unreadable";
    return false;
  }

  std::array<LPVOID, hooks.size()> originals{};
  std::array<LPVOID, hooks.size()> created{};
  std::size_t created_count{};
  for (std::size_t index = 0U; index < hooks.size(); ++index) {
    const auto status = MH_CreateHook(hooks[index].address, hooks[index].detour,
                                      &originals[index]);
    if (status != MH_OK) {
      remove_hooks(std::span(created).first(created_count));
      error = "cannot create online-world hook ";
      error += hooks[index].name;
      error += ": ";
      error += MH_StatusToString(status);
      return false;
    }
    created[created_count++] = hooks[index].address;
  }

  g_plan = plan;
  g_ai_layout = layout;
  g_original_create_players_regular =
      reinterpret_cast<CreatePlayersRegular>(originals[0]);
  g_original_update_ghosts = reinterpret_cast<UpdateStockActors>(originals[1]);
  g_original_update_dealers =
      reinterpret_cast<UpdateStockActors>(originals[2]);
  g_original_get_assortment = reinterpret_cast<GetAssortment>(originals[3]);
  g_original_order_vehicle = reinterpret_cast<OrderVehicle>(originals[4]);
  g_original_go_hire_it = reinterpret_cast<GoHireIt>(originals[5]);
  g_original_focus_loss_pause_handler =
      reinterpret_cast<FocusLossPauseHandler>(originals[6]);
  g_kill_player = reinterpret_cast<KillPlayer>(plan.kill_player);

  g_safe_mode_requested.store(false, std::memory_order_release);
  g_world_ready.store(false, std::memory_order_release);
  g_sanitized.store(false, std::memory_order_release);
  g_background_tick_healthy.store(true, std::memory_order_release);
  g_rearm_requested.store(true, std::memory_order_release);
  g_game_thread_id.store(0U, std::memory_order_release);
  g_last_tick_ms.store(GetTickCount64(), std::memory_order_release);
  g_focus_loss_events.store(0U, std::memory_order_release);
  g_focus_loss_suppressed.store(0U, std::memory_order_release);

  for (const auto& hook : hooks) {
    const auto status = MH_QueueEnableHook(hook.address);
    if (status != MH_OK) {
      remove_hooks(std::span(created).first(created_count));
      error = "cannot queue online-world hook: ";
      error += MH_StatusToString(status);
      return false;
    }
  }
  const auto applied = MH_ApplyQueued();
  if (applied != MH_OK) {
    remove_hooks(std::span(created).first(created_count));
    error = "cannot enable online-world hooks: ";
    error += MH_StatusToString(applied);
    return false;
  }
  g_ready.store(true, std::memory_order_release);
  return true;
}

} // namespace

bool InitializeSteamOnlineWorld(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base, std::string& error) noexcept {
  try {
    error.clear();
    return initialize_impl(verification, requested_profile_id, module_base,
                           error);
  } catch (...) {
    error = "exception while initializing the Steam online world";
    return false;
  }
}

void TickSteamOnlineWorld(const bool local_world_ready) noexcept {
  if (!SteamOnlineWorldReady()) return;
  g_last_tick_ms.store(GetTickCount64(), std::memory_order_release);
  g_background_tick_healthy.store(true, std::memory_order_release);
  if (!active() || !observe_game_thread()) return;
  const bool was_ready = g_world_ready.exchange(
      local_world_ready, std::memory_order_acq_rel);
  if (!local_world_ready) {
    g_sanitized.store(false, std::memory_order_release);
    g_rearm_requested.store(true, std::memory_order_release);
    g_local_actors.store(0U, std::memory_order_relaxed);
    g_remote_owned_actors.store(0U, std::memory_order_relaxed);
    g_stock_actors.store(0U, std::memory_order_relaxed);
    return;
  }
  if (!was_ready || g_rearm_requested.exchange(false,
                                                std::memory_order_acq_rel)) {
    g_generation.fetch_add(1U, std::memory_order_relaxed);
    g_sanitized.store(false, std::memory_order_release);
  }
  // Keep scanning after the first cleanup: mission code may call the central
  // creator directly, which intentionally remains unhooked.
  if (!sanitize_registry()) {
    fail_runtime();
    return;
  }
  g_sanitized.store(true, std::memory_order_release);
}

void NotifySteamOnlineWorldRegistryReset() noexcept {
  if (!SteamOnlineWorldReady()) return;
  g_world_ready.store(false, std::memory_order_release);
  g_sanitized.store(false, std::memory_order_release);
  g_rearm_requested.store(true, std::memory_order_release);
}

void RequestSteamOnlineWorldSafeMode() noexcept {
  g_safe_mode_requested.store(true, std::memory_order_release);
  g_sanitized.store(false, std::memory_order_release);
  g_background_tick_healthy.store(false, std::memory_order_release);
}

void RearmSteamBackgroundTickWatchdog(const std::uint64_t now_ms) noexcept {
  if (!SteamOnlineWorldReady()) return;
  g_last_tick_ms.store(now_ms, std::memory_order_release);
  g_background_tick_healthy.store(true, std::memory_order_release);
}

bool SteamOnlineWorldReady() noexcept {
  return g_ready.load(std::memory_order_acquire);
}

bool SteamBackgroundTickWatchdogExpired(const std::uint64_t now_ms) noexcept {
  if (!active() || !g_world_ready.load(std::memory_order_acquire) ||
      !has_visible_unminimized_window() || process_has_foreground_window()) {
    return false;
  }
  const auto last = g_last_tick_ms.load(std::memory_order_acquire);
  if (last == 0U || now_ms <= last ||
      now_ms - last <= kBackgroundTickWatchdogLimitMs) {
    return false;
  }
  g_background_tick_healthy.store(false, std::memory_order_release);
  return true;
}

OnlineWorldStats GetOnlineWorldStats() noexcept {
  OnlineWorldStats result;
  result.ready = SteamOnlineWorldReady();
  result.safe_mode_requested =
      g_safe_mode_requested.load(std::memory_order_acquire);
  result.world_ready = g_world_ready.load(std::memory_order_acquire);
  result.sanitized = g_sanitized.load(std::memory_order_acquire);
  result.background_tick_healthy =
      g_background_tick_healthy.load(std::memory_order_acquire);
  result.generation = g_generation.load(std::memory_order_relaxed);
  result.local_actors = g_local_actors.load(std::memory_order_relaxed);
  result.remote_owned_actors =
      g_remote_owned_actors.load(std::memory_order_relaxed);
  result.stock_actors = g_stock_actors.load(std::memory_order_relaxed);
  result.suppressed_regular =
      g_suppressed_regular.load(std::memory_order_relaxed);
  result.suppressed_ghosts =
      g_suppressed_ghosts.load(std::memory_order_relaxed);
  result.suppressed_dealers =
      g_suppressed_dealers.load(std::memory_order_relaxed);
  result.blocked_assortments =
      g_blocked_assortments.load(std::memory_order_relaxed);
  result.blocked_orders = g_blocked_orders.load(std::memory_order_relaxed);
  result.blocked_hires = g_blocked_hires.load(std::memory_order_relaxed);
  result.focus_loss_events =
      g_focus_loss_events.load(std::memory_order_relaxed);
  result.focus_loss_suppressed =
      g_focus_loss_suppressed.load(std::memory_order_relaxed);
  for (std::size_t type = 0U; type < result.removed_by_type.size(); ++type) {
    result.removed_by_type[type] =
        g_removed_by_type[type].load(std::memory_order_relaxed);
  }
  result.validation_failures =
      g_validation_failures.load(std::memory_order_relaxed);
  result.thread_mismatches =
      g_thread_mismatches.load(std::memory_order_relaxed);
  const auto now = GetTickCount64();
  const auto last = g_last_tick_ms.load(std::memory_order_acquire);
  result.last_tick_age_ms = now >= last ? now - last : 0U;
  return result;
}

} // namespace ht2mp::bridge
