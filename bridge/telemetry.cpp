#include "telemetry.hpp"
#include "replication.hpp"

#include "actor_diagnostics.hpp"
#include "auto_enter.hpp"
#include "hook_thread_guard.hpp"
#include "online_world.hpp"
#include "runtime_validation.hpp"
#include "remote_actor_backend.hpp"

#include "ht2mp/game/local_world_observer.hpp"
#include "ht2mp/game/pattern.hpp"
#include "ht2mp/game/profile.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace ht2mp::bridge {
namespace {

class InProcessReadOnlyMemory final : public ht2mp::game::ReadOnlyMemory {
public:
  bool Read(const std::uintptr_t address, const std::span<std::byte> destination,
            std::string& error) noexcept override {
    error.clear();
    if (ReadFast(address, destination)) return true;
    error = "in-process telemetry read failed";
    return false;
  }

  bool ReadFast(const std::uintptr_t address,
                const std::span<std::byte> destination) noexcept override {
    if (address == 0U || destination.empty() ||
        address > std::numeric_limits<std::uintptr_t>::max() - destination.size()) {
      return false;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                     sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0U) {
      return false;
    }
    const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    if (address < region_begin || address - region_begin > memory.RegionSize ||
        destination.size() > memory.RegionSize - (address - region_begin)) {
      return false;
    }
    __try {
      std::memcpy(destination.data(), reinterpret_cast<const void*>(address),
                  destination.size());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return false;
    }
    return true;
  }
};

using PostAiTick = void(__cdecl*)();

struct LatestSlot final {
  SRWLOCK lock{SRWLOCK_INIT};
  ht2mp::ipc::PlayerSampleV1 sample{};
  bool pending{};
};

InProcessReadOnlyMemory g_memory;
std::unique_ptr<ht2mp::game::LocalWorldObserver> g_observer;
LatestSlot g_latest;
PostAiTick g_original_post_ai_tick{};
volatile LONG g_hook_ready{};
volatile LONG g_telemetry_enabled{};
volatile LONG g_hook_invocations{};
volatile LONG g_observer_polls{};
volatile LONG g_valid_samples{};
volatile LONG g_wire_sequence{};
volatile LONG g_was_in_world{};
volatile LONG64 g_last_tick_us{};
volatile LONG64 g_max_tick_gap_us{};
// LocalSample cadence is decided here, at capture time, so the interval
// between published samples is exact on the sender clock. The worker then
// forwards a pending sample immediately instead of adding its own gate.
constexpr std::uint64_t kLocalSampleIntervalUs = 45'000U;
std::uint64_t g_last_publish_us{};
HookThreadGuard g_hook_thread_guard;
LARGE_INTEGER g_qpc_frequency{};

std::string_view bounded_string(
    const std::array<char, ht2mp::ipc::kProfileIdCapacity>& value) noexcept {
  const auto end = std::find(value.begin(), value.end(), '\0');
  if (end == value.end()) return {};
  return {value.data(), static_cast<std::size_t>(end - value.begin())};
}

void set_detail(TelemetryInitialization& result,
                const std::string_view detail) noexcept {
  const auto size = std::min(detail.size(), result.detail.size() - 1U);
  if (size != 0U) std::memcpy(result.detail.data(), detail.data(), size);
  result.detail[size] = '\0';
}

bool executable_page(const std::uintptr_t address, const std::size_t size) noexcept {
  MEMORY_BASIC_INFORMATION memory{};
  if (address == 0U || size == 0U ||
      VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                   sizeof(memory)) != sizeof(memory) ||
      memory.State != MEM_COMMIT ||
      (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
    return false;
  }
  const DWORD base_protection = memory.Protect & 0xFFU;
  const bool executable = base_protection == PAGE_EXECUTE ||
                          base_protection == PAGE_EXECUTE_READ ||
                          base_protection == PAGE_EXECUTE_READWRITE ||
                          base_protection == PAGE_EXECUTE_WRITECOPY;
  const auto region_begin = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
  return executable && address >= region_begin &&
         address - region_begin <= memory.RegionSize &&
         size <= memory.RegionSize - (address - region_begin);
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

bool copy_live_bytes_seh(const std::uintptr_t address, std::uint8_t* destination,
                         const std::size_t size) noexcept {
  __try {
    std::memcpy(destination, reinterpret_cast<const void*>(address), size);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

bool live_tick_matches_verified_pattern(
    const ht2mp::game::GameProfile& profile,
    const std::uintptr_t address,
    std::string& error) {
  const auto* descriptor = find_descriptor(profile, "post_ai_tick_candidate");
  if (descriptor == nullptr) {
    error = "post-AI tick descriptor is unavailable";
    return false;
  }
  ht2mp::game::MaskedPattern pattern;
  if (!ht2mp::game::ParseMaskedPattern(descriptor->ida_pattern, pattern, error) ||
      pattern.empty() || pattern.size() > 64U) {
    if (error.empty()) error = "post-AI tick pattern has an invalid size";
    return false;
  }
  if (!executable_page(address, pattern.size())) {
    error = "post-AI tick target is not in committed executable memory";
    return false;
  }
  std::array<std::uint8_t, 64> live{};
  if (!copy_live_bytes_seh(address, live.data(), pattern.size())) {
    error = "structured exception while validating live tick bytes";
    return false;
  }
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    if (pattern.mask[index] != 0U && live[index] != pattern.bytes[index]) {
      error = "live post-AI tick bytes differ from the verified executable";
      return false;
    }
  }
  return true;
}

std::uint64_t monotonic_microseconds() noexcept {
  LARGE_INTEGER counter{};
  if (g_qpc_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&counter)) return 0U;
  const auto seconds = counter.QuadPart / g_qpc_frequency.QuadPart;
  const auto remainder = counter.QuadPart % g_qpc_frequency.QuadPart;
  return static_cast<std::uint64_t>(seconds) * 1'000'000ULL +
         static_cast<std::uint64_t>(remainder * 1'000'000LL /
                                    g_qpc_frequency.QuadPart);
}

void publish(const ht2mp::game::LocalWorldSample& observed) noexcept {
  if (!TryAcquireSRWLockExclusive(&g_latest.lock)) return;
  auto& sample = g_latest.sample;
  sample = {};
  sample.sample_time_ms = observed.transform.monotonic_time_us / 1000U;
  sample.sequence = static_cast<std::uint32_t>(
      InterlockedIncrement(&g_wire_sequence));
  sample.flags = 1U;
  if (observed.transform.teleport) sample.flags |= 4U;
  sample.position = {observed.transform.position.x,
                     observed.transform.position.y,
                     observed.transform.position.z};
  sample.orientation = {observed.transform.orientation.x,
                        observed.transform.orientation.y,
                        observed.transform.orientation.z,
                        observed.transform.orientation.w};
  if (observed.transform.velocity_valid) {
    sample.linear_velocity = {
        static_cast<float>(observed.transform.linear_velocity.x),
        static_cast<float>(observed.transform.linear_velocity.y),
        static_cast<float>(observed.transform.linear_velocity.z)};
  }
  if (observed.transform.angular_velocity_valid) {
    sample.angular_velocity = {
        static_cast<float>(observed.transform.angular_velocity.x),
        static_cast<float>(observed.transform.angular_velocity.y),
        static_cast<float>(observed.transform.angular_velocity.z)};
  }
  sample.vehicle_type = observed.vehicle_type;
  sample.paint_variant = observed.paint_variant;
  CaptureVehicleState(observed.vehicle_instance_address, sample.vehicle);
  sample.location.room_id = observed.current_room_id;
  sample.location.road_id = observed.road_id;
  sample.location.node_id = observed.node_id;
  sample.location.road_distance = observed.road_distance;
  sample.location.road_segment_vector_id =
      observed.road_segment_vector_id;
  sample.location.road_segment_id = observed.road_segment_id;
  sample.location.aux0 = observed.aux0;
  sample.location.aux1 = observed.aux1;
  g_latest.pending = true;
  ReleaseSRWLockExclusive(&g_latest.lock);
}

bool publish_hidden(const std::uint64_t timestamp_us) noexcept {
  if (!TryAcquireSRWLockExclusive(&g_latest.lock)) return false;
  auto& sample = g_latest.sample;
  sample = {};
  sample.sample_time_ms = timestamp_us / 1000U;
  sample.sequence = static_cast<std::uint32_t>(
      InterlockedIncrement(&g_wire_sequence));
  sample.orientation[3] = 1.0F;
  sample.location.room_id = -1;
  sample.location.road_id = -1;
  sample.location.node_id = -1;
  sample.location.road_segment_vector_id = -1;
  sample.location.road_segment_id = -1;
  g_latest.pending = true;
  ReleaseSRWLockExclusive(&g_latest.lock);
  return true;
}

bool poll_after_original(std::int32_t& local_room_id) noexcept {
  local_room_id = -1;
  if (InterlockedCompareExchange(&g_telemetry_enabled, 0, 0) == 0 ||
      g_observer == nullptr) {
    return false;
  }
  InterlockedIncrement(&g_observer_polls);
  const auto timestamp = monotonic_microseconds();
  if (timestamp == 0U) return false;
  const auto result = g_observer->PollFast(timestamp);
  if (result.has_sample()) {
    InterlockedIncrement(&g_valid_samples);
    const bool first_in_world =
        InterlockedCompareExchange(&g_was_in_world, 0, 0) == 0;
    if (first_in_world || result.sample.transform.teleport ||
        timestamp - g_last_publish_us >= kLocalSampleIntervalUs) {
      publish(result.sample);
      g_last_publish_us = timestamp;
    }
    NotifyAutoEnterWorldReady();
    InterlockedExchange(&g_was_in_world, 1);
    local_room_id = result.sample.current_room_id;
    return true;
  } else if (InterlockedCompareExchange(&g_was_in_world, 0, 0) != 0 &&
             publish_hidden(timestamp)) {
    // Send one transition immediately so peers hide this actor on a menu,
    // loading, ownership, list, or room mismatch instead of timing out.
    InterlockedExchange(&g_was_in_world, 0);
  }
  return false;
}

void __cdecl PostAiTickDetour() {
  // Preserve the game's ordering and semantics: the original is always first.
  // We do not catch or translate exceptions originating in game code.
  const auto original = g_original_post_ai_tick;
  if (original != nullptr) original();
  // All HT2MP work below is noexcept and read-only with respect to game data.
  InterlockedIncrement(&g_hook_invocations);
  {
    // Cadence diagnostics: the worker reports the largest gap between ticks so
    // background throttling is measured instead of inferred from an average.
    const auto now_us = monotonic_microseconds();
    const auto previous_us = static_cast<std::uint64_t>(
        InterlockedExchange64(&g_last_tick_us, static_cast<LONG64>(now_us)));
    if (previous_us != 0U && now_us > previous_us) {
      const auto gap = static_cast<LONG64>(now_us - previous_us);
      LONG64 current = InterlockedCompareExchange64(&g_max_tick_gap_us, 0, 0);
      while (gap > current) {
        const auto seen =
            InterlockedCompareExchange64(&g_max_tick_gap_us, gap, current);
        if (seen == current) break;
        current = seen;
      }
    }
  }
  if (!g_hook_thread_guard.Observe(GetCurrentThreadId())) {
    // The selected semantic tick was expected to have one stable owner. A
    // second thread invalidates both the read-only sampling assumption and any
    // future game-thread mutation plan, so stop producing telemetry at once.
    InterlockedExchange(&g_telemetry_enabled, 0);
    RequestRemoteActorSafeMode();
    RequestSteamOnlineWorldSafeMode();
    return;
  }
  std::int32_t local_room_id{-1};
  const auto local_world_ready = poll_after_original(local_room_id);
  TickReplication(local_world_ready);
  TickSteamOnlineWorld(local_world_ready);
  TickGogRemoteActors(local_world_ready, local_room_id);
}

TelemetryInitialization initialize_impl(
    const std::array<char, ht2mp::ipc::kProfileIdCapacity>& profile_id,
    const bool enable_actor_diagnostics,
    const bool enable_remote_actors,
    const bool enable_auto_enter,
    const bool enable_online_world,
    const std::uint32_t selected_vehicle,
    const std::uint32_t selected_paint) {
  TelemetryInitialization result;
  const auto requested = bounded_string(profile_id);
  if (requested != "gog-05588140" && requested != "steam-8138acee") {
    result.status = ht2mp::ipc::BridgeStatusCode::profile_mismatch;
    set_detail(result, "Telemetry is disabled for the requested profile");
    return result;
  }

  std::array<wchar_t, 32768> executable_path{};
  const auto path_size = GetModuleFileNameW(nullptr, executable_path.data(),
                                            static_cast<DWORD>(executable_path.size()));
  if (path_size == 0U || path_size >= executable_path.size()) {
    result.status = ht2mp::ipc::BridgeStatusCode::profile_mismatch;
    set_detail(result, "Cannot resolve the current king.exe path");
    return result;
  }
  const auto verification = ht2mp::game::VerifyGameExecutable(
      std::filesystem::path(executable_path.data()), requested);
  if (!verification.accepted() || !verification.observer_ready()) {
    result.status = ht2mp::ipc::BridgeStatusCode::symbol_resolution_failed;
    set_detail(result, verification.errors.empty()
                           ? "Exact profile or observer verification failed"
                           : verification.errors.front());
    return result;
  }

  const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  GogTelemetryPlan plan;
  std::string validation_error;
  if (!BuildGogTelemetryPlan(verification, requested, module, plan,
                             validation_error) ||
      !live_tick_matches_verified_pattern(*verification.profile,
                                          plan.post_ai_tick_address,
                                          validation_error)) {
    result.status = ht2mp::ipc::BridgeStatusCode::symbol_resolution_failed;
    set_detail(result, validation_error);
    return result;
  }

  std::string observer_error;
  auto observer = ht2mp::game::LocalWorldObserver::Create(
      verification, module, g_memory, observer_error);
  if (!observer) {
    result.status = ht2mp::ipc::BridgeStatusCode::symbol_resolution_failed;
    set_detail(result, observer_error);
    return result;
  }
  if (!QueryPerformanceFrequency(&g_qpc_frequency) ||
      g_qpc_frequency.QuadPart <= 0) {
    result.status = ht2mp::ipc::BridgeStatusCode::telemetry_runtime_error;
    set_detail(result, "High-resolution monotonic clock is unavailable");
    return result;
  }

  const auto initialized = MH_Initialize();
  const bool owns_minhook = initialized == MH_OK;
  if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
    result.status = ht2mp::ipc::BridgeStatusCode::hook_install_failed;
    set_detail(result, MH_StatusToString(initialized));
    return result;
  }
  LPVOID original{};
  const auto created = MH_CreateHook(
      reinterpret_cast<LPVOID>(plan.post_ai_tick_address),
      reinterpret_cast<LPVOID>(&PostAiTickDetour), &original);
  if (created != MH_OK) {
    if (owns_minhook) (void)MH_Uninitialize();
    result.status = ht2mp::ipc::BridgeStatusCode::hook_install_failed;
    set_detail(result, MH_StatusToString(created));
    return result;
  }
  g_original_post_ai_tick = reinterpret_cast<PostAiTick>(original);
  g_observer = std::move(observer);
  g_hook_thread_guard.Reset();
  InterlockedExchange(&g_telemetry_enabled, 0);
  const auto enabled = MH_EnableHook(
      reinterpret_cast<LPVOID>(plan.post_ai_tick_address));
  if (enabled != MH_OK) {
    g_observer.reset();
    g_original_post_ai_tick = nullptr;
    (void)MH_RemoveHook(reinterpret_cast<LPVOID>(plan.post_ai_tick_address));
    if (owns_minhook) (void)MH_Uninitialize();
    result.status = ht2mp::ipc::BridgeStatusCode::hook_install_failed;
    set_detail(result, MH_StatusToString(enabled));
    return result;
  }

  InterlockedExchange(&g_hook_ready, 1);
  InterlockedExchange(&g_telemetry_enabled, 1);
  result.mode = ht2mp::ipc::BridgeMode::observer;
  result.status = ht2mp::ipc::BridgeStatusCode::ok;
  result.capabilities |=
      static_cast<std::uint32_t>(ht2mp::ipc::Capability::local_telemetry);
  std::string auto_enter_error;
  const bool auto_enter_ready =
      !enable_auto_enter ||
      InitializeGogAutoEnter(verification, requested, module,
                             auto_enter_error);
  if (enable_auto_enter && auto_enter_ready) {
    result.capabilities |= static_cast<std::uint32_t>(
        ht2mp::ipc::Capability::auto_enter_world);
  }
  if (enable_online_world) {
    std::string actor_error;
    std::string online_error;
    const bool remote_ready =
        requested == "steam-8138acee" && enable_remote_actors &&
        InitializeGogRemoteActors(verification, requested, module,
                                  selected_vehicle, selected_paint, actor_error);
    const bool online_ready =
        remote_ready && InitializeSteamOnlineWorld(
                            verification, requested, module, online_error);
    if (online_ready) {
      result.mode = ht2mp::ipc::BridgeMode::active;
      result.capabilities |=
          static_cast<std::uint32_t>(ht2mp::ipc::Capability::remote_actors) |
          static_cast<std::uint32_t>(
              ht2mp::ipc::Capability::stock_npc_suppression) |
          static_cast<std::uint32_t>(
              ht2mp::ipc::Capability::background_tick);
      std::string detail =
          "Steam online world active: peer actors, stock-NPC suppression, "
          "parking guard and background tick";
      if (enable_auto_enter && auto_enter_ready) detail += "; auto-enter armed";
      set_detail(result, detail);
    } else {
      RequestRemoteActorSafeMode();
      RequestSteamOnlineWorldSafeMode();
      InterlockedExchange(&g_telemetry_enabled, 0);
      result.mode = ht2mp::ipc::BridgeMode::safe;
      result.status = ht2mp::ipc::BridgeStatusCode::hook_install_failed;
      result.capabilities = static_cast<std::uint32_t>(
          ht2mp::ipc::Capability::observer_mode);
      std::string detail = "Steam online world rejected: ";
      detail += remote_ready ? online_error : actor_error;
      set_detail(result, detail);
    }
  } else if (enable_remote_actors) {
    std::string actor_error;
    if (InitializeGogRemoteActors(verification, requested, module,
                                  selected_vehicle, selected_paint, actor_error)) {
      result.mode = ht2mp::ipc::BridgeMode::active;
      result.capabilities |= static_cast<std::uint32_t>(
          ht2mp::ipc::Capability::remote_actors);
      const auto label = requested == "steam-8138acee" ? "Steam" : "GOG";
      std::string detail = "EXPERIMENTAL ";
      detail += label;
      detail += " dedicated remote actors active";
      if (enable_auto_enter && auto_enter_ready) detail += "; auto-enter armed";
      set_detail(result, detail);
    } else {
      std::string detail = "Exact-profile telemetry active; remote actors rejected: ";
      detail += actor_error;
      set_detail(result, detail);
    }
  } else if (enable_actor_diagnostics) {
    std::string actor_error;
    if (InitializeGogActorDiagnostics(verification, requested, module,
                                      actor_error)) {
      if (enable_auto_enter && auto_enter_ready) {
        set_detail(result,
                   "GOG telemetry + actor diagnostics active; auto-enter armed");
      } else {
        set_detail(result,
                   "GOG telemetry + actor pass-through counters active; actors off");
      }
    } else {
      std::string detail = "GOG telemetry active; actor diagnostics rejected: ";
      detail += actor_error;
      set_detail(result, detail);
    }
  } else if (enable_auto_enter && auto_enter_ready) {
    set_detail(result, requested == "steam-8138acee"
                           ? "Steam telemetry active; native auto-enter armed"
                           : "GOG telemetry active; native auto-enter armed");
  } else {
    set_detail(result, requested == "steam-8138acee"
                           ? "EXPERIMENTAL Steam local VehicleInstance telemetry active; read-only; actors off"
                           : "GOG local VehicleInstance telemetry active; read-only; actors off");
  }
  if (requested == "steam-8138acee" && result.mode == ht2mp::ipc::BridgeMode::active) {
    std::string replication_error;
    if (InitializeSteamReplication(verification, module, replication_error)) {
      result.capabilities |= static_cast<std::uint32_t>(ht2mp::ipc::Capability::vehicle_state) |
                             static_cast<std::uint32_t>(ht2mp::ipc::Capability::environment);
    } else {
      RequestRemoteActorSafeMode();
      RequestSteamOnlineWorldSafeMode();
      result.mode = ht2mp::ipc::BridgeMode::safe;
      result.status = ht2mp::ipc::BridgeStatusCode::hook_install_failed;
      result.capabilities = static_cast<std::uint32_t>(ht2mp::ipc::Capability::observer_mode);
      set_detail(result, replication_error);
    }
  }
  if (enable_auto_enter && !auto_enter_ready) {
    std::string detail = "Exact-profile telemetry active; auto-enter rejected: ";
    detail += auto_enter_error;
    set_detail(result, detail);
  }
  return result;
}

} // namespace

TelemetryInitialization InitializeGogTelemetry(
    const std::array<char, ht2mp::ipc::kProfileIdCapacity>& profile_id,
    const bool enable_actor_diagnostics,
    const bool enable_remote_actors,
    const bool enable_auto_enter,
    const bool enable_online_world,
    const std::uint32_t selected_vehicle,
    const std::uint32_t selected_paint) noexcept {
  try {
    return initialize_impl(profile_id, enable_actor_diagnostics,
                           enable_remote_actors,
                           enable_auto_enter, enable_online_world,
                           selected_vehicle, selected_paint);
  } catch (const std::exception& exception) {
    TelemetryInitialization result;
    result.status = ht2mp::ipc::BridgeStatusCode::telemetry_runtime_error;
    set_detail(result, exception.what());
    return result;
  } catch (...) {
    TelemetryInitialization result;
    result.status = ht2mp::ipc::BridgeStatusCode::telemetry_runtime_error;
    set_detail(result, "Unknown exception during telemetry initialization");
    return result;
  }
}

bool ConsumeLatestLocalSample(ht2mp::ipc::PlayerSampleV1& sample) noexcept {
  AcquireSRWLockExclusive(&g_latest.lock);
  if (!g_latest.pending) {
    ReleaseSRWLockExclusive(&g_latest.lock);
    return false;
  }
  sample = g_latest.sample;
  g_latest.pending = false;
  ReleaseSRWLockExclusive(&g_latest.lock);
  return true;
}

bool SetTelemetryEnabled(const bool enabled) noexcept {
  if (InterlockedCompareExchange(&g_hook_ready, 0, 0) == 0) {
    InterlockedExchange(&g_telemetry_enabled, 0);
    return false;
  }
  if (enabled &&
      g_hook_thread_guard.Snapshot().mismatches != 0U) {
    InterlockedExchange(&g_telemetry_enabled, 0);
    return false;
  }
  InterlockedExchange(&g_telemetry_enabled, enabled ? 1 : 0);
  if (!enabled && InterlockedCompareExchange(&g_was_in_world, 0, 0) != 0 &&
      publish_hidden(monotonic_microseconds())) {
    InterlockedExchange(&g_was_in_world, 0);
  }
  return true;
}

bool TelemetryHookReady() noexcept {
  return InterlockedCompareExchange(&g_hook_ready, 0, 0) != 0;
}

TelemetryHookStats GetTelemetryHookStats() noexcept {
  TelemetryHookStats result;
  result.invocations = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_hook_invocations, 0, 0));
  result.polls = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_observer_polls, 0, 0));
  result.valid_samples = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_valid_samples, 0, 0));
  const auto thread = g_hook_thread_guard.Snapshot();
  result.thread_id = thread.thread_id;
  result.thread_mismatches = thread.mismatches;
  result.last_tick_us = static_cast<std::uint64_t>(
      InterlockedCompareExchange64(&g_last_tick_us, 0, 0));
  result.max_tick_gap_us = static_cast<std::uint64_t>(
      InterlockedCompareExchange64(&g_max_tick_gap_us, 0, 0));
  return result;
}

std::uint64_t ConsumeMaxTickGapUs() noexcept {
  return static_cast<std::uint64_t>(
      InterlockedExchange64(&g_max_tick_gap_us, 0));
}

std::uint64_t MonotonicMicroseconds() noexcept {
  return monotonic_microseconds();
}

void DisableTelemetryNoWait() noexcept {
  InterlockedExchange(&g_telemetry_enabled, 0);
  InterlockedExchange(&g_was_in_world, 0);
}

} // namespace ht2mp::bridge
