#pragma once

#include "ht2mp/ipc/protocol.hpp"

#include <array>
#include <cstdint>

namespace ht2mp::bridge {

struct TelemetryInitialization final {
  ht2mp::ipc::BridgeMode mode{ht2mp::ipc::BridgeMode::safe};
  ht2mp::ipc::BridgeStatusCode status{
      ht2mp::ipc::BridgeStatusCode::symbol_resolution_failed};
  std::uint32_t capabilities{
      static_cast<std::uint32_t>(ht2mp::ipc::Capability::observer_mode)};
  std::array<char, 160> detail{};
};

struct TelemetryHookStats final {
  std::uint32_t invocations{};
  std::uint32_t polls{};
  std::uint32_t valid_samples{};
  std::uint32_t thread_id{};
  std::uint32_t thread_mismatches{};
  // QPC microseconds of the most recent hooked tick and the largest gap
  // between two consecutive ticks since ConsumeMaxTickGapUs() last reset it.
  std::uint64_t last_tick_us{};
  std::uint64_t max_tick_gap_us{};
};

// Performs complete on-disk profile verification and live target validation
// before MinHook is allowed to patch the verified tick entry point.
[[nodiscard]] TelemetryInitialization InitializeGogTelemetry(
    const std::array<char, ht2mp::ipc::kProfileIdCapacity>& profile_id,
    bool enable_actor_diagnostics,
    bool enable_remote_actors,
    bool enable_auto_enter,
    bool enable_online_world,
    std::uint32_t selected_vehicle,
    std::uint32_t selected_paint) noexcept;

// The hook's producer never waits: it drops a sample if this one-slot mailbox
// is momentarily owned by the pipe consumer. New samples replace older ones.
[[nodiscard]] bool ConsumeLatestLocalSample(
    ht2mp::ipc::PlayerSampleV1& sample) noexcept;

[[nodiscard]] bool SetTelemetryEnabled(bool enabled) noexcept;
[[nodiscard]] bool TelemetryHookReady() noexcept;
[[nodiscard]] TelemetryHookStats GetTelemetryHookStats() noexcept;
// Returns the largest inter-tick gap observed since the previous call and
// starts a new measurement window. Worker-thread use only.
[[nodiscard]] std::uint64_t ConsumeMaxTickGapUs() noexcept;
// QueryPerformanceCounter in microseconds (0 before initialization). This is
// the clock every bridge timestamp uses; std::chrono::steady_clock on MSVC is
// the same counter, so sidecar stamps share the domain on one machine.
[[nodiscard]] std::uint64_t MonotonicMicroseconds() noexcept;
void DisableTelemetryNoWait() noexcept;

} // namespace ht2mp::bridge
