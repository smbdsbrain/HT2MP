#pragma once

#include "ht2mp/game/observer.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ht2mp::game {

struct ObservedAiPlayer {
  std::uint32_t node_address{};
  std::uint32_t player_address{};
  std::string name;
  std::int32_t vehicle_id{};
  std::int32_t actor_type{};
  std::uint32_t liveness_marker{};
  std::uint32_t flags{};
  std::uint32_t self_player_id{};
  bool constructor_marker_matches{};
  bool x_controlled{};
  bool self_matches_node{};
  bool selected_local{};
  bool name_valid{};
  // Endpoint RoomIds derived by the game from this actor's current
  // PositionId. They are observation fields, not a single authoritative
  // current room.
  std::int32_t position_room_a{-1};
  std::int32_t position_room_b{-1};
  bool position_rooms_in_bounds{};
  std::array<std::uint8_t, 32> raw_position_id{};
};

// PositionId is still an opaque game-owned value. This view exists solely for
// read-only reverse-engineering telemetry: it preserves all eight little-endian
// dwords and exposes the double observed at bytes 8..15 as a diagnostic
// candidate. It must not be treated as a validated WorldLocation or copied to
// the network protocol.
struct PositionIdDiagnostics {
  std::array<std::uint32_t, 8> dwords{};
  double distance_candidate{};
  bool distance_candidate_finite{};
};

struct ObservedLocalVehicle {
  std::uint32_t viewer_address{};
  std::uint32_t vehicle_address{};
  std::uint32_t owner_player_id{};
  std::uint32_t physics_address{};
  std::uint32_t moving_item_address{};
  std::uint32_t physics_reverse_moving_item{};
  std::array<float, 3> position{};
  std::array<float, 9> raw_orientation{};
  ObservedQuaternion orientation{};
  std::string current_room_name;
  std::int32_t current_room_id{-1};
  bool chain_consistent{};
  bool owner_matches_local{};
  bool physics_available{};
  bool physics_reverse_matches{};
  bool position_finite{};
  bool orientation_valid{};
  bool current_room_resolved{};
  std::string detail;
};

[[nodiscard]] PositionIdDiagnostics DecodePositionIdDiagnostics(
    std::span<const std::uint8_t, 32> raw) noexcept;

enum class AiPlayerPoolStatus : std::uint8_t {
  snapshot,
  not_ready,
  read_failed,
  corrupt_list,
  changed_during_read,
};

struct AiPlayerPoolSnapshot {
  AiPlayerPoolStatus status{AiPlayerPoolStatus::not_ready};
  std::uint32_t sentinel_address{};
  std::uint32_t local_player_id{};
  bool ai_subsystem_initialized{};
  std::uint32_t room_registry_count{};
  // This only proves that the cached local PlayerId is a live, ordinary local
  // actor in the consistent list snapshot. It deliberately does not mean the
  // world/room is loaded and must not be mapped directly to wire in_world.
  bool local_player_valid{};
  // Read-only Viewer -> VehicleInstance diagnostics. Even a consistent chain
  // and resolved current room are not, by themselves, a complete loading or
  // wire-level in_world decision.
  ObservedLocalVehicle local_vehicle;
  // Conservative exact-build readiness conjunction used by the external
  // diagnostic observer. LocalWorldObserver independently repeats the same
  // semantic checks allocation-free on the game thread; no external observer
  // result, pointer, or raw object layout is shared over IPC.
  bool local_world_ready{};
  std::vector<ObservedAiPlayer> players;
  std::string detail;

  [[nodiscard]] bool has_snapshot() const noexcept {
    return status == AiPlayerPoolStatus::snapshot;
  }
};

// Enumerates the game's intrusive AI player list with consistency checks. This
// is intentionally a discovery/diagnostic API: it cannot mutate nodes or player
// objects and is enabled only for a live-validated exact profile.
class AiPlayerPoolObserver {
 public:
  static std::unique_ptr<AiPlayerPoolObserver> Create(
      const ProfileVerification& verified, std::uintptr_t module_base,
      ReadOnlyMemory& memory, std::string& error);

  [[nodiscard]] AiPlayerPoolSnapshot Snapshot() noexcept;

 private:
  AiPlayerPoolObserver(std::uintptr_t sentinel_cell_address,
                       std::uintptr_t local_player_cell_address,
                       std::uintptr_t room_registry_begin_cell_address,
                       std::uintptr_t room_registry_end_cell_address,
                       std::uintptr_t ai_subsystem_initialized_address,
                       std::uintptr_t viewer_instance_cell_address,
                       AiPlayerPoolLayout layout,
                       ReadOnlyMemory& memory) noexcept;

  std::uintptr_t sentinel_cell_address_{};
  std::uintptr_t local_player_cell_address_{};
  std::uintptr_t room_registry_begin_cell_address_{};
  std::uintptr_t room_registry_end_cell_address_{};
  std::uintptr_t ai_subsystem_initialized_address_{};
  std::uintptr_t viewer_instance_cell_address_{};
  AiPlayerPoolLayout layout_{};
  ReadOnlyMemory* memory_{};
};

[[nodiscard]] std::string_view ToString(AiPlayerPoolStatus value) noexcept;

}  // namespace ht2mp::game
