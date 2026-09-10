#include "ht2mp/game/ai_player_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_set>

namespace ht2mp::game {
namespace {

std::uint32_t DecodeU32(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data() + offset);
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

bool IsPlausibleTargetPointer(std::uint32_t pointer) noexcept {
  return pointer >= 0x00010000U && pointer < 0x80000000U &&
         (pointer & 0x3U) == 0U;
}

bool ReadBytes(ReadOnlyMemory& memory, std::uint32_t address,
               std::span<std::byte> destination, std::string& error) noexcept {
  if (!IsPlausibleTargetPointer(address)) {
    error = "implausible 32-bit target pointer";
    return false;
  }
  return memory.Read(static_cast<std::uintptr_t>(address), destination, error);
}

struct ListNode {
  std::uint32_t next{};
  std::uint32_t previous{};
};

bool ReadNode(ReadOnlyMemory& memory, std::uint32_t address, ListNode& node,
              std::string& error) noexcept {
  std::array<std::byte, 8> bytes{};
  if (!ReadBytes(memory, address, bytes, error)) {
    return false;
  }
  node.next = DecodeU32(bytes, 0U);
  node.previous = DecodeU32(bytes, 4U);
  return true;
}

bool SameNode(const ListNode& left, const ListNode& right) noexcept {
  return left.next == right.next && left.previous == right.previous;
}

bool CheckedAdd(const std::uint32_t base, const std::uint32_t offset,
                std::uint32_t& result) noexcept {
  if (base > std::numeric_limits<std::uint32_t>::max() - offset) {
    return false;
  }
  result = base + offset;
  return IsPlausibleTargetPointer(result);
}

bool ReadU32(ReadOnlyMemory& memory, const std::uint32_t address,
             std::uint32_t& value, std::string& error) noexcept {
  std::array<std::byte, 4> bytes{};
  if (!ReadBytes(memory, address, bytes, error)) {
    return false;
  }
  value = DecodeU32(bytes, 0U);
  return true;
}

bool DecodeInlineName(const std::span<const std::byte> bytes, std::string& name,
                      std::string& error) {
  name.clear();
  for (const auto value : bytes) {
    const auto character = static_cast<unsigned char>(value);
    if (character == 0U) {
      if (name.empty()) {
        error = "room name is empty";
        return false;
      }
      return true;
    }
    if (character < 0x20U || character == 0x7fU) {
      error = "room name contains a non-printable byte";
      name.clear();
      return false;
    }
    name.push_back(static_cast<char>(character));
  }
  error = "room name is not terminated within the profile bound";
  name.clear();
  return false;
}

bool ReadRoomName(ReadOnlyMemory& memory, const std::uint32_t room_registry_begin,
                  const std::uint32_t room_id,
                  const LocalVehicleLayout& layout, std::string& name,
                  std::string& error) {
  if (room_id > (std::numeric_limits<std::uint32_t>::max() -
                 room_registry_begin) /
                    4U) {
    error = "room registry entry address overflows";
    return false;
  }
  const auto entry_address = room_registry_begin + room_id * 4U;
  std::uint32_t room_address{};
  if (!ReadU32(memory, entry_address, room_address, error) ||
      !IsPlausibleTargetPointer(room_address)) {
    error = "room registry contains an implausible object pointer";
    return false;
  }

  const auto header_size = std::max(
      {layout.room_name_begin_offset + 4U, layout.room_name_end_offset + 4U,
       layout.room_name_capacity_offset + 4U, layout.room_id_offset + 4U});
  if (header_size > 64U) {
    error = "room object layout exceeds the observer bound";
    return false;
  }
  std::array<std::byte, 64> header_storage{};
  const auto header = std::span<std::byte>(header_storage).first(header_size);
  if (!ReadBytes(memory, room_address, header, error)) {
    return false;
  }
  const auto immutable_header = std::span<const std::byte>(header);
  const auto begin =
      DecodeU32(immutable_header, layout.room_name_begin_offset);
  const auto end = DecodeU32(immutable_header, layout.room_name_end_offset);
  const auto capacity =
      DecodeU32(immutable_header, layout.room_name_capacity_offset);
  const auto embedded_id = DecodeU32(immutable_header, layout.room_id_offset);
  if (embedded_id != room_id || !IsPlausibleTargetPointer(begin) ||
      end < begin || capacity < end || end - begin == 0U ||
      end - begin > layout.maximum_room_name_bytes ||
      capacity - begin > 4096U) {
    error = "room object name vector or embedded RoomId is invalid";
    return false;
  }

  std::array<std::byte, 128> name_storage{};
  const auto size = static_cast<std::size_t>(end - begin);
  if (size > name_storage.size()) {
    error = "room object name exceeds the observer storage";
    return false;
  }
  const auto name_bytes = std::span<std::byte>(name_storage).first(size);
  if (!ReadBytes(memory, begin, name_bytes, error)) {
    return false;
  }
  return DecodeInlineName(std::span<const std::byte>(name_bytes), name, error);
}

void ObserveLocalVehicle(ReadOnlyMemory& memory,
                         const std::uintptr_t viewer_instance_cell_address,
                         const std::uint32_t room_registry_begin,
                         const std::uint32_t room_registry_count,
                         const std::uint32_t local_player_id,
                         const ObservedAiPlayer& local_player,
                         const LocalVehicleLayout& layout,
                         ObservedLocalVehicle& result) noexcept {
  std::string error;
  std::array<std::byte, 4> viewer_before_bytes{};
  if (!memory.Read(viewer_instance_cell_address, viewer_before_bytes, error)) {
    result.detail = error.empty() ? "cannot read Viewer instance cell" : error;
    return;
  }
  const auto viewer = DecodeU32(viewer_before_bytes, 0U);
  result.viewer_address = viewer;
  if (!IsPlausibleTargetPointer(viewer)) {
    result.detail = "Viewer instance is not available";
    return;
  }

  std::uint32_t vehicle_pointer_address{};
  if (!CheckedAdd(viewer, layout.viewer_vehicle_offset,
                  vehicle_pointer_address)) {
    result.detail = "Viewer vehicle field address is implausible";
    return;
  }
  std::uint32_t vehicle{};
  if (!ReadU32(memory, vehicle_pointer_address, vehicle, error) ||
      !IsPlausibleTargetPointer(vehicle)) {
    result.detail = "Viewer has no plausible VehicleInstance";
    return;
  }
  result.vehicle_address = vehicle;

  std::uint32_t owner_address{};
  std::uint32_t orientation_address{};
  std::uint32_t room_name_address{};
  std::uint32_t physics_address{};
  std::uint32_t moving_item{};
  if (!CheckedAdd(vehicle, layout.vehicle_owner_player_id_offset,
                  owner_address) ||
      !CheckedAdd(vehicle, layout.vehicle_orientation_offset,
                  orientation_address) ||
      !CheckedAdd(vehicle, layout.vehicle_current_room_name_offset,
                  room_name_address) ||
      !CheckedAdd(vehicle, layout.vehicle_physics_offset, physics_address) ||
      !CheckedAdd(vehicle, layout.vehicle_moving_item_offset, moving_item)) {
    result.detail = "VehicleInstance field address is implausible";
    return;
  }

  std::uint32_t owner{};
  std::uint32_t physics{};
  std::array<std::byte, 256> transform_storage{};
  std::array<std::byte, 128> room_name_storage{};
  if (layout.maximum_room_name_bytes == 0U ||
      layout.maximum_room_name_bytes > room_name_storage.size() ||
      layout.vehicle_transform_read_size == 0U ||
      layout.vehicle_transform_read_size > transform_storage.size() ||
      layout.vehicle_position_offset < layout.vehicle_orientation_offset) {
    result.detail = "profile vehicle transform/name bounds are invalid";
    return;
  }
  const auto transform_bytes =
      std::span<std::byte>(transform_storage)
          .first(layout.vehicle_transform_read_size);
  const auto room_name_bytes =
      std::span<std::byte>(room_name_storage)
          .first(layout.maximum_room_name_bytes);
  if (!ReadU32(memory, owner_address, owner, error) ||
      !ReadBytes(memory, orientation_address, transform_bytes, error) ||
      !ReadBytes(memory, room_name_address, room_name_bytes, error) ||
      !ReadU32(memory, physics_address, physics, error)) {
    result.detail = error.empty() ? "cannot read VehicleInstance diagnostics"
                                  : error;
    return;
  }
  result.owner_player_id = owner;
  result.physics_address = physics;
  result.moving_item_address = moving_item;
  LocalTransformSample decoded_transform;
  const RigidTransformLayout transform_layout{
      0U,
      layout.vehicle_position_offset - layout.vehicle_orientation_offset,
      layout.vehicle_transform_read_size,
      layout.maximum_absolute_position,
      layout.basis_length_tolerance,
      layout.basis_dot_tolerance,
  };
  const auto transform_status = DecodeRigidTransformFast(
      std::span<const std::byte>(transform_bytes), transform_layout,
      decoded_transform);
  result.position = {static_cast<float>(decoded_transform.position.x),
                     static_cast<float>(decoded_transform.position.y),
                     static_cast<float>(decoded_transform.position.z)};
  result.raw_orientation = decoded_transform.raw_orientation;
  result.orientation = decoded_transform.orientation;
  result.position_finite = transform_status == ObservationStatus::sample;
  result.orientation_valid = transform_status == ObservationStatus::sample;
  result.owner_matches_local = owner == local_player_id;
  result.physics_available = IsPlausibleTargetPointer(physics);
  if (result.physics_available) {
    std::uint32_t reverse_address{};
    if (!CheckedAdd(physics, layout.physics_vehicle_reverse_offset,
                    reverse_address) ||
        !ReadU32(memory, reverse_address, result.physics_reverse_moving_item,
                 error)) {
      result.detail = error.empty()
                          ? "cannot read physics reverse VehicleInstance link"
                          : error;
      return;
    }
    result.physics_reverse_matches =
        result.physics_reverse_moving_item == moving_item;
  }
  if (!DecodeInlineName(std::span<const std::byte>(room_name_bytes),
                        result.current_room_name, error)) {
    result.detail = std::move(error);
    return;
  }

  std::array<std::int32_t, 2> endpoint_ids{local_player.position_room_a,
                                           local_player.position_room_b};
  std::uint32_t matching_rooms{};
  for (std::size_t index = 0; index < endpoint_ids.size(); ++index) {
    const auto signed_id = endpoint_ids[index];
    if (signed_id < 0 ||
        static_cast<std::uint32_t>(signed_id) >= room_registry_count ||
        (index != 0U && endpoint_ids[0] == signed_id)) {
      continue;
    }
    std::string candidate_name;
    std::string candidate_error;
    if (ReadRoomName(memory, room_registry_begin,
                     static_cast<std::uint32_t>(signed_id), layout,
                     candidate_name, candidate_error) &&
        candidate_name == result.current_room_name) {
      ++matching_rooms;
      result.current_room_id = signed_id;
    }
  }
  result.current_room_resolved = matching_rooms == 1U;
  if (!result.current_room_resolved) {
    result.current_room_id = -1;
  }

  std::array<std::byte, 4> viewer_after_bytes{};
  std::uint32_t vehicle_after{};
  std::uint32_t owner_after{};
  std::uint32_t physics_after{};
  std::uint32_t physics_reverse_after{};
  std::array<std::byte, 128> room_name_after_storage{};
  const auto room_name_after =
      std::span<std::byte>(room_name_after_storage)
          .first(layout.maximum_room_name_bytes);
  std::uint32_t physics_reverse_address{};
  if (!CheckedAdd(physics, layout.physics_vehicle_reverse_offset,
                  physics_reverse_address) ||
      !memory.Read(viewer_instance_cell_address, viewer_after_bytes, error) ||
      !ReadU32(memory, vehicle_pointer_address, vehicle_after, error) ||
      !ReadU32(memory, owner_address, owner_after, error) ||
      !ReadU32(memory, physics_address, physics_after, error) ||
      !ReadU32(memory, physics_reverse_address, physics_reverse_after, error) ||
      !ReadBytes(memory, room_name_address, room_name_after, error) ||
      DecodeU32(viewer_after_bytes, 0U) != viewer || vehicle_after != vehicle ||
      owner_after != owner || physics_after != physics ||
      physics_reverse_after != result.physics_reverse_moving_item ||
      !std::equal(room_name_bytes.begin(), room_name_bytes.end(),
                  room_name_after.begin())) {
    result.detail = "Viewer/VehicleInstance chain changed during observation";
    return;
  }

  result.chain_consistent = true;
  if (!result.owner_matches_local) {
    result.detail = "VehicleInstance owner does not match cached local PlayerId";
  } else if (!result.position_finite) {
    result.detail = "VehicleInstance rigid transform is invalid";
  } else if (!result.physics_available) {
    result.detail = "VehicleInstance physics object is not available";
  } else if (!result.physics_reverse_matches) {
    result.detail =
        "physics reverse link does not match VehicleInstance MovingItem";
  } else if (!result.current_room_resolved) {
    result.detail =
        "current room name did not resolve uniquely to a bounded endpoint RoomId";
  } else {
    result.detail =
        "consistent read-only local VehicleInstance and endpoint room match";
  }
}

bool ReadPlayer(ReadOnlyMemory& memory, std::uint32_t node_address,
                std::uint32_t player_address, const AiPlayerPoolLayout& layout,
                ObservedAiPlayer& player, std::string& error) noexcept {
  const auto minimum_size = std::max(
      {layout.player_name_capacity_offset + 4U,
       layout.player_position_offset + layout.player_position_size,
       layout.player_vehicle_id_offset + 4U,
       layout.player_liveness_marker_offset + 4U,
       layout.player_actor_type_offset + 4U,
       layout.player_flags_offset + 4U,
       layout.player_self_id_offset + 4U,
       layout.player_position_room_a_offset + 4U,
       layout.player_position_room_b_offset + 4U});
  if (minimum_size > 1024U || layout.player_position_size != 32U) {
    error = "invalid AI player layout in profile";
    return false;
  }
  std::array<std::byte, 1024> storage{};
  auto bytes = std::span<std::byte>(storage).first(minimum_size);
  if (!ReadBytes(memory, player_address, bytes, error)) {
    return false;
  }
  const auto immutable_bytes = std::span<const std::byte>(bytes);
  player.node_address = node_address;
  player.player_address = player_address;
  player.vehicle_id = static_cast<std::int32_t>(
      DecodeU32(immutable_bytes, layout.player_vehicle_id_offset));
  player.liveness_marker =
      DecodeU32(immutable_bytes, layout.player_liveness_marker_offset);
  player.actor_type = static_cast<std::int32_t>(
      DecodeU32(immutable_bytes, layout.player_actor_type_offset));
  player.flags = DecodeU32(immutable_bytes, layout.player_flags_offset);
  player.self_player_id =
      DecodeU32(immutable_bytes, layout.player_self_id_offset);
  player.position_room_a = static_cast<std::int32_t>(
      DecodeU32(immutable_bytes, layout.player_position_room_a_offset));
  player.position_room_b = static_cast<std::int32_t>(
      DecodeU32(immutable_bytes, layout.player_position_room_b_offset));
  player.constructor_marker_matches =
      player.liveness_marker == layout.constructor_liveness_marker;
  player.x_controlled = (player.flags & layout.x_controlled_flag_mask) != 0U;
  player.self_matches_node = player.self_player_id == node_address;
  std::memcpy(player.raw_position_id.data(),
              immutable_bytes.data() + layout.player_position_offset,
              player.raw_position_id.size());

  const auto begin =
      DecodeU32(immutable_bytes, layout.player_name_begin_offset);
  const auto end = DecodeU32(immutable_bytes, layout.player_name_end_offset);
  const auto capacity =
      DecodeU32(immutable_bytes, layout.player_name_capacity_offset);
  if (begin == 0U && end == 0U && capacity == 0U) {
    player.name_valid = true;
    return true;
  }
  if (!IsPlausibleTargetPointer(begin) || end < begin || capacity < end ||
      end - begin > 64U || capacity - begin > 4096U) {
    player.name = "<invalid-name-vector>";
    return true;
  }
  const auto size = static_cast<std::size_t>(end - begin);
  std::array<std::byte, 64> name_bytes{};
  std::string name_error;
  if (size != 0U &&
      !ReadBytes(memory, begin, std::span<std::byte>(name_bytes).first(size),
                 name_error)) {
    player.name = "<unreadable-name>";
    return true;
  }
  player.name.reserve(size);
  bool printable = true;
  for (std::size_t index = 0; index < size; ++index) {
    const auto character = static_cast<unsigned char>(name_bytes[index]);
    if (character == 0U) {
      break;
    }
    if (character < 0x20U || character == 0x7fU) {
      printable = false;
    }
    player.name.push_back(static_cast<char>(character));
  }
  player.name_valid = printable;
  if (!printable) {
    player.name = "<invalid-name-bytes>";
  }
  return true;
}

}  // namespace

PositionIdDiagnostics DecodePositionIdDiagnostics(
    const std::span<const std::uint8_t, 32> raw) noexcept {
  PositionIdDiagnostics result;
  for (std::size_t index = 0; index < result.dwords.size(); ++index) {
    const auto offset = index * sizeof(std::uint32_t);
    result.dwords[index] =
        static_cast<std::uint32_t>(raw[offset]) |
        (static_cast<std::uint32_t>(raw[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(raw[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(raw[offset + 3U]) << 24U);
  }

  // GOG static analysis and live movement correlation place the diagnostic
  // double at bytes 8..15 (dwords 2 and 3). Reassemble those bits explicitly
  // so host alignment and endian do
  // not affect the observation. A non-finite value is never exposed as valid.
  const auto distance_bits = static_cast<std::uint64_t>(result.dwords[2]) |
                             (static_cast<std::uint64_t>(result.dwords[3])
                              << 32U);
  static_assert(sizeof(result.distance_candidate) == sizeof(distance_bits));
  std::memcpy(&result.distance_candidate, &distance_bits,
              sizeof(result.distance_candidate));
  result.distance_candidate_finite = std::isfinite(result.distance_candidate);
  if (!result.distance_candidate_finite) {
    result.distance_candidate = 0.0;
  }
  return result;
}

AiPlayerPoolObserver::AiPlayerPoolObserver(
    std::uintptr_t sentinel_cell_address,
    std::uintptr_t local_player_cell_address,
    std::uintptr_t room_registry_begin_cell_address,
    std::uintptr_t room_registry_end_cell_address,
    std::uintptr_t ai_subsystem_initialized_address,
    std::uintptr_t viewer_instance_cell_address,
    AiPlayerPoolLayout layout,
    ReadOnlyMemory& memory) noexcept
    : sentinel_cell_address_(sentinel_cell_address),
      local_player_cell_address_(local_player_cell_address),
      room_registry_begin_cell_address_(room_registry_begin_cell_address),
      room_registry_end_cell_address_(room_registry_end_cell_address),
      ai_subsystem_initialized_address_(ai_subsystem_initialized_address),
      viewer_instance_cell_address_(viewer_instance_cell_address),
      layout_(layout),
      memory_(&memory) {}

std::unique_ptr<AiPlayerPoolObserver> AiPlayerPoolObserver::Create(
    const ProfileVerification& verified, std::uintptr_t module_base,
    ReadOnlyMemory& memory, std::string& error) {
  error.clear();
  if (!verified.accepted()) {
    error = "game profile has not passed fail-closed verification";
    return nullptr;
  }
  const auto& layout = verified.profile->ai_player_pool;
  if (!layout.observer_enabled) {
    error = "AI player pool observer is disabled for this profile";
    return nullptr;
  }
  const auto* sentinel_symbol =
      verified.FindSymbol(layout.sentinel_cell_symbol);
  if (sentinel_symbol == nullptr || !sentinel_symbol->accepted) {
    error = "verified AI player sentinel symbol is unavailable";
    return nullptr;
  }
  const auto* local_symbol =
      verified.FindSymbol(layout.local_player_cell_symbol);
  if (local_symbol == nullptr || !local_symbol->accepted) {
    error = "verified local PlayerId cell symbol is unavailable";
    return nullptr;
  }
  const auto* room_begin_symbol =
      verified.FindSymbol(layout.room_registry_begin_cell_symbol);
  const auto* room_end_symbol =
      verified.FindSymbol(layout.room_registry_end_cell_symbol);
  const auto* initialized_symbol =
      verified.FindSymbol(layout.ai_subsystem_initialized_symbol);
  if (room_begin_symbol == nullptr || !room_begin_symbol->accepted ||
      room_end_symbol == nullptr || !room_end_symbol->accepted ||
      initialized_symbol == nullptr || !initialized_symbol->accepted) {
    error = "verified AI room/lifecycle symbols are unavailable";
    return nullptr;
  }
  const auto* viewer_symbol =
      verified.FindSymbol(layout.local_vehicle.viewer_instance_cell_symbol);
  if (!layout.local_vehicle.observer_enabled || viewer_symbol == nullptr ||
      !viewer_symbol->accepted) {
    error = "verified local Viewer/Vehicle symbol is unavailable";
    return nullptr;
  }
  const auto maximum_rva = std::numeric_limits<std::uintptr_t>::max() - module_base;
  if (sentinel_symbol->result_rva > maximum_rva ||
      local_symbol->result_rva > maximum_rva ||
      room_begin_symbol->result_rva > maximum_rva ||
      room_end_symbol->result_rva > maximum_rva ||
      initialized_symbol->result_rva > maximum_rva ||
      viewer_symbol->result_rva > maximum_rva) {
    error = "runtime AI observer address overflows uintptr_t";
    return nullptr;
  }
  if (layout.node_next_offset != 0U || layout.node_previous_offset != 4U ||
      layout.node_player_offset != 8U || layout.maximum_nodes == 0U ||
       layout.maximum_nodes > 4096U || layout.player_position_size != 32U ||
       layout.player_vehicle_descriptor_offset == 0U ||
       layout.player_vehicle_descriptor_offset > 0x10000U - 4U ||
       layout.vehicle_descriptor_vehicle_id_offset > 60U ||
       layout.vehicle_descriptor_model_selector_offset > 60U ||
       layout.vehicle_descriptor_paint_variant_offset > 60U ||
       layout.vehicle_descriptor_vehicle_id_offset ==
           layout.vehicle_descriptor_model_selector_offset ||
       layout.vehicle_descriptor_vehicle_id_offset ==
           layout.vehicle_descriptor_paint_variant_offset ||
       layout.vehicle_descriptor_model_selector_offset ==
           layout.vehicle_descriptor_paint_variant_offset ||
       layout.maximum_vehicle_model_selector == 0U ||
       layout.maximum_vehicle_paint_variant != 3U ||
       layout.player_position_room_a_offset ==
           layout.player_position_room_b_offset ||
       layout.maximum_rooms == 0U || layout.maximum_rooms > 65536U ||
       layout.maximum_world_location_id == 0U ||
       layout.maximum_road_distance <= 0.0 ||
       layout.constructor_liveness_marker == 0U ||
       layout.player_self_id_offset == 0U ||
       layout.required_local_actor_type <= 0 ||
       layout.local_vehicle.maximum_room_name_bytes == 0U ||
       layout.local_vehicle.maximum_room_name_bytes > 128U ||
       layout.local_vehicle.vehicle_orientation_offset >=
           layout.local_vehicle.vehicle_position_offset ||
       layout.local_vehicle.vehicle_transform_read_size == 0U ||
       layout.local_vehicle.vehicle_transform_read_size > 256U ||
       layout.local_vehicle.vehicle_simulation_orientation_offset == 0U ||
       layout.local_vehicle.vehicle_simulation_position_offset <=
           layout.local_vehicle.vehicle_simulation_orientation_offset ||
       layout.local_vehicle.vehicle_simulation_position_offset -
                   layout.local_vehicle.vehicle_simulation_orientation_offset !=
               9U * sizeof(float) ||
       layout.local_vehicle.vehicle_moving_item_offset == 0U ||
       layout.local_vehicle.vehicle_moving_item_offset > 0x10000U ||
       layout.local_vehicle.physics_vehicle_reverse_offset == 0U ||
       layout.local_vehicle.physics_vehicle_reverse_offset > 0x100000U ||
       layout.local_vehicle.physics_orientation_offset == 0U ||
       layout.local_vehicle.physics_position_offset <=
           layout.local_vehicle.physics_orientation_offset ||
       layout.local_vehicle.physics_orientation_offset >
           0x10000U - 9U * sizeof(float) ||
       layout.local_vehicle.physics_position_offset >
           0x10000U - 3U * sizeof(float) ||
       layout.local_vehicle.physics_body_to_world_orientation_offset == 0U ||
       layout.local_vehicle.physics_body_to_world_orientation_offset >
           0x10000U - 9U * sizeof(float) ||
       layout.local_vehicle.physics_world_to_body_orientation_offset == 0U ||
       layout.local_vehicle.physics_world_to_body_orientation_offset >
           0x10000U - 9U * sizeof(float) ||
       layout.local_vehicle.physics_body_to_world_orientation_offset ==
           layout.local_vehicle.physics_world_to_body_orientation_offset ||
       layout.local_vehicle.vehicle_position_offset -
                   layout.local_vehicle.vehicle_orientation_offset +
               3U * sizeof(float) >
           layout.local_vehicle.vehicle_transform_read_size ||
       layout.local_vehicle.maximum_absolute_position <= 0.0F ||
       layout.local_vehicle.basis_length_tolerance <= 0.0F ||
       layout.local_vehicle.basis_dot_tolerance <= 0.0F) {
    error = "profile contains an unsupported AI list layout";
    return nullptr;
  }
  return std::unique_ptr<AiPlayerPoolObserver>(new AiPlayerPoolObserver(
      module_base + sentinel_symbol->result_rva,
      module_base + local_symbol->result_rva,
      module_base + room_begin_symbol->result_rva,
      module_base + room_end_symbol->result_rva,
      module_base + initialized_symbol->result_rva,
      module_base + viewer_symbol->result_rva, layout, memory));
}

AiPlayerPoolSnapshot AiPlayerPoolObserver::Snapshot() noexcept {
  AiPlayerPoolSnapshot result;
  std::array<std::byte, 4> room_begin_before_bytes{};
  std::array<std::byte, 4> room_end_before_bytes{};
  std::array<std::byte, 1> initialized_before_bytes{};
  std::string read_error;
  if (!memory_->Read(room_registry_begin_cell_address_,
                     room_begin_before_bytes, read_error) ||
      !memory_->Read(room_registry_end_cell_address_, room_end_before_bytes,
                     read_error) ||
      !memory_->Read(ai_subsystem_initialized_address_,
                     initialized_before_bytes, read_error)) {
    result.status = AiPlayerPoolStatus::read_failed;
    result.detail = read_error.empty()
                        ? "cannot read AI room/lifecycle diagnostics"
                        : std::move(read_error);
    return result;
  }
  const auto room_begin = DecodeU32(room_begin_before_bytes, 0U);
  const auto room_end = DecodeU32(room_end_before_bytes, 0U);
  result.ai_subsystem_initialized =
      static_cast<std::uint8_t>(initialized_before_bytes[0]) != 0U;
  if (room_begin == 0U && room_end == 0U) {
    result.room_registry_count = 0U;
  } else if (!IsPlausibleTargetPointer(room_begin) ||
             !IsPlausibleTargetPointer(room_end) || room_end < room_begin ||
             ((room_end - room_begin) % 4U) != 0U ||
             (room_end - room_begin) / 4U > layout_.maximum_rooms) {
    result.status = AiPlayerPoolStatus::corrupt_list;
    result.detail = "AI room registry bounds are implausible";
    return result;
  } else {
    result.room_registry_count = (room_end - room_begin) / 4U;
  }

  std::array<std::byte, 4> pointer_bytes{};
  if (!memory_->Read(sentinel_cell_address_, pointer_bytes, read_error)) {
    result.status = AiPlayerPoolStatus::read_failed;
    result.detail = read_error.empty() ? "cannot read AI list sentinel cell"
                                       : std::move(read_error);
    return result;
  }
  const auto sentinel = DecodeU32(pointer_bytes, 0U);
  result.sentinel_address = sentinel;
  if (sentinel == 0U) {
    result.status = AiPlayerPoolStatus::not_ready;
    result.detail = "AI list has not been initialized";
    return result;
  }
  if (!IsPlausibleTargetPointer(sentinel)) {
    result.status = AiPlayerPoolStatus::corrupt_list;
    result.detail = "AI list sentinel pointer is implausible";
    return result;
  }

  std::array<std::byte, 4> local_pointer_bytes{};
  if (!memory_->Read(local_player_cell_address_, local_pointer_bytes,
                     read_error)) {
    result.status = AiPlayerPoolStatus::read_failed;
    result.detail = read_error.empty() ? "cannot read local PlayerId cell"
                                       : std::move(read_error);
    return result;
  }
  const auto local_player_id = DecodeU32(local_pointer_bytes, 0U);
  result.local_player_id = local_player_id;
  if (local_player_id != 0U && local_player_id != sentinel &&
      !IsPlausibleTargetPointer(local_player_id)) {
    result.status = AiPlayerPoolStatus::corrupt_list;
    result.detail = "cached local PlayerId is implausible";
    return result;
  }

  ListNode sentinel_before;
  if (!ReadNode(*memory_, sentinel, sentinel_before, read_error)) {
    result.status = AiPlayerPoolStatus::read_failed;
    result.detail = "cannot read AI list sentinel: " + read_error;
    return result;
  }
  if (!IsPlausibleTargetPointer(sentinel_before.next) ||
      !IsPlausibleTargetPointer(sentinel_before.previous)) {
    result.status = AiPlayerPoolStatus::corrupt_list;
    result.detail = "sentinel links are implausible";
    return result;
  }

  std::unordered_set<std::uint32_t> visited;
  visited.reserve(layout_.maximum_nodes);
  auto current = sentinel_before.next;
  auto expected_previous = sentinel;
  while (current != sentinel) {
    if (result.players.size() >= layout_.maximum_nodes) {
      result.status = AiPlayerPoolStatus::corrupt_list;
      result.detail = "AI list exceeds the profile node bound";
      result.players.clear();
      return result;
    }
    if (!visited.insert(current).second) {
      result.status = AiPlayerPoolStatus::corrupt_list;
      result.detail = "AI list contains a cycle that does not reach the sentinel";
      result.players.clear();
      return result;
    }
    ListNode node;
    if (!ReadNode(*memory_, current, node, read_error)) {
      result.status = AiPlayerPoolStatus::read_failed;
      result.detail = "cannot read AI list node: " + read_error;
      result.players.clear();
      return result;
    }
    if (node.previous != expected_previous) {
      result.status = AiPlayerPoolStatus::changed_during_read;
      result.detail = "AI list backlink changed during traversal";
      result.players.clear();
      return result;
    }
    if (!IsPlausibleTargetPointer(node.next) ||
        current > std::numeric_limits<std::uint32_t>::max() -
                      layout_.node_player_offset) {
      result.status = AiPlayerPoolStatus::corrupt_list;
      result.detail = "AI list node contains an implausible pointer";
      result.players.clear();
      return result;
    }
    const auto player_address = current + layout_.node_player_offset;
    if (!IsPlausibleTargetPointer(player_address)) {
      result.status = AiPlayerPoolStatus::corrupt_list;
      result.detail = "embedded AI player address is implausible";
      result.players.clear();
      return result;
    }
    ObservedAiPlayer player;
    if (!ReadPlayer(*memory_, current, player_address, layout_, player, read_error)) {
      result.status = AiPlayerPoolStatus::read_failed;
      result.detail = "cannot read AI player: " + read_error;
      result.players.clear();
      return result;
    }
    player.selected_local = current == local_player_id;
    const auto valid_room = [&result](const std::int32_t room) {
      return room == -1 ||
             (room >= 0 && static_cast<std::uint32_t>(room) <
                               result.room_registry_count);
    };
    player.position_rooms_in_bounds = valid_room(player.position_room_a) &&
                                      valid_room(player.position_room_b);
    result.players.push_back(std::move(player));
    expected_previous = current;
    current = node.next;
  }
  if (sentinel_before.previous != expected_previous) {
    result.status = AiPlayerPoolStatus::changed_during_read;
    result.detail = "AI list tail changed during traversal";
    result.players.clear();
    return result;
  }

  std::array<std::byte, 4> pointer_after{};
  std::array<std::byte, 4> local_pointer_after{};
  std::array<std::byte, 4> room_begin_after{};
  std::array<std::byte, 4> room_end_after{};
  std::array<std::byte, 1> initialized_after{};
  ListNode sentinel_after;
  if (!memory_->Read(sentinel_cell_address_, pointer_after, read_error) ||
      DecodeU32(pointer_after, 0U) != sentinel ||
      !memory_->Read(local_player_cell_address_, local_pointer_after,
                     read_error) ||
      DecodeU32(local_pointer_after, 0U) != local_player_id ||
      !memory_->Read(room_registry_begin_cell_address_, room_begin_after,
                     read_error) ||
      !memory_->Read(room_registry_end_cell_address_, room_end_after,
                     read_error) ||
      !memory_->Read(ai_subsystem_initialized_address_, initialized_after,
                     read_error) ||
      DecodeU32(room_begin_after, 0U) != room_begin ||
      DecodeU32(room_end_after, 0U) != room_end ||
      initialized_after != initialized_before_bytes ||
      !ReadNode(*memory_, sentinel, sentinel_after, read_error) ||
      !SameNode(sentinel_before, sentinel_after)) {
    result.status = AiPlayerPoolStatus::changed_during_read;
    result.detail = "AI list changed while the snapshot was collected";
    result.players.clear();
    return result;
  }

  if (local_player_id != 0U && local_player_id != sentinel) {
    const auto selected = std::find_if(
        result.players.begin(), result.players.end(),
        [local_player_id](const ObservedAiPlayer& player) {
          return player.node_address == local_player_id;
        });
    if (selected == result.players.end()) {
      result.status = AiPlayerPoolStatus::changed_during_read;
      result.detail =
          "cached local PlayerId was not present in the consistent list";
      result.players.clear();
      return result;
    }
    result.local_player_valid =
        selected->actor_type == layout_.required_local_actor_type &&
        selected->constructor_marker_matches && !selected->x_controlled &&
        selected->self_matches_node;
    if (result.local_player_valid) {
      ObserveLocalVehicle(*memory_, viewer_instance_cell_address_, room_begin,
                          result.room_registry_count, local_player_id, *selected,
                          layout_.local_vehicle, result.local_vehicle);
      std::array<std::byte, 4> room_begin_vehicle_after{};
      std::array<std::byte, 4> room_end_vehicle_after{};
      if (!memory_->Read(room_registry_begin_cell_address_,
                         room_begin_vehicle_after, read_error) ||
          !memory_->Read(room_registry_end_cell_address_, room_end_vehicle_after,
                         read_error) ||
          DecodeU32(room_begin_vehicle_after, 0U) != room_begin ||
          DecodeU32(room_end_vehicle_after, 0U) != room_end) {
        result.local_vehicle.chain_consistent = false;
        result.local_vehicle.current_room_resolved = false;
        result.local_vehicle.current_room_id = -1;
        result.local_vehicle.detail =
            "room registry changed during local VehicleInstance observation";
      }
      result.local_world_ready =
          result.ai_subsystem_initialized &&
          selected->position_rooms_in_bounds &&
          result.local_vehicle.chain_consistent &&
          result.local_vehicle.owner_matches_local &&
          result.local_vehicle.physics_available &&
          result.local_vehicle.physics_reverse_matches &&
          result.local_vehicle.position_finite &&
          result.local_vehicle.orientation_valid &&
          result.local_vehicle.current_room_resolved;
    }
  }

  result.status = AiPlayerPoolStatus::snapshot;
  if (result.local_player_valid) {
    result.detail =
        "consistent read-only AI list; local PlayerId identity validated (world/room readiness is not implied)";
  } else if (local_player_id == 0U || local_player_id == sentinel) {
    result.detail = "consistent read-only AI list; no local PlayerId selected";
  } else {
    result.detail =
        "consistent read-only AI list; selected local PlayerId failed type/control validation";
  }
  return result;
}

std::string_view ToString(AiPlayerPoolStatus value) noexcept {
  switch (value) {
    case AiPlayerPoolStatus::snapshot:
      return "snapshot";
    case AiPlayerPoolStatus::not_ready:
      return "not-ready";
    case AiPlayerPoolStatus::read_failed:
      return "read-failed";
    case AiPlayerPoolStatus::corrupt_list:
      return "corrupt-list";
    case AiPlayerPoolStatus::changed_during_read:
      return "changed-during-read";
  }
  return "unknown";
}

}  // namespace ht2mp::game
