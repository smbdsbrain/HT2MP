#pragma once

#include "ht2mp/game/observer.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

namespace ht2mp::game {

enum class LocalWorldObservationStatus : std::uint8_t {
  sample,
  not_ready,
  read_failed,
  invalid_state,
  changed_during_read,
};

struct LocalWorldSample {
  // Published pose. On exact builds this is the VehicleInstance simulation
  // copy (+0x204/+0x228): the render copy (+0x4ef4/+0x4f18) is refreshed by
  // the game's render-history ring with a multi-record delay and only while
  // the vehicle is "stable", so it lags and stalls under motion. The render
  // copy is still validated for world readiness and used as fallback.
  LocalTransformSample transform;
  // Transient validated address, consumed only by the in-process bridge.
  std::uint32_t vehicle_instance_address{};
  bool transform_from_simulation_copy{};
  std::int32_t current_room_id{-1};
  std::int32_t endpoint_room_a{-1};
  std::int32_t endpoint_room_b{-1};
  std::int32_t vehicle_id{-1};
  std::uint16_t vehicle_type{};
  std::uint8_t paint_variant{};
  std::int32_t road_id{-1};
  std::int32_t node_id{-1};
  double road_distance{};
  std::int32_t road_segment_vector_id{-1};
  std::int32_t road_segment_id{-1};
  std::int32_t aux0{};
  std::int32_t aux1{};
  std::array<std::uint8_t, 32> raw_position_id{};
};

struct FastLocalWorldObservation {
  LocalWorldObservationStatus status{LocalWorldObservationStatus::not_ready};
  LocalWorldSample sample;

  [[nodiscard]] bool has_sample() const noexcept {
    return status == LocalWorldObservationStatus::sample;
  }
};

static_assert(std::is_trivially_copyable_v<FastLocalWorldObservation>);

// Game-thread observer for the exact-build local actor. Construction may
// allocate while resolving a verified profile; PollFast performs bounded,
// read-only memory access and never allocates, formats, waits, or calls into
// game code. Any lifecycle, ownership, list, room, or transform mismatch is
// fail-closed and resets velocity history.
class LocalWorldObserver {
 public:
  static std::unique_ptr<LocalWorldObserver> Create(
      const ProfileVerification& verified, std::uintptr_t module_base,
      ReadOnlyMemory& memory, std::string& error);

  [[nodiscard]] FastLocalWorldObservation PollFast(
      std::uint64_t monotonic_time_us) noexcept;
  void ResetHistory() noexcept;

 private:
  LocalWorldObserver(std::uintptr_t sentinel_cell_address,
                     std::uintptr_t local_player_cell_address,
                     std::uintptr_t room_registry_begin_cell_address,
                     std::uintptr_t room_registry_end_cell_address,
                     std::uintptr_t ai_subsystem_initialized_address,
                     std::uintptr_t viewer_instance_cell_address,
                     AiPlayerPoolLayout layout, bool exact_steam_appearance,
                     ReadOnlyMemory& memory) noexcept;

  std::uintptr_t sentinel_cell_address_{};
  std::uintptr_t local_player_cell_address_{};
  std::uintptr_t room_registry_begin_cell_address_{};
  std::uintptr_t room_registry_end_cell_address_{};
  std::uintptr_t ai_subsystem_initialized_address_{};
  std::uintptr_t viewer_instance_cell_address_{};
  AiPlayerPoolLayout layout_{};
  bool exact_steam_appearance_{};
  ReadOnlyMemory* memory_{};
  std::uint64_t sequence_{};
  std::uint64_t previous_time_us_{};
  ObservedVec3 previous_position_{};
  ObservedQuaternion previous_orientation_{};
  std::int32_t previous_room_id_{-1};
  bool have_previous_{};
};

[[nodiscard]] std::string_view ToString(
    LocalWorldObservationStatus value) noexcept;

}  // namespace ht2mp::game
