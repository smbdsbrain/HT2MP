#pragma once

#include "remote_pose_timeline.hpp"

#include "ht2mp/game/profile.hpp"
#include "ht2mp/ipc/protocol.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ht2mp::bridge {

struct RemoteActorBackendStats final {
  bool ready{};
  bool safe_mode_requested{};
  std::uint32_t active_slots{};
  std::uint32_t game_actors{};
  std::uint32_t captured_vehicles{};
  std::uint32_t constructor_vehicle_candidates{};
  std::uint32_t resolver_vehicle_candidates{};
  std::array<std::uint32_t, 4> local_vehicle_constructor_args{};
  std::array<std::uint32_t, 4> remote_vehicle_constructor_args{};
  std::uint32_t requested_vehicle_model{};
  std::uint32_t applied_vehicle_model{};
  // Diagnostics only: game-owned VehicleInstance of the first active slot (for
  // the read-only external observer) and native setPositionId call count.
  std::uint32_t vehicle_instance_address{};
  std::uint32_t set_position_calls{};
  std::uint32_t transform_writes{};
  std::uint32_t changed_pose_commands{};
  std::uint32_t pose_sequence{};
  std::uint32_t pose_valid_mask{};
  std::array<float, 3> commanded_position{};
  std::array<float, 3> pre_render_position{};
  std::array<float, 3> pre_physics_position{};
  std::array<float, 3> post_render_position{};
  std::array<float, 3> post_physics_position{};
  float pre_render_orientation_error{};
  float pre_physics_orientation_error{};
  float post_render_orientation_error{};
  float post_physics_orientation_error{};
  std::uint32_t spawns{};
  std::uint32_t despawns{};
  std::uint32_t applied_samples{};
  std::uint32_t suppressed_moves{};
  std::uint32_t suppressed_physics_updates{};
  std::uint32_t corrected_render_updates{};
  std::uint32_t frame_max_gap_us{};
  std::uint32_t scene_reads{};
  float scene_position_error{};
  float scene_orientation_error{};
  std::uint32_t shadow_updates{};
  float shadow_position_error{};
  float shadow_orientation_error{};
  std::uint32_t suppressed_collisions{};
  std::uint32_t suppressed_hits{};
  std::uint32_t validation_failures{};
  std::uint32_t thread_mismatches{};
  // Playback timeline of the first active slot plus queue drop accounting.
  PoseTimelineStats timeline{};
  std::uint32_t dropped_states{};
  std::uint32_t receive_stamps_replaced{};
  std::uint32_t motion_mode_writes{};
  std::uint32_t motion_mode_failures{};
  std::uint32_t suppressed_set_position{};
};

[[nodiscard]] bool InitializeGogRemoteActors(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    std::uint32_t selected_vehicle,
    std::uint32_t selected_paint,
    std::string& error) noexcept;

// Producer API: called only by the named-pipe worker.  No function below
// touches game memory; state updates enter a bounded per-player FIFO.
[[nodiscard]] bool QueueRemoteSpawn(
    const ht2mp::ipc::SpawnRemoteV1& command) noexcept;
[[nodiscard]] bool QueueRemoteSample(
    const ht2mp::ipc::PlayerSampleV1& command) noexcept;
[[nodiscard]] bool QueueRemoteDespawn(
    const ht2mp::ipc::DespawnRemoteV1& command) noexcept;

// Consumer API: called after the original AI tick on its verified owner
// thread. Owns lifecycle, binding validation and native mode switches. Steam's
// vehicle-history hook also advances and writes the pose on that same thread.
void TickGogRemoteActors(bool local_world_ready,
                         std::int32_t local_room_id) noexcept;
void RequestRemoteActorSafeMode() noexcept;

// Lock-free ownership snapshot for the game-thread online-world sanitizer.
// The central creator is shared by stock and peer actors, so PlayerId
// ownership—not actor type—is the preservation boundary.
[[nodiscard]] bool RemoteActorOwnsPlayerId(std::uint32_t player_id) noexcept;

[[nodiscard]] bool GetRemoteVehicleState(std::uint32_t vehicle, ht2mp::protocol::VehicleState& state,
    std::array<float, ht2mp::protocol::kMaxVehicleWheels>& wheel_phases) noexcept;

[[nodiscard]] bool RemoteActorsReady() noexcept;
[[nodiscard]] RemoteActorBackendStats GetRemoteActorBackendStats() noexcept;

} // namespace ht2mp::bridge
