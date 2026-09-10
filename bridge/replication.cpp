#include "replication.hpp"
#include "horn.hpp"
#include "remote_actor_backend.hpp"
#include "telemetry.hpp"
#include "wheel_animation.hpp"
#include "ht2mp/game/pattern.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>

namespace ht2mp::bridge {
namespace {
using GetState = int(__thiscall*)(void*, float*);
using SetState = int(__thiscall*)(void*, const float*);
using Update = void(__thiscall*)(void*);
using DeltaTime = float(__cdecl*)();
GetState g_get_state{};
SetState g_set_state{};
Update g_lights{}, g_weather{}, g_wheel_step{};
DeltaTime g_delta_time{};
std::uintptr_t g_local_vehicle{}, g_local_physics{};
ht2mp::protocol::WheelSpeeds g_native_wheels{};
std::atomic<bool> g_enabled{}, g_in_world{};
std::atomic<DWORD> g_owner{};
std::uintptr_t g_base{};
SRWLOCK g_environment_lock = SRWLOCK_INIT;
ht2mp::ipc::EnvironmentV1 g_pending{}, g_anchor{};
bool g_has_pending{};
std::atomic<std::uint32_t> g_captured{}, g_applied{}, g_environment_ticks{}, g_failures{};
std::atomic<std::int32_t> g_steering{};
std::atomic<std::uint32_t> g_headlights{};
std::atomic<float> g_deformation{}, g_day_hours{}, g_weather_phase{};
std::atomic<std::uint32_t> g_wheel_steps{};
std::atomic<float> g_local_wheel_speed{}, g_remote_wheel_speed{}, g_remote_wheel_phase{};
constexpr std::array<std::uint32_t, 4> kLampOffsets{0x5178, 0x517c, 0x51b4, 0x51b8};

bool owner_thread() noexcept {
  return g_enabled.load() && g_owner.load() != 0 && g_owner.load() == GetCurrentThreadId();
}

void fail() noexcept {
  ++g_failures;
  g_enabled = false;
  RequestRemoteActorSafeMode();
}

bool capture_wheel_step(void* self) noexcept {
  __try {
    const auto physics = reinterpret_cast<std::uintptr_t>(self);
    if (*reinterpret_cast<const std::uint32_t*>(physics + 0x29d4) != g_local_vehicle + 0x10) return false;
    const auto count = *reinterpret_cast<const std::uint32_t*>(physics + 0xdd4);
    const float seconds = g_delta_time();
    if (count < 2 || count > ht2mp::protocol::kMaxVehicleWheels ||
        !std::isfinite(seconds) || seconds < 0.0F || seconds > 1.0F) return false;
    g_native_wheels = {};
    g_native_wheels.count = static_cast<std::uint8_t>(count);
    if (seconds > 1.0e-6F) {
      for (std::size_t i = 0; i < count; ++i) {
        // Car_V::run2 adds this signed per-substep increment to the angle
        // at +0xe9c. Read dt here while the physics substep override is active.
        const auto delta = *reinterpret_cast<const float*>(physics + 0xebc + i * 4);
        const auto speed = delta / seconds;
        if (!std::isfinite(speed) || std::abs(speed) > 2000.0F) return false;
        g_native_wheels.radians_per_second[i] = std::abs(speed) < 0.005F ? 0.0F : speed;
      }
    }
    ++g_wheel_steps;
    g_local_wheel_speed = g_native_wheels.radians_per_second[0];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __fastcall WheelStepDetour(void* self, void*) {
  g_wheel_step(self);
  if (owner_thread() && g_in_world.load() &&
      reinterpret_cast<std::uintptr_t>(self) == g_local_physics &&
      !capture_wheel_step(self)) fail();
}

// SEH only protects our integration boundary. Native originals outside these
// helpers retain their original exception behavior.
bool capture(void* self, ht2mp::protocol::VehicleState& state) noexcept {
  __try {
    const auto address = reinterpret_cast<std::uintptr_t>(self);
    if (g_get_state(self, state.condition.data()) != 49) return false;
    // The physics integrator can undershoot zero while settling after a hit
    // (observed chassis channels 11/12: -0.00009/-0.00036). The native setter
    // accepts normalized conditions. Quantize these finite boundary overshoots
    // here, before wire validation; a real collision must not disable the mod.
    for (std::size_t i = 0; i < state.condition.size(); ++i) {
      if (!std::isfinite(state.condition[i])) return false;
      if (i != 41) state.condition[i] = std::clamp(state.condition[i], 0.0F, 1.0F);
    }
    state.steering = *reinterpret_cast<const std::int8_t*>(address + 0x27c4);
    const auto physics = *reinterpret_cast<const std::uint32_t*>(address + 0x5460);
    if (g_local_vehicle != address || g_local_physics != physics) {
      g_local_vehicle = address;
      g_local_physics = physics;
      g_native_wheels = {};
    }
    const auto visible_wheels = *reinterpret_cast<const std::uint32_t*>(address + 0x28b8);
    if (visible_wheels < 2 || visible_wheels > ht2mp::protocol::kMaxVehicleWheels) return false;
    if (g_native_wheels.count != 0) {
      state.wheels.count = static_cast<std::uint8_t>(visible_wheels);
      for (std::size_t i = 0; i < visible_wheels; ++i) {
        // Native extra visual wheels reuse physical i-2 / i-4 rotations.
        const auto physical = i < g_native_wheels.count ? i :
            (i >= 2 && i - 2 < g_native_wheels.count ? i - 2 :
             (i >= 4 && i - 4 < g_native_wheels.count ? i - 4 : ht2mp::protocol::kMaxVehicleWheels));
        if (physical < g_native_wheels.count)
          state.wheels.radians_per_second[i] = g_native_wheels.radians_per_second[physical];
      }
    }
    for (std::size_t i = 0; i < kLampOffsets.size(); ++i) {
      const auto lamp = *reinterpret_cast<const std::uint32_t*>(address + kLampOffsets[i]);
      if (lamp > 2U) return false;
      state.lights[i] = static_cast<std::uint8_t>(lamp);
    }
    if (!CaptureHorn(static_cast<std::uint32_t>(address), state.horn)) return false;
    state.valid = true;
    return ht2mp::protocol::valid_vehicle_state(state);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool apply(void* self, const ht2mp::protocol::VehicleState& state,
           const std::array<float, ht2mp::protocol::kMaxVehicleWheels>& phases) noexcept {
  __try {
    const auto address = reinterpret_cast<std::uintptr_t>(self);
    // Reapply through the game's setter, including discrete damage stages.
    // This also accepts genuine repairs made on the owning client.
    if (g_set_state(self, state.condition.data()) != 49) return false;
    *reinterpret_cast<std::int8_t*>(address + 0x27c4) = state.steering;
    // M11b keeps remotes in simulated mode. In that mode Vehicle::render
    // skips its cheap wheel rebuild and consumes the cached matrices instead.
    // Match make_new_Pos: Rx(wheel spin) * Rz(steering), preserving suspension
    // translations. Both front wheels use the signed -127..127 / 45-degree yaw.
    const auto wheels = *reinterpret_cast<const std::uint32_t*>(address + 0x28b8);
    if (wheels < 2 || wheels > 8) return false;
    const float yaw = static_cast<float>(state.steering) * 0.7853981852531433F * (1.0F / 127.0F);
    if (state.wheels.count != 0 && state.wheels.count != wheels) return false;
    const auto animated_wheels = state.wheels.count != 0 ? wheels : 2U;
    for (std::size_t i = 0; i < animated_wheels; ++i) {
      const float spin = state.wheels.count != 0 ? phases[i] :
          *reinterpret_cast<const float*>(address + 0x27d0 + i * 4);
      if (!std::isfinite(spin)) return false;
      const auto wheel = WheelRotation(spin, i < 2 ? yaw : 0.0F);
      *reinterpret_cast<float*>(address + 0x27d0 + i * 4) = spin;
      std::memcpy(reinterpret_cast<void*>(address + 0x25b4 + i * 0x30), wheel.data(), sizeof(wheel));
    }
    for (std::size_t i = 0; i < kLampOffsets.size(); ++i)
      *reinterpret_cast<std::uint32_t*>(address + kLampOffsets[i]) = state.lights[i];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __fastcall LightsDetour(void* self, void*) {
  ht2mp::protocol::VehicleState state;
  std::array<float, ht2mp::protocol::kMaxVehicleWheels> phases{};
  if (owner_thread() && GetRemoteVehicleState(
          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(self)), state, phases) && state.valid) {
    if (apply(self, state, phases)) {
      ++g_applied;
      g_steering = state.steering;
      g_headlights = state.lights[2];
      g_deformation = *std::max_element(state.condition.begin(), state.condition.begin() + 9);
      g_remote_wheel_speed = state.wheels.radians_per_second[0];
      g_remote_wheel_phase = phases[0];
    } else fail();
  }
  g_lights(self);
}

bool prepare_weather(void* self, const ht2mp::protocol::EnvironmentState& state,
                     std::uint32_t& flags, std::uint32_t& demo) noexcept {
  __try {
    const auto address = reinterpret_cast<std::uintptr_t>(self);
    if (*reinterpret_cast<void**>(g_base + 0x3025b8) != self) return false;
    *reinterpret_cast<double*>(address + 0x58) = state.day_hours;
    *reinterpret_cast<double*>(address + 0x68) = state.weather_phase;
    *reinterpret_cast<double*>(address + 0x78) = state.variation_phase;
    *reinterpret_cast<float*>(address + 0x220) = state.weather_bias;
    // Both palette endpoints use the server-selected WEATHER1..8 preset.
    *reinterpret_cast<std::uint32_t*>(g_base + 0x2fb3d0) = state.weather_preset;
    *reinterpret_cast<std::uint32_t*>(g_base + 0x2fb3d8) = state.weather_preset;
    *reinterpret_cast<double*>(g_base + 0x2fb3e8) = 0.0;
    flags = *reinterpret_cast<std::uint32_t*>(address + 0x23c);
    demo = *reinterpret_cast<std::uint32_t*>(address + 0x238);
    *reinterpret_cast<std::uint32_t*>(address + 0x23c) = flags | 0x10000U;
    *reinterpret_cast<std::uint32_t*>(address + 0x238) = 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void restore_weather_flags(void* self, std::uint32_t flags, std::uint32_t demo) noexcept {
  __try {
    const auto address = reinterpret_cast<std::uintptr_t>(self);
    *reinterpret_cast<std::uint32_t*>(address + 0x23c) = flags;
    *reinterpret_cast<std::uint32_t*>(address + 0x238) = demo;
  } __except (EXCEPTION_EXECUTE_HANDLER) { fail(); }
}

void __fastcall WeatherDetour(void* self, void*) {
  bool prepared = false;
  std::uint32_t flags{}, demo{};
  if (owner_thread() && g_in_world.load()) {
    if (TryAcquireSRWLockExclusive(&g_environment_lock)) {
      if (g_has_pending) { g_anchor = g_pending; g_has_pending = false; }
      ReleaseSRWLockExclusive(&g_environment_lock);
    }
    const auto now = MonotonicMicroseconds();
    if (g_anchor.state.session_id != 0 && now >= g_anchor.receive_time_us &&
        now - g_anchor.receive_time_us <= 3'000'000U) {
      const auto state = ht2mp::protocol::advance_environment(
          g_anchor.state, static_cast<double>(now - g_anchor.receive_time_us) / 1'000'000.0);
      prepared = prepare_weather(self, state, flags, demo);
      if (prepared) {
        ++g_environment_ticks;
        g_day_hours = static_cast<float>(state.day_hours);
        g_weather_phase = static_cast<float>(state.weather_phase);
      } else fail();
    }
  }
  g_weather(self);
  if (prepared) restore_weather_flags(self, flags, demo);
}

bool live_match(std::uintptr_t address, const ht2mp::game::MaskedPattern& pattern) noexcept {
  __try {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
    for (std::size_t i = 0; i < pattern.size(); ++i)
      if (pattern.mask[i] && bytes[i] != pattern.bytes[i]) return false;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool InitializeSteamReplication(const ht2mp::game::ProfileVerification& verified,
                                std::uintptr_t base, std::string& error) {
  if (!verified.accepted() || verified.profile->id != "steam-8138acee" ||
      base != verified.profile->pe.image_base) {
    error = "replication requires the verified exact-Steam image";
    return false;
  }
  struct Target { const char* name; std::uint32_t rva; };
  constexpr std::array<Target, 6> targets{{
      {"vehicle_get_condition", 0x146a30}, {"vehicle_set_condition", 0x146900},
      {"vehicle_lamp_render", 0x146d10}, {"weather_update", 0x1a9dc0},
      {"physics_delta_time", 0xe2749}, {"physics_wheel_step", 0xf35f0}}};
  std::array<std::uintptr_t, 6> addresses{};
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const auto* symbol = verified.FindSymbol(targets[i].name);
    const auto descriptor = std::find_if(verified.profile->symbols.begin(), verified.profile->symbols.end(),
        [&](const auto& item) { return item.name == targets[i].name; });
    ht2mp::game::MaskedPattern pattern;
    if (!symbol || !symbol->accepted || symbol->result_rva != targets[i].rva ||
        descriptor == verified.profile->symbols.end() ||
        !ht2mp::game::ParseMaskedPattern(descriptor->ida_pattern, pattern, error) ||
        pattern.empty() || !live_match(base + symbol->result_rva, pattern)) {
      error = std::string("replication target validation failed: ") + targets[i].name;
      return false;
    }
    addresses[i] = base + symbol->result_rva;
  }
  g_base = base;
  g_get_state = reinterpret_cast<GetState>(addresses[0]);
  g_set_state = reinterpret_cast<SetState>(addresses[1]);
  g_delta_time = reinterpret_cast<DeltaTime>(addresses[4]);
  const std::array<LPVOID, 3> detours{reinterpret_cast<LPVOID>(&LightsDetour), reinterpret_cast<LPVOID>(&WeatherDetour), reinterpret_cast<LPVOID>(&WheelStepDetour)};
  const std::array<LPVOID*, 3> originals{reinterpret_cast<LPVOID*>(&g_lights), reinterpret_cast<LPVOID*>(&g_weather), reinterpret_cast<LPVOID*>(&g_wheel_step)};
  constexpr std::array<std::size_t, 3> hooked{2, 3, 5};
  std::size_t created = 0;
  for (; created < hooked.size(); ++created) {
    if (MH_CreateHook(reinterpret_cast<LPVOID>(addresses[hooked[created]]), detours[created], originals[created]) != MH_OK) break;
  }
  if (created == hooked.size()) {
    for (std::size_t i = 0; i < hooked.size(); ++i) {
      if (MH_EnableHook(reinterpret_cast<LPVOID>(addresses[hooked[i]])) != MH_OK) {
        goto rollback;
      }
    }
    if (!InitializeSteamHorn(verified, base, error)) goto rollback;
    g_enabled = true;
    return true;
  }
rollback:
  for (std::size_t i = 0; i < created; ++i) {
    MH_DisableHook(reinterpret_cast<LPVOID>(addresses[hooked[i]]));
    MH_RemoveHook(reinterpret_cast<LPVOID>(addresses[hooked[i]]));
  }
  error = "could not install exact-Steam replication hooks";
  return false;
}

void CaptureVehicleState(std::uint32_t vehicle, ht2mp::protocol::VehicleState& state) noexcept {
  state = {};
  if (!owner_thread() || !vehicle) return;
  if (capture(reinterpret_cast<void*>(vehicle), state)) ++g_captured;
  else { state = {}; fail(); }
}

bool QueueEnvironment(const ht2mp::ipc::EnvironmentV1& command) noexcept {
  if (!g_enabled.load() || !ht2mp::protocol::valid_environment(command.state) ||
      command.state.session_id == 0 || command.receive_time_us == 0) return false;
  AcquireSRWLockExclusive(&g_environment_lock);
  if (g_pending.state.session_id != command.state.session_id ||
      command.server_time_ms > g_pending.server_time_ms) {
    g_pending = command;
    g_has_pending = true;
  }
  ReleaseSRWLockExclusive(&g_environment_lock);
  return true;
}

void TickReplication(bool ready) noexcept {
  if (!g_enabled.load()) return;
  DWORD expected = 0;
  g_owner.compare_exchange_strong(expected, GetCurrentThreadId());
  if (!owner_thread()) { fail(); return; }
  g_in_world = ready;
  TickHorn(ready);
  if (!ready) { g_local_vehicle = 0; g_local_physics = 0; g_native_wheels = {}; }
}

void DisableReplication() noexcept { g_enabled = false; g_in_world = false; DisableHorn(); }
ReplicationStats GetReplicationStats() noexcept {
  return {g_captured.load(), g_applied.load(), g_environment_ticks.load(), g_failures.load(),
          g_steering.load(), g_headlights.load(), g_deformation.load(), g_day_hours.load(), g_weather_phase.load(),
          g_wheel_steps.load(), g_local_wheel_speed.load(), g_remote_wheel_speed.load(), g_remote_wheel_phase.load()};
}
} // namespace ht2mp::bridge
