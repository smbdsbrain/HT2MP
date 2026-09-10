#include "horn.hpp"
#include "horn_playback.hpp"
#include "remote_actor_backend.hpp"
#include "telemetry.hpp"
#include "ht2mp/game/pattern.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace ht2mp::bridge {
namespace {
using InputQuery = std::uint32_t(__thiscall*)(void*, int);
using GetSound = void*(__thiscall*)(void*);
using SpatialSound = void(__cdecl*)(void*, float, float, const float*);
using SoundTick = void(__cdecl*)();
using StopChannel = void(__cdecl*)(int);
InputQuery g_input{};
GetSound g_sound{};
SpatialSound g_spatial{};
SoundTick g_tick{};
StopChannel g_stop{};
std::uintptr_t g_base{};
std::uint32_t g_local_vehicle{};
ht2mp::protocol::HornState g_local{};
std::atomic<bool> g_enabled{}, g_world{};
std::atomic<DWORD> g_owner{};
std::atomic<std::uint32_t> g_presses{}, g_local_active{}, g_voices{}, g_starts{}, g_stops{}, g_frames{}, g_failures{};
constexpr std::array<std::uint32_t, 3> kSoundCells{0x2ef264, 0x2ef268, 0x2ef270};

struct RemoteHorn {
  std::uint32_t vehicle{};
  HornPlayback playback{};
  std::array<float, 3> position{};
  // SoundRef is 32 bytes. Stable bridge-owned storage is registered only in
  // the native 32-channel active-voice table. Clip/name pointers are borrowed
  // from the native template; its channel/playback state is never copied.
  std::array<std::uint32_t, 8> sound{};
  bool configured{};
  std::uint8_t tone{};
};
std::array<RemoteHorn, 7> g_remote{};

bool game_thread() noexcept { return g_owner.load() != 0 && g_owner.load() == GetCurrentThreadId(); }
void fail() noexcept { ++g_failures; g_enabled = false; RequestRemoteActorSafeMode(); }

bool stop(RemoteHorn& horn) noexcept {
  __try {
    const auto channel = static_cast<std::int32_t>(horn.sound[6]);
    if (horn.configured && channel >= 0 && channel < 32) {
      auto* cell = reinterpret_cast<std::uint32_t*>(g_base + 0x323aa8 + channel * 4);
      if (*cell == reinterpret_cast<std::uintptr_t>(horn.sound.data())) {
        // Native stop(-1) stops ALL channels. Only an identity-checked owned
        // channel may reach this call; mirror SoundTick's pool cleanup.
        if (*reinterpret_cast<const int*>(g_base + 0x323b28) != 0) g_stop(channel);
        *cell = 0;
        ++g_stops;
      }
    }
    horn.sound[5] = 0;
    horn.sound[6] = UINT32_MAX;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool update_input(std::uint32_t down) noexcept {
  __try {
    if (g_local_vehicle == 0) return true;
    const auto viewer = *reinterpret_cast<const std::uint32_t*>(g_base + 0x2d2078);
    if (!viewer || *reinterpret_cast<const std::uint32_t*>(viewer + 0x268) != g_local_vehicle) {
      g_local.active = false;
      g_local_active = 0;
      return true;
    }
    const auto physics = *reinterpret_cast<const std::uint32_t*>(g_local_vehicle + 0x5460);
    if (!physics || *reinterpret_cast<const std::uint32_t*>(physics + 0x29d4) != g_local_vehicle + 0x10) return false;
    // Same horn availability condition as Viewer::input at 0x53a492.
    bool active = down != 0 && *reinterpret_cast<const int*>(physics + 0x3344) == 0;
    const auto selected = reinterpret_cast<std::uintptr_t>(g_sound(reinterpret_cast<void*>(g_local_vehicle)));
    std::uint8_t tone = 0;
    bool found = false;
    for (std::uint8_t i = 0; i < kSoundCells.size(); ++i) {
      if (selected && selected == *reinterpret_cast<const std::uint32_t*>(g_base + kSoundCells[i])) {
        tone = i;
        found = true;
        break;
      }
    }
    active = active && found;
    if (active && !g_local.active) { ++g_local.press_sequence; ++g_presses; }
    g_local.active = active;
    g_local.tone = tone;
    g_local_active = active ? 1U : 0U;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::uint32_t __fastcall InputDetour(void* self, void*, int key) {
  const auto value = g_input(self, key);
  if (key == 0x15 && reinterpret_cast<std::uintptr_t>(self) == g_base + 0x2d1db8 &&
      g_enabled.load() && g_world.load() && game_thread() && !update_input(value)) fail();
  return value;
}

bool render_sounds(std::uint64_t now) noexcept {
  __try {
    const auto viewer = *reinterpret_cast<const std::uint32_t*>(g_base + 0x2d2078);
    const bool ready = g_enabled.load() && g_world.load() && viewer && g_local_vehicle &&
        *reinterpret_cast<const std::uint32_t*>(viewer + 0x268) == g_local_vehicle;
    std::uint32_t playing = 0;
    for (auto& horn : g_remote) {
      if (!ready || !horn.vehicle || !horn.playback.Playing(now)) {
        if (!stop(horn)) return false;
        continue;
      }
      const auto tone = horn.playback.tone();
      if (!horn.configured || horn.tone != tone) {
        if (!stop(horn)) return false;
        const auto source = *reinterpret_cast<const std::uint32_t*>(g_base + kSoundCells[tone]);
        if (!source) continue; // native sound resource not available yet
        const auto* fields = reinterpret_cast<const std::uint32_t*>(source);
        horn.sound = {fields[0], fields[1], fields[2], 0U, 0U, 0U, UINT32_MAX, fields[7]};
        horn.tone = tone;
        horn.configured = true;
      }
      // Viewer+0x5c0 is the world-to-listener matrix used by its own horn.
      const auto* matrix = reinterpret_cast<const float*>(viewer + 0x5c0);
      std::array<float, 3> relative{};
      for (std::size_t axis = 0; axis < 3; ++axis) {
        relative[axis] = matrix[axis] * horn.position[0] + matrix[3 + axis] * horn.position[1] +
                         matrix[6 + axis] * horn.position[2] + matrix[9 + axis];
        if (!std::isfinite(relative[axis])) return false;
      }
      // Native pan divides by distance; at the same position use a tiny,
      // centred forward offset. Native 15 m attenuation handles distance.
      if (std::abs(relative[0]) + std::abs(relative[1]) + std::abs(relative[2]) < 0.001F)
        relative[1] = 0.001F;
      const bool was_playing = horn.sound[5] != 0;
      g_spatial(horn.sound.data(), 15.0F, 1.0F, relative.data());
      if (horn.sound[5] != 0) {
        ++playing;
        if (!was_playing) ++g_starts;
      }
    }
    g_voices = playing;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __cdecl SoundTickDetour() {
  // Renew continuous voices before native expiry, including when offscreen.
  // Static voice storage remains valid even if shutdown skips a final tick.
  if (game_thread()) {
    ++g_frames;
    if (!render_sounds(MonotonicMicroseconds())) fail();
  }
  g_tick();
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

bool InitializeSteamHorn(const ht2mp::game::ProfileVerification& verified, std::uintptr_t base, std::string& error) {
  if (!verified.accepted() || verified.profile->id != "steam-8138acee" || base != verified.profile->pe.image_base) return false;
  struct Target { const char* name; std::uint32_t rva; };
  constexpr std::array<Target, 5> targets{{{"input_binding_query", 0x130070}, {"vehicle_horn_sound", 0x143bb0},
      {"sound_spatial_play", 0xe5140}, {"sound_frame_update", 0x1fd160}, {"sound_stop_channel", 0x204fe0}}};
  std::array<std::uintptr_t, 5> addresses{};
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const auto* symbol = verified.FindSymbol(targets[i].name);
    const auto descriptor = std::find_if(verified.profile->symbols.begin(), verified.profile->symbols.end(),
        [&](const auto& value) { return value.name == targets[i].name; });
    ht2mp::game::MaskedPattern pattern;
    if (!symbol || !symbol->accepted || symbol->match_count != 1 || symbol->result_rva != targets[i].rva ||
        descriptor == verified.profile->symbols.end() ||
        !ht2mp::game::ParseMaskedPattern(descriptor->ida_pattern, pattern, error) || pattern.empty() ||
        !live_match(base + symbol->result_rva, pattern)) {
      error = std::string("horn target validation failed: ") + targets[i].name;
      return false;
    }
    addresses[i] = base + symbol->result_rva;
  }
  g_base = base;
  g_sound = reinterpret_cast<GetSound>(addresses[1]);
  g_spatial = reinterpret_cast<SpatialSound>(addresses[2]);
  g_stop = reinterpret_cast<StopChannel>(addresses[4]);
  constexpr std::array<std::size_t, 2> hooked{0, 3};
  const std::array<LPVOID, 2> detours{reinterpret_cast<LPVOID>(&InputDetour), reinterpret_cast<LPVOID>(&SoundTickDetour)};
  const std::array<LPVOID*, 2> originals{reinterpret_cast<LPVOID*>(&g_input), reinterpret_cast<LPVOID*>(&g_tick)};
  std::size_t created = 0;
  for (; created < hooked.size(); ++created)
    if (MH_CreateHook(reinterpret_cast<LPVOID>(addresses[hooked[created]]), detours[created], originals[created]) != MH_OK) break;
  if (created == hooked.size()) {
    for (std::size_t i = 0; i < hooked.size(); ++i)
      if (MH_EnableHook(reinterpret_cast<LPVOID>(addresses[hooked[i]])) != MH_OK) goto rollback;
    g_enabled = true;
    return true;
  }
rollback:
  for (std::size_t i = 0; i < created; ++i) {
    MH_DisableHook(reinterpret_cast<LPVOID>(addresses[hooked[i]]));
    MH_RemoveHook(reinterpret_cast<LPVOID>(addresses[hooked[i]]));
  }
  error = "could not install exact-Steam horn hooks";
  return false;
}

void TickHorn(bool ready) noexcept {
  if (!g_enabled.load()) return;
  DWORD expected = 0;
  g_owner.compare_exchange_strong(expected, GetCurrentThreadId());
  if (!game_thread()) { fail(); return; }
  g_world = ready;
  if (!ready) { g_local_vehicle = 0; g_local.active = false; g_local_active = 0; }
}
void DisableHorn() noexcept { g_enabled = false; g_world = false; }

bool CaptureHorn(std::uint32_t vehicle, ht2mp::protocol::HornState& state) noexcept {
  __try {
    if (!g_enabled.load() || !game_thread()) { state = {}; return true; }
    if (g_local_vehicle != vehicle) { g_local.active = false; g_local_vehicle = vehicle; }
    if (!update_input(g_input(reinterpret_cast<void*>(g_base + 0x2d1db8), 0x15))) return false;
    state = g_local;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void UpdateRemoteHorn(std::uint32_t vehicle, const ht2mp::protocol::HornState& state,
                      std::uint64_t received_us, const std::array<float, 3>& position) noexcept {
  if (!g_enabled.load() || !game_thread() || !vehicle) return;
  auto found = std::find_if(g_remote.begin(), g_remote.end(), [vehicle](const auto& h) { return h.vehicle == vehicle; });
  if (found == g_remote.end()) found = std::find_if(g_remote.begin(), g_remote.end(), [](const auto& h) { return h.vehicle == 0; });
  if (found == g_remote.end()) { fail(); return; }
  found->vehicle = vehicle;
  found->position = position;
  found->playback.Observe(state, received_us, MonotonicMicroseconds());
}
void ReleaseRemoteHorn(std::uint32_t vehicle) noexcept {
  if (!game_thread() || !vehicle) return;
  for (auto& horn : g_remote) if (horn.vehicle == vehicle) {
    if (!stop(horn)) { fail(); return; }
    horn = {};
  }
}
HornStats GetHornStats() noexcept {
  return {g_presses.load(), g_local_active.load(), g_voices.load(), g_starts.load(), g_stops.load(), g_frames.load(), g_failures.load()};
}
} // namespace ht2mp::bridge
