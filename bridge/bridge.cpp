#include "actor_diagnostics.hpp"
#include "auto_enter.hpp"
#include "online_world.hpp"
#include "online_world_policy.hpp"
#include "telemetry.hpp"
#include "replication.hpp"
#include "horn.hpp"
#include "runtime_validation.hpp"
#include "remote_actor_backend.hpp"

#include "ht2mp/ipc/bootstrap.hpp"
#include "ht2mp/ipc/codec.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <span>
#include <string_view>

namespace {

volatile LONG g_initialization_state{}; // 0=new, 1=initializing, 2=ready
volatile LONG g_mode{static_cast<LONG>(ht2mp::ipc::BridgeMode::safe)};
volatile LONG g_stop_requested{};
PVOID volatile g_pipe{};
ht2mp::ipc::BootstrapV1 g_bootstrap{};
ht2mp::bridge::TelemetryInitialization g_telemetry_initialization{};

HANDLE pipe_handle() noexcept {
  return static_cast<HANDLE>(
      InterlockedCompareExchangePointer(&g_pipe, nullptr, nullptr));
}

void close_pipe() noexcept {
  const auto pipe =
      static_cast<HANDLE>(InterlockedExchangePointer(&g_pipe, nullptr));
  if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}

bool running_inside_king() noexcept {
  std::array<wchar_t, 32768> path{};
  const auto length = GetModuleFileNameW(nullptr, path.data(),
                                         static_cast<DWORD>(path.size()));
  if (length == 0U || length >= path.size()) return false;
  const wchar_t* filename = path.data();
  for (DWORD i = 0; i < length; ++i) {
    if (path[i] == L'\\' || path[i] == L'/') filename = path.data() + i + 1U;
  }
  return _wcsicmp(filename, L"king.exe") == 0;
}

bool write_all(HANDLE pipe, const std::span<const std::byte> bytes) noexcept {
  std::size_t cursor{};
  while (cursor < bytes.size()) {
    DWORD written{};
    const auto amount = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - cursor, MAXDWORD));
    if (!WriteFile(pipe, bytes.data() + cursor, amount, &written, nullptr) ||
        written == 0U) {
      return false;
    }
    cursor += written;
  }
  return true;
}

bool read_all(HANDLE pipe, const std::span<std::byte> bytes) noexcept {
  std::size_t cursor{};
  while (cursor < bytes.size()) {
    DWORD read{};
    const auto amount = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - cursor, MAXDWORD));
    if (!ReadFile(pipe, bytes.data() + cursor, amount, &read, nullptr) ||
        read == 0U) {
      return false;
    }
    cursor += read;
  }
  return true;
}

bool read_frame(HANDLE pipe,
                std::array<std::byte, ht2mp::ipc::kMaximumFrameSize>& frame,
                ht2mp::ipc::FrameHeader& header,
                std::span<const std::byte>& payload) noexcept {
  payload = {};
  auto header_bytes = std::span(frame).first(ht2mp::ipc::kFrameHeaderSize);
  if (!read_all(pipe, header_bytes)) return false;
  ht2mp::ipc::PayloadReader reader(header_bytes);
  std::uint32_t magic{};
  std::uint16_t version{};
  std::uint16_t raw_type{};
  if (!reader.get_u32(magic) || !reader.get_u16(version) ||
      !reader.get_u16(raw_type) || !reader.get_u32(header.payload_size) ||
      !reader.get_u32(header.sequence) || magic != ht2mp::ipc::kFrameMagic ||
      version != ht2mp::ipc::kProtocolVersion ||
      raw_type < static_cast<std::uint16_t>(
                     ht2mp::ipc::MessageType::bridge_hello) ||
      raw_type > static_cast<std::uint16_t>(
                     ht2mp::ipc::MessageType::environment) ||
      header.payload_size > ht2mp::ipc::kMaximumPayloadSize) {
    return false;
  }
  header.type = static_cast<ht2mp::ipc::MessageType>(raw_type);
  if (!read_all(pipe, std::span(frame).subspan(
                          ht2mp::ipc::kFrameHeaderSize,
                          header.payload_size))) {
    return false;
  }
  const auto frame_size = ht2mp::ipc::kFrameHeaderSize + header.payload_size;
  ht2mp::ipc::FrameHeader validated{};
  return ht2mp::ipc::decode_frame(std::span(frame).first(frame_size), validated,
                                  payload) == ht2mp::ipc::CodecError::none;
}

template <typename Encoder, typename Message>
bool send_message(HANDLE pipe, Encoder encoder, const Message& message) noexcept {
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> frame{};
  std::size_t frame_size{};
  if (encoder(message, frame, frame_size) != ht2mp::ipc::CodecError::none) {
    return false;
  }
  return write_all(pipe, std::span(frame).first(frame_size));
}

bool send_status(HANDLE pipe, const ht2mp::ipc::BridgeStatusCode code,
                 const ht2mp::ipc::BridgeMode mode,
                 const std::string_view detail) noexcept {
  ht2mp::ipc::BridgeStatusV1 status;
  status.code = code;
  status.mode = mode;
  const auto length = std::min(detail.size(), status.detail.size() - 1U);
  if (length != 0U) std::memcpy(status.detail.data(), detail.data(), length);
  status.detail[length] = '\0';
  return send_message(pipe, ht2mp::ipc::encode_bridge_status, status);
}

bool send_hook_activity(
    HANDLE pipe, const ht2mp::bridge::TelemetryHookStats& previous,
    const ht2mp::bridge::TelemetryHookStats& current,
    const ULONGLONG elapsed_ms, const std::uint32_t min_calls_per_second,
    const std::uint32_t max_calls_per_second,
    const std::uint64_t max_tick_gap_us) noexcept {
  const auto calls = current.invocations - previous.invocations;
  const auto rate_tenths = elapsed_ms == 0U
                               ? 0ULL
                               : static_cast<unsigned long long>(calls) *
                                     10'000ULL / elapsed_ms;
  std::array<char, 160> detail{};
  (void)_snprintf_s(
      detail.data(), detail.size(), _TRUNCATE,
      "EXPERIMENTAL exact-profile hook: calls=%lu rate=%llu.%lluHz "
      "min1s=%lu max1s=%lu maxgap=%llums polls=%lu samples=%lu thread=%lu "
      "mismatch=%lu",
      static_cast<unsigned long>(current.invocations), rate_tenths / 10ULL,
      rate_tenths % 10ULL,
      static_cast<unsigned long>(min_calls_per_second),
      static_cast<unsigned long>(max_calls_per_second),
      static_cast<unsigned long long>(max_tick_gap_us / 1000ULL),
      static_cast<unsigned long>(current.polls),
      static_cast<unsigned long>(current.valid_samples),
      static_cast<unsigned long>(current.thread_id),
      static_cast<unsigned long>(current.thread_mismatches));
  return send_status(pipe, ht2mp::ipc::BridgeStatusCode::ok,
                     ht2mp::ipc::BridgeMode::observer, detail.data());
}

bool send_actor_diagnostic_activity(
    HANDLE pipe, const ht2mp::bridge::ActorDiagnosticStats& stats) noexcept {
  std::array<char, 160> collision_detail{};
  (void)_snprintf_s(
      collision_detail.data(), collision_detail.size(), _TRUNCATE,
      "ACTOR-DIAG pass-through calls/thread/mismatch: resolver=%lu/%lu/%lu "
      "pair=%lu/%lu/%lu hit=%lu/%lu/%lu",
      static_cast<unsigned long>(stats.moving_item_resolver.invocations),
      static_cast<unsigned long>(stats.moving_item_resolver.thread_id),
      static_cast<unsigned long>(stats.moving_item_resolver.thread_mismatches),
      static_cast<unsigned long>(stats.pair_collision.invocations),
      static_cast<unsigned long>(stats.pair_collision.thread_id),
      static_cast<unsigned long>(stats.pair_collision.thread_mismatches),
      static_cast<unsigned long>(stats.hit_player.invocations),
      static_cast<unsigned long>(stats.hit_player.thread_id),
      static_cast<unsigned long>(stats.hit_player.thread_mismatches));
  if (!send_status(pipe, ht2mp::ipc::BridgeStatusCode::ok,
                   ht2mp::ipc::BridgeMode::observer,
                   collision_detail.data())) {
    return false;
  }

  std::array<char, 160> lifecycle_detail{};
  (void)_snprintf_s(
      lifecycle_detail.data(), lifecycle_detail.size(), _TRUNCATE,
      "ACTOR-DIAG lifecycle calls/thread/mismatch: reset=%lu/%lu/%lu "
      "save=%lu/%lu/%lu; no behavior changes",
      static_cast<unsigned long>(stats.pre_registry_reset.invocations),
      static_cast<unsigned long>(stats.pre_registry_reset.thread_id),
      static_cast<unsigned long>(stats.pre_registry_reset.thread_mismatches),
      static_cast<unsigned long>(stats.pre_save.invocations),
      static_cast<unsigned long>(stats.pre_save.thread_id),
      static_cast<unsigned long>(stats.pre_save.thread_mismatches));
  return send_status(pipe, ht2mp::ipc::BridgeStatusCode::ok,
                     ht2mp::ipc::BridgeMode::observer,
                     lifecycle_detail.data());
}

bool send_remote_actor_activity(
    HANDLE pipe, const ht2mp::bridge::RemoteActorBackendStats& stats) noexcept {
  std::array<char, 160> detail{};
  (void)_snprintf_s(
      detail.data(), detail.size(), _TRUNCATE,
      "REMOTE-ACTOR s/a/v=%lu/%lu/%lu life=%lu/%lu samples=%lu cap=%lu/%lu "
      "sup=%lu/%lu/%lu/%lu fix=%lu model=%lu/%lu vi=%08lx setpos=%lu/%lu "
      "mode=%lu/%lu fail=%lu tm=%lu",
      static_cast<unsigned long>(stats.active_slots),
      static_cast<unsigned long>(stats.game_actors),
      static_cast<unsigned long>(stats.captured_vehicles),
      static_cast<unsigned long>(stats.spawns),
      static_cast<unsigned long>(stats.despawns),
      static_cast<unsigned long>(stats.applied_samples),
      static_cast<unsigned long>(stats.constructor_vehicle_candidates),
      static_cast<unsigned long>(stats.resolver_vehicle_candidates),
      static_cast<unsigned long>(stats.suppressed_moves),
      static_cast<unsigned long>(stats.suppressed_physics_updates),
      static_cast<unsigned long>(stats.suppressed_collisions),
      static_cast<unsigned long>(stats.suppressed_hits),
      static_cast<unsigned long>(stats.corrected_render_updates),
      static_cast<unsigned long>(stats.requested_vehicle_model),
      static_cast<unsigned long>(stats.applied_vehicle_model),
      static_cast<unsigned long>(stats.vehicle_instance_address),
      static_cast<unsigned long>(stats.set_position_calls),
      static_cast<unsigned long>(stats.suppressed_set_position),
      static_cast<unsigned long>(stats.motion_mode_writes),
      static_cast<unsigned long>(stats.motion_mode_failures),
      static_cast<unsigned long>(stats.validation_failures),
      static_cast<unsigned long>(stats.thread_mismatches));
  const auto code = stats.safe_mode_requested
                        ? ht2mp::ipc::BridgeStatusCode::telemetry_runtime_error
                        : ht2mp::ipc::BridgeStatusCode::ok;
  const auto mode = stats.safe_mode_requested ? ht2mp::ipc::BridgeMode::safe
                                               : ht2mp::ipc::BridgeMode::active;
  if (!send_status(pipe, code, mode, detail.data())) return false;

  std::array<char, 160> frame_detail{};
  (void)_snprintf_s(
      frame_detail.data(), frame_detail.size(), _TRUNCATE,
      "REMOTE-FRAME writes=%lu maxgap_us=%lu mode=%lu/%lu fail=%lu tm=%lu",
      static_cast<unsigned long>(stats.corrected_render_updates),
      static_cast<unsigned long>(stats.frame_max_gap_us),
      static_cast<unsigned long>(stats.motion_mode_writes),
      static_cast<unsigned long>(stats.motion_mode_failures),
      static_cast<unsigned long>(stats.validation_failures),
      static_cast<unsigned long>(stats.thread_mismatches));
  if (!send_status(pipe, code, mode, frame_detail.data())) return false;
  (void)_snprintf_s(
      frame_detail.data(), frame_detail.size(), _TRUNCATE,
      "REMOTE-SCENE reads=%lu native_error=%.4fm/%.6f",
      static_cast<unsigned long>(stats.scene_reads),
      static_cast<double>(stats.scene_position_error),
      static_cast<double>(stats.scene_orientation_error));
  if (!send_status(pipe, code, mode, frame_detail.data())) return false;

  (void)_snprintf_s(
      frame_detail.data(), frame_detail.size(), _TRUNCATE,
      "REMOTE-SHADOW updates=%lu native_error=%.4fm/%.6f",
      static_cast<unsigned long>(stats.shadow_updates),
      static_cast<double>(stats.shadow_position_error),
      static_cast<double>(stats.shadow_orientation_error));
  if (!send_status(pipe, code, mode, frame_detail.data())) return false;

  std::array<char, 160> vehicle_detail{};
  (void)_snprintf_s(
      vehicle_detail.data(), vehicle_detail.size(), _TRUNCATE,
      "REMOTE-VEHICLE ctor local=%08lx/%08lx/%08lx/%08lx "
      "remote=%08lx/%08lx/%08lx/%08lx",
      static_cast<unsigned long>(stats.local_vehicle_constructor_args[0]),
      static_cast<unsigned long>(stats.local_vehicle_constructor_args[1]),
      static_cast<unsigned long>(stats.local_vehicle_constructor_args[2]),
      static_cast<unsigned long>(stats.local_vehicle_constructor_args[3]),
      static_cast<unsigned long>(stats.remote_vehicle_constructor_args[0]),
      static_cast<unsigned long>(stats.remote_vehicle_constructor_args[1]),
      static_cast<unsigned long>(stats.remote_vehicle_constructor_args[2]),
      static_cast<unsigned long>(stats.remote_vehicle_constructor_args[3]));
  if (!send_status(pipe, code, mode, vehicle_detail.data())) return false;

  std::array<char, 160> pose_detail{};
  (void)_snprintf_s(
      pose_detail.data(), pose_detail.size(), _TRUNCATE,
      "REMOTE-POSE seq=%lu changed=%lu writes=%lu mask=%02lx "
      "cmd=(%.2f %.2f %.2f)",
      static_cast<unsigned long>(stats.pose_sequence),
      static_cast<unsigned long>(stats.changed_pose_commands),
      static_cast<unsigned long>(stats.transform_writes),
      static_cast<unsigned long>(stats.pose_valid_mask),
      static_cast<double>(stats.commanded_position[0]),
      static_cast<double>(stats.commanded_position[1]),
      static_cast<double>(stats.commanded_position[2]));
  if (!send_status(pipe, code, mode, pose_detail.data())) return false;

  std::array<char, 160> pre_pose_detail{};
  (void)_snprintf_s(
      pre_pose_detail.data(), pre_pose_detail.size(), _TRUNCATE,
      "REMOTE-PRE render=(%.2f %.2f %.2f) physics=(%.2f %.2f %.2f) "
      "rot=%.4g/%.4g",
      static_cast<double>(stats.pre_render_position[0]),
      static_cast<double>(stats.pre_render_position[1]),
      static_cast<double>(stats.pre_render_position[2]),
      static_cast<double>(stats.pre_physics_position[0]),
      static_cast<double>(stats.pre_physics_position[1]),
      static_cast<double>(stats.pre_physics_position[2]),
      static_cast<double>(stats.pre_render_orientation_error),
      static_cast<double>(stats.pre_physics_orientation_error));
  if (!send_status(pipe, code, mode, pre_pose_detail.data())) return false;

  std::array<char, 160> post_pose_detail{};
  (void)_snprintf_s(
      post_pose_detail.data(), post_pose_detail.size(), _TRUNCATE,
      "REMOTE-POST render=(%.2f %.2f %.2f) physics=(%.2f %.2f %.2f) "
      "rot=%.4g/%.4g",
      static_cast<double>(stats.post_render_position[0]),
      static_cast<double>(stats.post_render_position[1]),
      static_cast<double>(stats.post_render_position[2]),
      static_cast<double>(stats.post_physics_position[0]),
      static_cast<double>(stats.post_physics_position[1]),
      static_cast<double>(stats.post_physics_position[2]),
      static_cast<double>(stats.post_render_orientation_error),
      static_cast<double>(stats.post_physics_orientation_error));
  if (!send_status(pipe, code, mode, post_pose_detail.data())) return false;

  // Receive-to-consume latency percentiles from the 25 ms histogram.
  const auto replication = ht2mp::bridge::GetReplicationStats();
  std::array<char, 160> replication_detail{};
  (void)_snprintf_s(replication_detail.data(), replication_detail.size(), _TRUNCATE,
      "REPLICATION captured=%u applied=%u world=%u fail=%u steer=%d lights=%u dent=%.3f hour=%.4f weather=%.3f",
      replication.captured, replication.applied, replication.environment_ticks, replication.failures,
      replication.steering, replication.headlights, static_cast<double>(replication.deformation),
      static_cast<double>(replication.day_hours), static_cast<double>(replication.weather_phase));
  if (!send_status(pipe, code, mode, replication_detail.data())) return false;
  (void)_snprintf_s(replication_detail.data(), replication_detail.size(), _TRUNCATE,
      "WHEELS steps=%u local_rad_s=%.3f remote_rad_s=%.3f phase=%.3f",
      replication.wheel_steps, static_cast<double>(replication.local_wheel_speed),
      static_cast<double>(replication.remote_wheel_speed), static_cast<double>(replication.remote_wheel_phase));
  if (!send_status(pipe, code, mode, replication_detail.data())) return false;
  const auto horn = ht2mp::bridge::GetHornStats();
  (void)_snprintf_s(replication_detail.data(), replication_detail.size(), _TRUNCATE,
      "HORNS presses=%u local=%u voices=%u starts=%u stops=%u frames=%u fail=%u",
      horn.presses, horn.local_active, horn.voices, horn.starts, horn.stops, horn.frames, horn.failures);
  if (!send_status(pipe, code, mode, replication_detail.data())) return false;
  const auto& timeline = stats.timeline;
  std::uint64_t total{};
  for (const auto count : timeline.latency_histogram) total += count;
  const auto percentile = [&](const double fraction) {
    if (total == 0U) return 0ULL;
    const auto threshold = static_cast<std::uint64_t>(
        static_cast<double>(total) * fraction);
    std::uint64_t accumulated{};
    for (std::size_t bucket = 0U; bucket < timeline.latency_histogram.size();
         ++bucket) {
      accumulated += timeline.latency_histogram[bucket];
      if (accumulated >= threshold) {
        return static_cast<unsigned long long>((bucket + 1U) *
                                               ht2mp::bridge::kPoseLatencyBucketUs /
                                               1000U);
      }
    }
    return static_cast<unsigned long long>(
        timeline.latency_histogram.size() * ht2mp::bridge::kPoseLatencyBucketUs /
        1000U);
  };
  std::array<char, 160> timeline_detail{};
  (void)_snprintf_s(
      timeline_detail.data(), timeline_detail.size(), _TRUNCATE,
      "REMOTE-TIMELINE off=%lldms delay=%lums int=%lums jit=%lums "
      "st=%lu/%lu/%lu rs=%lu snap=%lu buf=%lu lat=%llu/%llu/%llums "
      "drop=%lu rej=%lu restamp=%lu",
      static_cast<long long>(timeline.clock_offset_us / 1000),
      static_cast<unsigned long>(timeline.delay_us / 1000U),
      static_cast<unsigned long>(timeline.interval_max_us / 1000U),
      static_cast<unsigned long>(timeline.jitter_max_us / 1000U),
      static_cast<unsigned long>(timeline.interpolated),
      static_cast<unsigned long>(timeline.extrapolated),
      static_cast<unsigned long>(timeline.held),
      static_cast<unsigned long>(timeline.resyncs),
      static_cast<unsigned long>(timeline.snaps),
      static_cast<unsigned long>(timeline.buffered), percentile(0.5),
      percentile(0.95),
      static_cast<unsigned long long>(timeline.latency_max_us / 1000U),
      static_cast<unsigned long>(stats.dropped_states),
      static_cast<unsigned long>(timeline.rejected),
      static_cast<unsigned long>(stats.receive_stamps_replaced));
  return send_status(pipe, code, mode, timeline_detail.data());
}

bool send_online_world_activity(
    HANDLE pipe, const ht2mp::bridge::OnlineWorldStats& stats) noexcept {
  std::array<char, 160> detail{};
  (void)_snprintf_s(
      detail.data(), detail.size(), _TRUNCATE,
      "ONLINE gen=%lu keep=%lu/%lu stock=%lu sup=%lu/%lu/%lu "
      "park=%lu/%lu/%lu bg=%u age=%llums fail=%lu tm=%lu",
      static_cast<unsigned long>(stats.generation),
      static_cast<unsigned long>(stats.local_actors),
      static_cast<unsigned long>(stats.remote_owned_actors),
      static_cast<unsigned long>(stats.stock_actors),
      static_cast<unsigned long>(stats.suppressed_regular),
      static_cast<unsigned long>(stats.suppressed_ghosts),
      static_cast<unsigned long>(stats.suppressed_dealers),
      static_cast<unsigned long>(stats.blocked_assortments),
      static_cast<unsigned long>(stats.blocked_orders),
      static_cast<unsigned long>(stats.blocked_hires),
      stats.background_tick_healthy ? 1U : 0U,
      static_cast<unsigned long long>(stats.last_tick_age_ms),
      static_cast<unsigned long>(stats.validation_failures),
      static_cast<unsigned long>(stats.thread_mismatches));
  const auto code = stats.safe_mode_requested
                        ? ht2mp::ipc::BridgeStatusCode::online_world_runtime_error
                        : ht2mp::ipc::BridgeStatusCode::ok;
  const auto mode = stats.safe_mode_requested
                        ? ht2mp::ipc::BridgeMode::safe
                        : ht2mp::ipc::BridgeMode::active;
  if (!send_status(pipe, code, mode, detail.data())) return false;

  std::array<char, 160> focus{};
  (void)_snprintf_s(
      focus.data(), focus.size(), _TRUNCATE,
      "ONLINE-FOCUS loss=%lu suppressed=%lu",
      static_cast<unsigned long>(stats.focus_loss_events),
      static_cast<unsigned long>(stats.focus_loss_suppressed));
  if (!send_status(pipe, code, mode, focus.data())) return false;

  std::array<char, 160> removed{};
  (void)_snprintf_s(
      removed.data(), removed.size(), _TRUNCATE,
      "ONLINE removed-by-type 0..9=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
      static_cast<unsigned long>(stats.removed_by_type[0]),
      static_cast<unsigned long>(stats.removed_by_type[1]),
      static_cast<unsigned long>(stats.removed_by_type[2]),
      static_cast<unsigned long>(stats.removed_by_type[3]),
      static_cast<unsigned long>(stats.removed_by_type[4]),
      static_cast<unsigned long>(stats.removed_by_type[5]),
      static_cast<unsigned long>(stats.removed_by_type[6]),
      static_cast<unsigned long>(stats.removed_by_type[7]),
      static_cast<unsigned long>(stats.removed_by_type[8]),
      static_cast<unsigned long>(stats.removed_by_type[9]));
  return send_status(pipe, code, mode, removed.data());
}

const char* auto_enter_state_name(
    const ht2mp::bridge::AutoEnterState state) noexcept {
  switch (state) {
  case ht2mp::bridge::AutoEnterState::disabled: return "disabled";
  case ht2mp::bridge::AutoEnterState::armed: return "armed";
  case ht2mp::bridge::AutoEnterState::dispatching: return "dispatching";
  case ht2mp::bridge::AutoEnterState::load_dispatched:
    return "load-dispatched";
  case ht2mp::bridge::AutoEnterState::world_ready: return "world-ready";
  case ht2mp::bridge::AutoEnterState::failed: return "failed";
  }
  return "invalid";
}

bool send_auto_enter_activity(
    HANDLE pipe, const ht2mp::bridge::AutoEnterStats& stats) noexcept {
  std::array<char, 160> detail{};
  (void)_snprintf_s(
      detail.data(), detail.size(), _TRUNCATE,
      "AUTO-ENTER state=%s hooks=%lu attempts=%lu thread=%lu mismatch=%lu failure=%lu",
      auto_enter_state_name(stats.state),
      static_cast<unsigned long>(stats.hook_invocations),
      static_cast<unsigned long>(stats.attempts),
      static_cast<unsigned long>(stats.thread_id),
      static_cast<unsigned long>(stats.thread_mismatches),
      static_cast<unsigned long>(stats.failure));
  return send_status(
      pipe,
      stats.state == ht2mp::bridge::AutoEnterState::failed
          ? ht2mp::ipc::BridgeStatusCode::telemetry_runtime_error
          : ht2mp::ipc::BridgeStatusCode::ok,
      static_cast<ht2mp::ipc::BridgeMode>(
          InterlockedCompareExchange(&g_mode, 0, 0)),
      detail.data());
}

std::string_view initial_status_detail() noexcept {
  const auto& value = g_telemetry_initialization.detail;
  const auto end = std::find(value.begin(), value.end(), '\0');
  return {value.data(), static_cast<std::size_t>(end - value.begin())};
}

bool send_handshake(HANDLE pipe) noexcept {
  ht2mp::ipc::BridgeHelloV1 hello;
  hello.run_id = g_bootstrap.run_id;
  hello.nonce = g_bootstrap.nonce;
  hello.profile_id = g_bootstrap.profile_id;
  if (!send_message(pipe, ht2mp::ipc::encode_bridge_hello, hello)) return false;

  ht2mp::ipc::BridgeReadyV1 ready;
  ready.mode = g_telemetry_initialization.mode;
  ready.capabilities = g_telemetry_initialization.capabilities;
  if (!send_message(pipe, ht2mp::ipc::encode_bridge_ready, ready)) return false;
  return send_status(pipe, g_telemetry_initialization.status,
                     g_telemetry_initialization.mode, initial_status_detail());
}

bool process_control(HANDLE pipe, const ht2mp::ipc::FrameHeader& header,
                     const std::span<const std::byte> payload) noexcept {
  const auto fail_remote_command = [&](const char* detail) {
    ht2mp::bridge::RequestRemoteActorSafeMode();
    ht2mp::bridge::RequestSteamOnlineWorldSafeMode();
    InterlockedExchange(&g_mode,
                        static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
    return send_status(pipe, ht2mp::ipc::BridgeStatusCode::queue_overflow,
                       ht2mp::ipc::BridgeMode::safe, detail);
  };
  if (header.type == ht2mp::ipc::MessageType::environment) {
    ht2mp::ipc::EnvironmentV1 command;
    if (ht2mp::ipc::decode_environment(payload, command) != ht2mp::ipc::CodecError::none ||
        !ht2mp::bridge::QueueEnvironment(command)) return fail_remote_command("Invalid server environment");
    return true;
  }
  if (header.type == ht2mp::ipc::MessageType::spawn_remote) {
    ht2mp::ipc::SpawnRemoteV1 command;
    if (ht2mp::ipc::decode_spawn_remote(payload, command) !=
            ht2mp::ipc::CodecError::none ||
        !ht2mp::bridge::QueueRemoteSpawn(command)) {
      return fail_remote_command(
          "Malformed/overflowing SpawnRemote; remote actors entered safe mode");
    }
    return true;
  }
  if (header.type == ht2mp::ipc::MessageType::remote_sample) {
    ht2mp::ipc::PlayerSampleV1 command;
    if (ht2mp::ipc::decode_player_sample(payload, command) !=
            ht2mp::ipc::CodecError::none ||
        !ht2mp::bridge::QueueRemoteSample(command)) {
      return fail_remote_command(
          "Malformed/overflowing RemoteSample; remote actors entered safe mode");
    }
    return true;
  }
  if (header.type == ht2mp::ipc::MessageType::despawn_remote) {
    ht2mp::ipc::DespawnRemoteV1 command;
    if (ht2mp::ipc::decode_despawn_remote(payload, command) !=
            ht2mp::ipc::CodecError::none ||
        !ht2mp::bridge::QueueRemoteDespawn(command)) {
      return fail_remote_command(
          "Malformed/overflowing DespawnRemote; remote actors entered safe mode");
    }
    return true;
  }
  if (header.type == ht2mp::ipc::MessageType::set_observer_mode) {
    ht2mp::ipc::SetObserverModeV1 command;
    if (ht2mp::ipc::decode_set_observer_mode(payload, command) !=
        ht2mp::ipc::CodecError::none) {
      ht2mp::bridge::DisableTelemetryNoWait();
      ht2mp::bridge::DisableAutoEnterNoWait();
      ht2mp::bridge::RequestRemoteActorSafeMode();
      ht2mp::bridge::RequestSteamOnlineWorldSafeMode();
      InterlockedExchange(&g_mode,
                          static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
      return send_status(pipe,
                         ht2mp::ipc::BridgeStatusCode::malformed_command,
                         ht2mp::ipc::BridgeMode::safe,
                         "Malformed SetObserverMode; entered safe mode");
    }
    if (!command.enabled) {
      (void)ht2mp::bridge::SetTelemetryEnabled(false);
      ht2mp::bridge::DisableAutoEnterNoWait();
      ht2mp::bridge::RequestRemoteActorSafeMode();
      ht2mp::bridge::RequestSteamOnlineWorldSafeMode();
      InterlockedExchange(&g_mode,
                          static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
      return send_status(pipe, ht2mp::ipc::BridgeStatusCode::ok,
                         ht2mp::ipc::BridgeMode::safe,
                         "Exact-profile read-only telemetry disabled");
    }
    if (!ht2mp::bridge::SetTelemetryEnabled(true)) {
      InterlockedExchange(&g_mode,
                          static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
      return send_status(
          pipe, ht2mp::ipc::BridgeStatusCode::hook_install_failed,
          ht2mp::ipc::BridgeMode::safe,
          "Telemetry cannot start because its verified hook is unavailable");
    }
    const auto mode = ht2mp::bridge::RemoteActorsReady()
                          ? ht2mp::ipc::BridgeMode::active
                          : ht2mp::ipc::BridgeMode::observer;
    InterlockedExchange(&g_mode, static_cast<LONG>(mode));
    return send_status(pipe, ht2mp::ipc::BridgeStatusCode::ok,
                       mode, mode == ht2mp::ipc::BridgeMode::active
                                 ? "Exact-profile telemetry and remote actors enabled"
                                 : "Exact-profile read-only telemetry enabled");
  }

  if (header.type == ht2mp::ipc::MessageType::enter_safe_mode) {
    ht2mp::ipc::EnterSafeModeV1 command;
    if (ht2mp::ipc::decode_enter_safe_mode(payload, command) !=
        ht2mp::ipc::CodecError::none) {
      ht2mp::bridge::DisableTelemetryNoWait();
      ht2mp::bridge::DisableAutoEnterNoWait();
      ht2mp::bridge::RequestRemoteActorSafeMode();
      ht2mp::bridge::RequestSteamOnlineWorldSafeMode();
      InterlockedExchange(&g_mode,
                          static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
      return send_status(pipe,
                         ht2mp::ipc::BridgeStatusCode::malformed_command,
                         ht2mp::ipc::BridgeMode::safe,
                         "Malformed EnterSafeMode command");
    }
    (void)command;
    ht2mp::bridge::DisableTelemetryNoWait();
    ht2mp::bridge::DisableAutoEnterNoWait();
    ht2mp::bridge::RequestRemoteActorSafeMode();
    ht2mp::bridge::RequestSteamOnlineWorldSafeMode();
    InterlockedExchange(&g_mode,
                        static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
    return send_status(pipe, ht2mp::ipc::BridgeStatusCode::ok,
                       ht2mp::ipc::BridgeMode::safe,
                       "Safe mode entered; telemetry stopped");
  }

  return send_status(
      pipe, ht2mp::ipc::BridgeStatusCode::unsupported_command,
      static_cast<ht2mp::ipc::BridgeMode>(
          InterlockedCompareExchange(&g_mode, 0, 0)),
      "Message is unsupported by the read-only telemetry bridge");
}

bool process_available_control(HANDLE pipe) noexcept {
  for (unsigned handled = 0U; handled < 16U; ++handled) {
    std::array<std::byte, ht2mp::ipc::kFrameHeaderSize> preview{};
    DWORD previewed{};
    DWORD available{};
    if (!PeekNamedPipe(pipe, preview.data(), static_cast<DWORD>(preview.size()),
                       &previewed, &available, nullptr)) {
      return false;
    }
    if (available < ht2mp::ipc::kFrameHeaderSize ||
        previewed < ht2mp::ipc::kFrameHeaderSize) {
      return true;
    }
    ht2mp::ipc::PayloadReader preview_reader(preview);
    std::uint32_t magic{};
    std::uint16_t version{};
    std::uint16_t raw_type{};
    std::uint32_t payload_size{};
    std::uint32_t sequence{};
    if (!preview_reader.get_u32(magic) || !preview_reader.get_u16(version) ||
        !preview_reader.get_u16(raw_type) ||
        !preview_reader.get_u32(payload_size) ||
        !preview_reader.get_u32(sequence) || magic != ht2mp::ipc::kFrameMagic ||
        version != ht2mp::ipc::kProtocolVersion ||
        payload_size > ht2mp::ipc::kMaximumPayloadSize) {
      return false;
    }
    (void)raw_type;
    (void)sequence;
    if (available < ht2mp::ipc::kFrameHeaderSize + payload_size) return true;
    std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> frame{};
    ht2mp::ipc::FrameHeader header{};
    std::span<const std::byte> payload{};
    if (!read_frame(pipe, frame, header, payload) ||
        !process_control(pipe, header, payload)) {
      return false;
    }
  }
  return true;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// One-millisecond worker pacing without touching the process-wide timer
// resolution (timeBeginPeriod would also change the game's own timing).
class WorkerPacer final {
 public:
  WorkerPacer() noexcept {
    timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (timer_ == nullptr) {
      timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
  }
  ~WorkerPacer() {
    if (timer_ != nullptr) CloseHandle(timer_);
  }
  WorkerPacer(const WorkerPacer&) = delete;
  WorkerPacer& operator=(const WorkerPacer&) = delete;

  void Wait(const DWORD milliseconds) noexcept {
    if (timer_ != nullptr) {
      LARGE_INTEGER due{};
      due.QuadPart = -static_cast<LONGLONG>(milliseconds) * 10'000LL;
      if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
        (void)WaitForSingleObject(timer_, milliseconds + 5U);
        return;
      }
    }
    Sleep(milliseconds);
  }

 private:
  HANDLE timer_{};
};

DWORD WINAPI pipe_worker(void*) noexcept {
  const auto pipe = pipe_handle();
  if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) return 1U;
  WorkerPacer pacer;
  if (!send_handshake(pipe)) {
    ht2mp::bridge::DisableTelemetryNoWait();
    ht2mp::bridge::DisableAutoEnterNoWait();
    ht2mp::bridge::RequestRemoteActorSafeMode();
    close_pipe();
    return 2U;
  }

  ULONGLONG next_sample_ms = GetTickCount64();
  ULONGLONG previous_worker_ms = next_sample_ms;
  ULONGLONG hook_status_window_ms = next_sample_ms;
  ULONGLONG next_hook_status_ms = next_sample_ms + 2'000U;
  ULONGLONG next_actor_status_ms = next_sample_ms + 2'500U;
  ULONGLONG next_online_status_ms = next_sample_ms + 2'500U;
  auto previous_auto_state = ht2mp::bridge::AutoEnterState::disabled;
  auto previous_hook_stats = ht2mp::bridge::GetTelemetryHookStats();
  // One-second cadence buckets inside the 5-second hook status window.
  ULONGLONG second_window_ms = next_sample_ms;
  std::uint32_t second_window_calls = previous_hook_stats.invocations;
  std::uint32_t min_calls_per_second = UINT32_MAX;
  std::uint32_t max_calls_per_second = 0U;
  ULONGLONG next_stall_status_ms = 0U;
  while (InterlockedCompareExchange(&g_stop_requested, 0, 0) == 0) {
    if (!process_available_control(pipe)) break;
    const auto now = GetTickCount64();
    const auto worker_gap_ms =
        now > previous_worker_ms ? now - previous_worker_ms : 0U;
    previous_worker_ms = now;
    {
      // The game thread already paces publication (45 ms of capture time);
      // forwarding immediately keeps transit jitter at the worker period.
      ht2mp::ipc::PlayerSampleV1 sample;
      if (ht2mp::bridge::ConsumeLatestLocalSample(sample) &&
          !send_message(pipe, ht2mp::ipc::encode_local_sample, sample)) {
        break;
      }
    }
    const auto current_mode = static_cast<ht2mp::ipc::BridgeMode>(
        InterlockedCompareExchange(&g_mode, 0, 0));
    const auto live_hook_stats = ht2mp::bridge::GetTelemetryHookStats();
    if (now - second_window_ms >= 1'000U) {
      const auto calls_this_second =
          live_hook_stats.invocations - second_window_calls;
      min_calls_per_second = std::min(min_calls_per_second, calls_this_second);
      max_calls_per_second = std::max(max_calls_per_second, calls_this_second);
      second_window_calls = live_hook_stats.invocations;
      second_window_ms = now;
    }
    if ((current_mode == ht2mp::ipc::BridgeMode::observer ||
         current_mode == ht2mp::ipc::BridgeMode::active) &&
        live_hook_stats.thread_mismatches != 0U) {
      ht2mp::bridge::DisableTelemetryNoWait();
      ht2mp::bridge::DisableAutoEnterNoWait();
      ht2mp::bridge::RequestRemoteActorSafeMode();
      ht2mp::bridge::RequestSteamOnlineWorldSafeMode();
      InterlockedExchange(&g_mode,
                          static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
      if (!send_status(
              pipe, ht2mp::ipc::BridgeStatusCode::telemetry_runtime_error,
              ht2mp::ipc::BridgeMode::safe,
              "Post-AI hook changed threads; telemetry entered safe mode")) {
        break;
      }
      continue;
    }
    if (current_mode == ht2mp::ipc::BridgeMode::active &&
        ht2mp::bridge::SteamOnlineWorldReady()) {
      if (ht2mp::bridge::ShouldRearmBackgroundTickWatchdog(worker_gap_ms)) {
        ht2mp::bridge::RearmSteamBackgroundTickWatchdog(now);
      } else if (ht2mp::bridge::SteamBackgroundTickWatchdogExpired(now)) {
        // A visible unfocused window that stops ticking is a cadence defect,
        // not a safety hazard: the remote actors merely freeze. Report it and
        // keep the session so a covered or deprioritised window recovers as
        // soon as the desktop schedules it again.
        if (now >= next_stall_status_ms) {
          (void)send_status(
              pipe, ht2mp::ipc::BridgeStatusCode::background_tick_stalled,
              current_mode,
              "Visible unfocused game paused its AI tick for more than 500ms; "
              "playback holds until it resumes");
          next_stall_status_ms = now + 5'000U;
        }
      }
      const auto online_stats = ht2mp::bridge::GetOnlineWorldStats();
      if (online_stats.safe_mode_requested) {
        (void)send_online_world_activity(pipe, online_stats);
        ht2mp::bridge::DisableTelemetryNoWait();
        ht2mp::bridge::DisableAutoEnterNoWait();
        ht2mp::bridge::RequestRemoteActorSafeMode();
        InterlockedExchange(&g_mode,
                            static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
        break;
      }
      if (now >= next_online_status_ms) {
        if (!send_online_world_activity(pipe, online_stats)) break;
        next_online_status_ms = now + 2'500U;
      }
    }
    if (ht2mp::bridge::TelemetryHookReady() &&
        (current_mode == ht2mp::ipc::BridgeMode::observer ||
         current_mode == ht2mp::ipc::BridgeMode::active) &&
        now >= next_hook_status_ms) {
      const auto hook_stats = live_hook_stats;
      if (!send_hook_activity(
              pipe, previous_hook_stats, hook_stats,
              now - hook_status_window_ms,
              min_calls_per_second == UINT32_MAX ? 0U : min_calls_per_second,
              max_calls_per_second, ht2mp::bridge::ConsumeMaxTickGapUs())) {
        break;
      }
      min_calls_per_second = UINT32_MAX;
      max_calls_per_second = 0U;
      previous_hook_stats = hook_stats;
      hook_status_window_ms = now;
      next_hook_status_ms = now + 5'000U;
    }
    if (current_mode == ht2mp::ipc::BridgeMode::observer &&
        ht2mp::bridge::ActorDiagnosticsReady() &&
        now >= next_actor_status_ms) {
      if (!send_actor_diagnostic_activity(
              pipe, ht2mp::bridge::GetActorDiagnosticStats())) {
        break;
      }
      next_actor_status_ms = now + 5'000U;
    }
    if (current_mode == ht2mp::ipc::BridgeMode::active &&
        ht2mp::bridge::RemoteActorsReady() && now >= next_actor_status_ms) {
      const auto stats = ht2mp::bridge::GetRemoteActorBackendStats();
      if (!send_remote_actor_activity(pipe, stats)) break;
      if (stats.safe_mode_requested || stats.thread_mismatches != 0U) {
        InterlockedExchange(&g_mode,
                            static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
      }
      next_actor_status_ms = now + 2'500U;
    }
    if (ht2mp::bridge::AutoEnterReady()) {
      const auto auto_stats = ht2mp::bridge::GetAutoEnterStats();
      if (auto_stats.state != previous_auto_state) {
        if (!send_auto_enter_activity(pipe, auto_stats)) break;
        previous_auto_state = auto_stats.state;
      }
    }
    pacer.Wait(1U); // worker only; the hooked game thread never waits
  }

  ht2mp::bridge::DisableTelemetryNoWait();
  ht2mp::bridge::DisableAutoEnterNoWait();
  ht2mp::bridge::RequestRemoteActorSafeMode();
  InterlockedExchange(&g_mode,
                      static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
  close_pipe();
  return 0U;
}

ht2mp::ipc::BootstrapResult bootstrap_impl(
    const ht2mp::ipc::BootstrapV1& bootstrap) noexcept {
  if (bootstrap.magic != ht2mp::ipc::kBootstrapMagic ||
      bootstrap.struct_size != sizeof(ht2mp::ipc::BootstrapV1) ||
      bootstrap.abi_version != ht2mp::ipc::kBootstrapAbiVersion) {
    return ht2mp::ipc::BootstrapResult::unsupported_abi;
  }
  if (!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap)) {
    return ht2mp::ipc::BootstrapResult::invalid_argument;
  }
  const auto pipe_end = std::find(bootstrap.pipe_name.begin(),
                                  bootstrap.pipe_name.end(), L'\0');
  if (pipe_end == bootstrap.pipe_name.end()) {
    return ht2mp::ipc::BootstrapResult::invalid_argument;
  }
  const std::wstring_view pipe_name(
      bootstrap.pipe_name.data(),
      static_cast<std::size_t>(pipe_end - bootstrap.pipe_name.begin()));
  if (!pipe_name.starts_with(L"\\\\.\\pipe\\HT2MP-") ||
      !running_inside_king()) {
    return ht2mp::ipc::BootstrapResult::wrong_process;
  }
  if (!WaitNamedPipeW(bootstrap.pipe_name.data(), 10'000U)) {
    return ht2mp::ipc::BootstrapResult::pipe_unavailable;
  }
  const auto pipe = CreateFileW(bootstrap.pipe_name.data(),
                                GENERIC_READ | GENERIC_WRITE, 0U, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    return ht2mp::ipc::BootstrapResult::pipe_unavailable;
  }
  InterlockedExchangePointer(&g_pipe, pipe);
  g_bootstrap = bootstrap;

  const auto experimental_telemetry =
      (bootstrap.flags & static_cast<std::uint32_t>(
                             ht2mp::ipc::BootstrapFlags::
                                 experimental_gog_telemetry)) != 0U;
  const auto experimental_actor_diagnostics =
      (bootstrap.flags & static_cast<std::uint32_t>(
                             ht2mp::ipc::BootstrapFlags::
                                 experimental_gog_actor_diagnostics)) != 0U;
  const auto experimental_remote_actors =
      (bootstrap.flags & static_cast<std::uint32_t>(
                             ht2mp::ipc::BootstrapFlags::
                                 experimental_gog_remote_actors)) != 0U;
  const auto auto_enter =
      (bootstrap.flags & static_cast<std::uint32_t>(
                             ht2mp::ipc::BootstrapFlags::
                                 auto_enter_world)) != 0U;
  const auto online_world =
      (bootstrap.flags & static_cast<std::uint32_t>(
                             ht2mp::ipc::BootstrapFlags::
                                 stock_npc_suppression)) != 0U;
  if (experimental_telemetry) {
    // Complete hash/PE/signature verification happens before MinHook is
    // called. Unknown builds remain passive.
    g_telemetry_initialization =
        ht2mp::bridge::InitializeGogTelemetry(
            bootstrap.profile_id, experimental_actor_diagnostics,
            experimental_remote_actors,
            auto_enter, online_world, bootstrap.vehicle_selector,
            bootstrap.paint_variant);
  } else {
    g_telemetry_initialization = {};
    g_telemetry_initialization.status =
        ht2mp::ipc::BridgeStatusCode::observer_only;
    constexpr std::string_view detail =
        "Passive bridge; hooks require explicit exact-profile experimental opt-in";
    std::copy(detail.begin(), detail.end(),
              g_telemetry_initialization.detail.begin());
    g_telemetry_initialization.detail[detail.size()] = '\0';
  }
  InterlockedExchange(&g_mode,
                      static_cast<LONG>(g_telemetry_initialization.mode));

  const auto worker = CreateThread(nullptr, 0U, pipe_worker, nullptr, 0U, nullptr);
  if (worker == nullptr) {
    ht2mp::bridge::DisableTelemetryNoWait();
    ht2mp::bridge::DisableAutoEnterNoWait();
    ht2mp::bridge::RequestRemoteActorSafeMode();
    close_pipe();
    return ht2mp::ipc::BootstrapResult::worker_start_failed;
  }
  CloseHandle(worker);
  InterlockedExchange(&g_initialization_state, 2);
  return ht2mp::ipc::BootstrapResult::ok;
}

bool copy_bootstrap_request_seh(const ht2mp::ipc::BootstrapV1* request,
                                ht2mp::ipc::BootstrapV1* destination) noexcept {
  __try {
    std::memcpy(destination, request, sizeof(*destination));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return true;
}

} // namespace

extern "C" DWORD WINAPI Ht2mpBootstrap(
    const ht2mp::ipc::BootstrapV1* request) noexcept {
  if (request == nullptr) {
    return static_cast<DWORD>(
        ht2mp::ipc::BootstrapResult::invalid_argument);
  }
  if (InterlockedCompareExchange(&g_initialization_state, 1, 0) != 0) {
    return static_cast<DWORD>(
        ht2mp::ipc::BootstrapResult::already_initialized);
  }
  ht2mp::ipc::BootstrapV1 local{};
  if (!copy_bootstrap_request_seh(request, &local)) {
    return static_cast<DWORD>(
        ht2mp::ipc::BootstrapResult::invalid_argument);
  }
  try {
    return static_cast<DWORD>(bootstrap_impl(local));
  } catch (...) {
    ht2mp::bridge::DisableTelemetryNoWait();
    close_pipe();
    return static_cast<DWORD>(ht2mp::ipc::BootstrapResult::internal_error);
  }
}

BOOL WINAPI DllMain(HINSTANCE instance, const DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  } else if (reason == DLL_PROCESS_DETACH) {
    InterlockedExchange(&g_stop_requested, 1);
    ht2mp::bridge::DisableTelemetryNoWait();
    InterlockedExchange(&g_mode,
                        static_cast<LONG>(ht2mp::ipc::BridgeMode::safe));
    close_pipe();
  }
  return TRUE;
}
