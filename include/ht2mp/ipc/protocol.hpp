#pragma once

#include "ht2mp/ipc/bootstrap.hpp"
#include "ht2mp/protocol/replication.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ht2mp::ipc {

inline constexpr std::uint32_t kFrameMagic = 0x50493248U; // "H2IP"
// Version 6 adds the immutable four-way paint variant.
inline constexpr std::uint16_t kProtocolVersion = 6U;
inline constexpr std::size_t kMaximumPayloadSize = 4096U;
inline constexpr std::size_t kFrameHeaderSize = 16U;
inline constexpr std::size_t kMaximumFrameSize = kFrameHeaderSize + kMaximumPayloadSize;

enum class MessageType : std::uint16_t {
  bridge_hello = 1U,
  bridge_ready = 2U,
  bridge_status = 3U,
  local_sample = 4U,
  spawn_remote = 5U,
  remote_sample = 6U,
  despawn_remote = 7U,
  set_observer_mode = 8U,
  enter_safe_mode = 9U,
  environment = 10U,
};

enum class BridgeMode : std::uint32_t {
  observer = 1U,
  active = 2U,
  safe = 3U,
};

enum class BridgeStatusCode : std::uint32_t {
  ok = 0U,
  observer_only = 1U,
  malformed_command = 2U,
  unsupported_command = 3U,
  queue_overflow = 4U,
  profile_mismatch = 5U,
  symbol_resolution_failed = 6U,
  pipe_disconnected = 7U,
  hook_install_failed = 8U,
  telemetry_runtime_error = 9U,
  online_world_runtime_error = 10U,
  background_tick_stalled = 11U,
};

enum class Capability : std::uint32_t {
  observer_mode = 1U << 0U,
  local_telemetry = 1U << 1U,
  remote_actors = 1U << 2U,
  auto_enter_world = 1U << 3U,
  stock_npc_suppression = 1U << 4U,
  background_tick = 1U << 5U,
  vehicle_state = 1U << 6U,
  environment = 1U << 7U,
};

struct FrameHeader final {
  MessageType type{};
  std::uint32_t payload_size{};
  std::uint32_t sequence{};
};

struct BridgeHelloV1 final {
  std::uint32_t abi_version{kBootstrapAbiVersion};
  std::uint64_t run_id{};
  std::array<std::uint8_t, kNonceSize> nonce{};
  std::array<char, kProfileIdCapacity> profile_id{};
};

struct BridgeReadyV1 final {
  BootstrapResult result{BootstrapResult::ok};
  BridgeMode mode{BridgeMode::observer};
  std::uint32_t capabilities{static_cast<std::uint32_t>(Capability::observer_mode)};
};

struct BridgeStatusV1 final {
  BridgeStatusCode code{BridgeStatusCode::ok};
  BridgeMode mode{BridgeMode::observer};
  std::array<char, 160> detail{};
};

struct SetObserverModeV1 final {
  bool enabled{true};
};

struct EnterSafeModeV1 final {
  std::uint32_t reason{};
};

// IPC-local representation. It intentionally mirrors the semantic network
// state without sharing process pointers or C++ object layout.
struct WorldLocationV1 final {
  std::int32_t room_id{};
  std::int32_t road_id{};
  std::int32_t node_id{};
  double road_distance{};
  std::int32_t road_segment_vector_id{};
  std::int32_t road_segment_id{};
  std::int32_t aux0{};
  std::int32_t aux1{};
};

struct PlayerSampleV1 final {
  std::uint64_t player_id{};
  std::uint64_t incarnation_id{};
  // Capture instant on the ORIGINATING bridge's QPC clock (milliseconds). It
  // is carried unchanged across the network so the receiving bridge can
  // estimate the sender clock offset instead of trusting arrival times.
  std::uint64_t sample_time_ms{};
  // Remote samples only: local QPC microseconds when the sidecar received the
  // datagram (0 = unknown, the bridge worker stamps it on arrival instead).
  std::uint64_t receive_time_us{};
  std::uint32_t sequence{};
  std::uint8_t flags{}; // bit 0: in_world, bit 1: paused, bit 2: teleport
  std::array<double, 3> position{};
  std::array<float, 4> orientation{};
  std::array<float, 3> linear_velocity{};
  std::array<float, 3> angular_velocity{};
  std::uint16_t vehicle_type{};
  std::uint8_t paint_variant{};
  WorldLocationV1 location{};
  ht2mp::protocol::VehicleState vehicle{};
};

struct EnvironmentV1 final {
  std::uint64_t server_time_ms{};
  std::uint64_t receive_time_us{};
  ht2mp::protocol::EnvironmentState state{};
};

struct SpawnRemoteV1 final {
  std::uint64_t player_id{};
  std::uint64_t incarnation_id{};
  std::uint16_t vehicle_type{};
  std::uint8_t paint_variant{};
  std::array<char, 33> display_name{};
};

struct DespawnRemoteV1 final {
  std::uint64_t player_id{};
  std::uint64_t incarnation_id{};
};

} // namespace ht2mp::ipc
