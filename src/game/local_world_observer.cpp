#include "ht2mp/game/local_world_observer.hpp"

#include "ht2mp/protocol/pose_math.hpp"
#include "ht2mp/protocol/vehicle_catalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>

namespace ht2mp::game {
namespace {

struct ListNode {
  std::uint32_t next{};
  std::uint32_t previous{};
};

struct WorldCells {
  std::uint32_t room_begin{};
  std::uint32_t room_end{};
  std::uint32_t sentinel{};
  std::uint32_t local_player{};
  std::uint32_t viewer{};
  std::uint8_t initialized{};

  friend bool operator==(const WorldCells&, const WorldCells&) = default;
};

bool IsPlausiblePointer(const std::uint32_t value) noexcept {
  return value >= 0x00010000U && value < 0x80000000U &&
         (value & 0x3U) == 0U;
}

bool CheckedAdd(const std::uint32_t base, const std::uint32_t offset,
                std::uint32_t& result) noexcept {
  if (base > std::numeric_limits<std::uint32_t>::max() - offset) return false;
  result = base + offset;
  return IsPlausiblePointer(result);
}

std::uint32_t DecodeU32(const std::span<const std::byte> bytes,
                        const std::size_t offset) noexcept {
  const auto* data =
      reinterpret_cast<const std::uint8_t*>(bytes.data() + offset);
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8U) |
         (static_cast<std::uint32_t>(data[2]) << 16U) |
         (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::int32_t DecodeI32(const std::span<const std::byte> bytes,
                       const std::size_t offset) noexcept {
  return static_cast<std::int32_t>(DecodeU32(bytes, offset));
}

double DecodeF64(const std::span<const std::byte> bytes,
                 const std::size_t offset) noexcept {
  const auto low = static_cast<std::uint64_t>(DecodeU32(bytes, offset));
  const auto high =
      static_cast<std::uint64_t>(DecodeU32(bytes, offset + 4U));
  const auto bits = low | (high << 32U);
  double value{};
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool ReadU32(ReadOnlyMemory& memory, const std::uintptr_t address,
             std::uint32_t& value) noexcept {
  std::array<std::byte, 4> bytes{};
  if (!memory.ReadFast(address, bytes)) return false;
  value = DecodeU32(bytes, 0U);
  return true;
}

bool ReadNode(ReadOnlyMemory& memory, const std::uint32_t address,
              ListNode& node) noexcept {
  if (!IsPlausiblePointer(address)) return false;
  std::array<std::byte, 8> bytes{};
  if (!memory.ReadFast(address, bytes)) return false;
  node.next = DecodeU32(bytes, 0U);
  node.previous = DecodeU32(bytes, 4U);
  return true;
}

bool ReadCells(ReadOnlyMemory& memory, const std::uintptr_t room_begin_cell,
               const std::uintptr_t room_end_cell,
               const std::uintptr_t initialized_cell,
               const std::uintptr_t sentinel_cell,
               const std::uintptr_t local_cell,
               const std::uintptr_t viewer_cell,
               WorldCells& cells) noexcept {
  std::array<std::byte, 1> initialized{};
  if (!ReadU32(memory, room_begin_cell, cells.room_begin) ||
      !ReadU32(memory, room_end_cell, cells.room_end) ||
      !memory.ReadFast(initialized_cell, initialized) ||
      !ReadU32(memory, sentinel_cell, cells.sentinel) ||
      !ReadU32(memory, local_cell, cells.local_player) ||
      !ReadU32(memory, viewer_cell, cells.viewer)) {
    return false;
  }
  cells.initialized = static_cast<std::uint8_t>(initialized[0]);
  return true;
}

bool RoomCount(const WorldCells& cells, const std::uint32_t maximum,
               std::uint32_t& count) noexcept {
  count = 0U;
  if (cells.room_begin == 0U && cells.room_end == 0U) return true;
  if (!IsPlausiblePointer(cells.room_begin) ||
      !IsPlausiblePointer(cells.room_end) ||
      cells.room_end < cells.room_begin ||
      ((cells.room_end - cells.room_begin) % 4U) != 0U) {
    return false;
  }
  count = (cells.room_end - cells.room_begin) / 4U;
  return count <= maximum;
}

enum class MembershipResult : std::uint8_t { found, missing, read_failed, changed };

MembershipResult VerifyMembership(ReadOnlyMemory& memory,
                                  const std::uint32_t sentinel,
                                  const std::uint32_t local,
                                  const std::uint32_t maximum_nodes,
                                  ListNode& sentinel_before) noexcept {
  if (!ReadNode(memory, sentinel, sentinel_before)) {
    return MembershipResult::read_failed;
  }
  if (!IsPlausiblePointer(sentinel_before.next) ||
      !IsPlausiblePointer(sentinel_before.previous)) {
    return MembershipResult::changed;
  }
  auto current = sentinel_before.next;
  auto expected_previous = sentinel;
  bool found{};
  std::uint32_t visited{};
  while (current != sentinel) {
    if (++visited > maximum_nodes || !IsPlausiblePointer(current)) {
      return MembershipResult::changed;
    }
    ListNode node;
    if (!ReadNode(memory, current, node)) return MembershipResult::read_failed;
    if (node.previous != expected_previous || !IsPlausiblePointer(node.next)) {
      return MembershipResult::changed;
    }
    if (current == local) found = true;
    expected_previous = current;
    current = node.next;
  }
  if (sentinel_before.previous != expected_previous) {
    return MembershipResult::changed;
  }
  return found ? MembershipResult::found : MembershipResult::missing;
}

bool DecodeInlineName(const std::span<const std::byte> bytes,
                      std::size_t& length) noexcept {
  length = 0U;
  for (const auto value : bytes) {
    const auto character = static_cast<std::uint8_t>(value);
    if (character == 0U) return length != 0U;
    if (character < 0x20U || character == 0x7fU) return false;
    ++length;
  }
  return false;
}

bool RoomNameMatches(ReadOnlyMemory& memory,
                     const std::uint32_t room_registry_begin,
                     const std::uint32_t room_id,
                     const LocalVehicleLayout& layout,
                     const std::span<const std::byte> expected,
                     const std::size_t expected_length,
                     bool& matches) noexcept {
  matches = false;
  if (room_id > (std::numeric_limits<std::uint32_t>::max() -
                 room_registry_begin) /
                    4U) {
    return false;
  }
  std::uint32_t room{};
  if (!ReadU32(memory, room_registry_begin + room_id * 4U, room) ||
      !IsPlausiblePointer(room)) {
    return false;
  }
  const auto header_size = std::max(
      {layout.room_name_begin_offset + 4U, layout.room_name_end_offset + 4U,
       layout.room_name_capacity_offset + 4U, layout.room_id_offset + 4U});
  if (header_size == 0U || header_size > 64U) return false;
  std::array<std::byte, 64> header_storage{};
  auto header = std::span<std::byte>(header_storage).first(header_size);
  if (!memory.ReadFast(room, header)) return false;
  const auto immutable = std::span<const std::byte>(header);
  const auto begin = DecodeU32(immutable, layout.room_name_begin_offset);
  const auto end = DecodeU32(immutable, layout.room_name_end_offset);
  const auto capacity = DecodeU32(immutable, layout.room_name_capacity_offset);
  if (DecodeU32(immutable, layout.room_id_offset) != room_id ||
      !IsPlausiblePointer(begin) || end < begin || capacity < end ||
      end - begin == 0U || end - begin > layout.maximum_room_name_bytes ||
      capacity - begin > 4096U) {
    return false;
  }
  std::array<std::byte, 128> name_storage{};
  const auto size = static_cast<std::size_t>(end - begin);
  if (size > name_storage.size()) return false;
  auto name = std::span<std::byte>(name_storage).first(size);
  if (!memory.ReadFast(begin, name)) return false;
  std::size_t length{};
  if (!DecodeInlineName(std::span<const std::byte>(name), length)) return false;
  matches = length == expected_length &&
            std::equal(name.begin(), name.begin() + length, expected.begin());
  return true;
}

double LengthSquared(const double x, const double y, const double z) noexcept {
  return x * x + y * y + z * z;
}

const ResolvedSymbol* RequiredSymbol(const ProfileVerification& verified,
                                     const std::string_view name) noexcept {
  const auto* symbol = verified.FindSymbol(name);
  return symbol != nullptr && symbol->accepted ? symbol : nullptr;
}

}  // namespace

LocalWorldObserver::LocalWorldObserver(
    const std::uintptr_t sentinel_cell_address,
    const std::uintptr_t local_player_cell_address,
    const std::uintptr_t room_registry_begin_cell_address,
    const std::uintptr_t room_registry_end_cell_address,
    const std::uintptr_t ai_subsystem_initialized_address,
    const std::uintptr_t viewer_instance_cell_address,
    const AiPlayerPoolLayout layout, const bool exact_steam_appearance,
    ReadOnlyMemory& memory) noexcept
    : sentinel_cell_address_(sentinel_cell_address),
      local_player_cell_address_(local_player_cell_address),
      room_registry_begin_cell_address_(room_registry_begin_cell_address),
      room_registry_end_cell_address_(room_registry_end_cell_address),
      ai_subsystem_initialized_address_(ai_subsystem_initialized_address),
      viewer_instance_cell_address_(viewer_instance_cell_address),
      layout_(layout),
      exact_steam_appearance_(exact_steam_appearance),
      memory_(&memory) {}

std::unique_ptr<LocalWorldObserver> LocalWorldObserver::Create(
    const ProfileVerification& verified, const std::uintptr_t module_base,
    ReadOnlyMemory& memory, std::string& error) {
  error.clear();
  if (!verified.accepted() || verified.profile == nullptr) {
    error = "game profile has not passed fail-closed verification";
    return nullptr;
  }
  const auto& layout = verified.profile->ai_player_pool;
  const auto& vehicle = layout.local_vehicle;
  if (!layout.observer_enabled || !vehicle.observer_enabled ||
      layout.node_next_offset != 0U || layout.node_previous_offset != 4U ||
      layout.node_player_offset != 8U || layout.maximum_nodes == 0U ||
      layout.maximum_nodes > 4096U || layout.maximum_rooms == 0U ||
      layout.maximum_rooms > 65536U || layout.player_position_size != 32U ||
      layout.maximum_world_location_id == 0U ||
      layout.maximum_world_location_id >
          static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) ||
      layout.maximum_road_distance <= 0.0 ||
      layout.constructor_liveness_marker == 0U ||
      layout.player_self_id_offset == 0U ||
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
      layout.maximum_vehicle_model_selector >
          std::numeric_limits<std::uint16_t>::max() ||
      layout.maximum_vehicle_paint_variant != 3U ||
      vehicle.maximum_room_name_bytes == 0U ||
      vehicle.maximum_room_name_bytes > 128U ||
      vehicle.vehicle_orientation_offset >= vehicle.vehicle_position_offset ||
      vehicle.vehicle_transform_read_size == 0U ||
      vehicle.vehicle_transform_read_size > 256U ||
      vehicle.vehicle_simulation_orientation_offset == 0U ||
      vehicle.vehicle_simulation_position_offset <=
          vehicle.vehicle_simulation_orientation_offset ||
      vehicle.vehicle_simulation_position_offset -
                  vehicle.vehicle_simulation_orientation_offset !=
              9U * sizeof(float) ||
      vehicle.vehicle_moving_item_offset == 0U ||
      vehicle.vehicle_moving_item_offset > 0x10000U ||
      vehicle.physics_vehicle_reverse_offset == 0U ||
      vehicle.physics_vehicle_reverse_offset > 0x100000U ||
      vehicle.physics_orientation_offset == 0U ||
      vehicle.physics_position_offset <= vehicle.physics_orientation_offset ||
      vehicle.physics_orientation_offset > 0x10000U - 9U * sizeof(float) ||
      vehicle.physics_position_offset > 0x10000U - 3U * sizeof(float) ||
      vehicle.physics_body_to_world_orientation_offset == 0U ||
      vehicle.physics_body_to_world_orientation_offset >
          0x10000U - 9U * sizeof(float) ||
      vehicle.physics_world_to_body_orientation_offset == 0U ||
      vehicle.physics_world_to_body_orientation_offset >
          0x10000U - 9U * sizeof(float) ||
      vehicle.physics_body_to_world_orientation_offset ==
          vehicle.physics_world_to_body_orientation_offset ||
      vehicle.vehicle_position_offset - vehicle.vehicle_orientation_offset +
              3U * sizeof(float) >
          vehicle.vehicle_transform_read_size ||
      vehicle.maximum_absolute_position <= 0.0F ||
      vehicle.basis_length_tolerance <= 0.0F ||
      vehicle.basis_dot_tolerance <= 0.0F) {
    error = "profile contains an unsupported fast local-world layout";
    return nullptr;
  }

  const auto* sentinel = RequiredSymbol(verified, layout.sentinel_cell_symbol);
  const auto* local = RequiredSymbol(verified, layout.local_player_cell_symbol);
  const auto* room_begin =
      RequiredSymbol(verified, layout.room_registry_begin_cell_symbol);
  const auto* room_end =
      RequiredSymbol(verified, layout.room_registry_end_cell_symbol);
  const auto* initialized =
      RequiredSymbol(verified, layout.ai_subsystem_initialized_symbol);
  const auto* viewer = RequiredSymbol(
      verified, layout.local_vehicle.viewer_instance_cell_symbol);
  if (sentinel == nullptr || local == nullptr || room_begin == nullptr ||
      room_end == nullptr || initialized == nullptr || viewer == nullptr) {
    error = "verified local-world symbols are unavailable";
    return nullptr;
  }
  const auto maximum_rva =
      std::numeric_limits<std::uintptr_t>::max() - module_base;
  if (sentinel->result_rva > maximum_rva || local->result_rva > maximum_rva ||
      room_begin->result_rva > maximum_rva ||
      room_end->result_rva > maximum_rva ||
      initialized->result_rva > maximum_rva || viewer->result_rva > maximum_rva) {
    error = "runtime local-world address overflows uintptr_t";
    return nullptr;
  }
  return std::unique_ptr<LocalWorldObserver>(new LocalWorldObserver(
      module_base + sentinel->result_rva, module_base + local->result_rva,
      module_base + room_begin->result_rva, module_base + room_end->result_rva,
      module_base + initialized->result_rva, module_base + viewer->result_rva,
      layout, verified.profile->edition == GameEdition::steam, memory));
}

FastLocalWorldObservation LocalWorldObserver::PollFast(
    const std::uint64_t monotonic_time_us) noexcept {
  FastLocalWorldObservation result;
  const auto fail = [this, &result](const LocalWorldObservationStatus status) {
    result.status = status;
    ResetHistory();
    return result;
  };

  WorldCells before;
  if (!ReadCells(*memory_, room_registry_begin_cell_address_,
                 room_registry_end_cell_address_,
                 ai_subsystem_initialized_address_, sentinel_cell_address_,
                 local_player_cell_address_, viewer_instance_cell_address_,
                 before)) {
    return fail(LocalWorldObservationStatus::read_failed);
  }
  std::uint32_t room_count{};
  if (!RoomCount(before, layout_.maximum_rooms, room_count)) {
    return fail(LocalWorldObservationStatus::invalid_state);
  }
  if (before.initialized == 0U || room_count == 0U ||
      !IsPlausiblePointer(before.sentinel) ||
      !IsPlausiblePointer(before.local_player) ||
      before.local_player == before.sentinel ||
      !IsPlausiblePointer(before.viewer)) {
    return fail(LocalWorldObservationStatus::not_ready);
  }

  ListNode sentinel_before;
  switch (VerifyMembership(*memory_, before.sentinel, before.local_player,
                           layout_.maximum_nodes, sentinel_before)) {
    case MembershipResult::found:
      break;
    case MembershipResult::missing:
      return fail(LocalWorldObservationStatus::not_ready);
    case MembershipResult::read_failed:
      return fail(LocalWorldObservationStatus::read_failed);
    case MembershipResult::changed:
      return fail(LocalWorldObservationStatus::changed_during_read);
  }

  const auto player_size = std::max(
      {layout_.player_position_offset + layout_.player_position_size,
       layout_.player_vehicle_id_offset + 4U,
       layout_.player_vehicle_descriptor_offset + 4U,
       layout_.player_actor_type_offset + 4U,
       layout_.player_flags_offset + 4U,
       layout_.player_self_id_offset + 4U,
       layout_.player_position_room_a_offset + 4U,
       layout_.player_position_room_b_offset + 4U});
  if (player_size > 1024U ||
      before.local_player > std::numeric_limits<std::uint32_t>::max() -
                                layout_.node_player_offset) {
    return fail(LocalWorldObservationStatus::invalid_state);
  }
  const auto player_address = before.local_player + layout_.node_player_offset;
  std::array<std::byte, 1024> player_storage{};
  auto player_bytes = std::span<std::byte>(player_storage).first(player_size);
  if (!memory_->ReadFast(player_address, player_bytes)) {
    return fail(LocalWorldObservationStatus::read_failed);
  }
  const auto player = std::span<const std::byte>(player_bytes);
  const auto actor_type = DecodeI32(player, layout_.player_actor_type_offset);
  const auto flags = DecodeU32(player, layout_.player_flags_offset);
  const auto liveness =
      DecodeU32(player, layout_.player_liveness_marker_offset);
  const auto self_player_id =
      DecodeU32(player, layout_.player_self_id_offset);
  const auto room_a = DecodeI32(player, layout_.player_position_room_a_offset);
  const auto room_b = DecodeI32(player, layout_.player_position_room_b_offset);
  const auto vehicle_id = DecodeU32(player, layout_.player_vehicle_id_offset);
  const auto vehicle_descriptor =
      DecodeU32(player, layout_.player_vehicle_descriptor_offset);
  const auto valid_room = [room_count](const std::int32_t room) {
    return room == -1 ||
           (room >= 0 && static_cast<std::uint32_t>(room) < room_count);
  };
  if (actor_type != layout_.required_local_actor_type ||
      liveness != layout_.constructor_liveness_marker ||
      self_player_id != before.local_player ||
      (flags & layout_.x_controlled_flag_mask) != 0U ||
      !valid_room(room_a) || !valid_room(room_b) || vehicle_id == 0U ||
      vehicle_id == UINT32_MAX || !IsPlausiblePointer(vehicle_descriptor)) {
    return fail(LocalWorldObservationStatus::not_ready);
  }

  const auto descriptor_size =
      std::max({layout_.vehicle_descriptor_vehicle_id_offset,
                layout_.vehicle_descriptor_model_selector_offset,
                layout_.vehicle_descriptor_paint_variant_offset}) + 4U;
  std::array<std::byte, 64> descriptor_storage{};
  auto descriptor_bytes =
      std::span<std::byte>(descriptor_storage).first(descriptor_size);
  if (!memory_->ReadFast(vehicle_descriptor, descriptor_bytes)) {
    return fail(LocalWorldObservationStatus::read_failed);
  }
  const auto descriptor = std::span<const std::byte>(descriptor_bytes);
  const auto descriptor_vehicle_id =
      DecodeU32(descriptor, layout_.vehicle_descriptor_vehicle_id_offset);
  const auto model_selector =
      DecodeU32(descriptor, layout_.vehicle_descriptor_model_selector_offset);
  const auto descriptor_paint_field =
      DecodeU32(descriptor, layout_.vehicle_descriptor_paint_variant_offset);
  if (descriptor_vehicle_id != vehicle_id ||
      model_selector > layout_.maximum_vehicle_model_selector ||
      descriptor_paint_field > layout_.maximum_vehicle_paint_variant) {
    return fail(LocalWorldObservationStatus::not_ready);
  }
  std::uint16_t network_vehicle = static_cast<std::uint16_t>(model_selector);
  std::uint8_t network_paint =
      static_cast<std::uint8_t>(descriptor_paint_field);
  if (exact_steam_appearance_) {
    const auto* vehicle =
        ht2mp::protocol::find_steam_vehicle_by_native_selector(
            static_cast<std::uint16_t>(model_selector), network_paint);
    if (vehicle == nullptr) {
      return fail(LocalWorldObservationStatus::not_ready);
    }
    network_vehicle = vehicle->selector;
  }

  const auto position_id =
      player.subspan(layout_.player_position_offset,
                     layout_.player_position_size);
  const auto road_id = DecodeI32(position_id, 0U);
  const auto node_id = DecodeI32(position_id, 4U);
  const auto road_distance = DecodeF64(position_id, 8U);
  const auto road_segment_vector_id = DecodeI32(position_id, 16U);
  const auto road_segment_id = DecodeI32(position_id, 20U);
  const auto aux0 = DecodeI32(position_id, 24U);
  const auto aux1 = DecodeI32(position_id, 28U);
  const auto valid_optional_location = [this](const std::int32_t value) {
    return value == -1 ||
           (value >= 0 && static_cast<std::uint32_t>(value) <=
                              layout_.maximum_world_location_id);
  };
  if (road_id < 0 ||
      static_cast<std::uint32_t>(road_id) >
          layout_.maximum_world_location_id ||
      !valid_optional_location(node_id) || road_segment_vector_id < 0 ||
      static_cast<std::uint32_t>(road_segment_vector_id) >
          layout_.maximum_world_location_id ||
      road_segment_id < 0 ||
      static_cast<std::uint32_t>(road_segment_id) >
          layout_.maximum_world_location_id ||
      !std::isfinite(road_distance) ||
      std::abs(road_distance) > layout_.maximum_road_distance) {
    return fail(LocalWorldObservationStatus::invalid_state);
  }

  std::uint32_t vehicle_pointer_address{};
  if (!CheckedAdd(before.viewer, layout_.local_vehicle.viewer_vehicle_offset,
                  vehicle_pointer_address)) {
    return fail(LocalWorldObservationStatus::invalid_state);
  }
  std::uint32_t vehicle{};
  if (!ReadU32(*memory_, vehicle_pointer_address, vehicle)) {
    return fail(LocalWorldObservationStatus::read_failed);
  }
  if (!IsPlausiblePointer(vehicle)) {
    return fail(LocalWorldObservationStatus::not_ready);
  }

  const auto& vehicle_layout = layout_.local_vehicle;
  std::uint32_t owner_address{};
  std::uint32_t transform_address{};
  std::uint32_t room_name_address{};
  std::uint32_t physics_address{};
  std::uint32_t moving_item{};
  if (!CheckedAdd(vehicle, vehicle_layout.vehicle_owner_player_id_offset,
                  owner_address) ||
      !CheckedAdd(vehicle, vehicle_layout.vehicle_orientation_offset,
                  transform_address) ||
      !CheckedAdd(vehicle, vehicle_layout.vehicle_current_room_name_offset,
                  room_name_address) ||
      !CheckedAdd(vehicle, vehicle_layout.vehicle_physics_offset,
                  physics_address) ||
      !CheckedAdd(vehicle, vehicle_layout.vehicle_moving_item_offset,
                  moving_item)) {
    return fail(LocalWorldObservationStatus::invalid_state);
  }
  std::uint32_t owner{};
  std::uint32_t physics{};
  std::array<std::byte, 256> transform_storage{};
  auto transform_bytes = std::span<std::byte>(transform_storage)
                             .first(vehicle_layout.vehicle_transform_read_size);
  std::array<std::byte, 256> simulation_storage{};
  auto simulation_bytes = std::span<std::byte>(simulation_storage)
                              .first(vehicle_layout.vehicle_transform_read_size);
  std::array<std::byte, 128> vehicle_room_storage{};
  auto vehicle_room = std::span<std::byte>(vehicle_room_storage)
                          .first(vehicle_layout.maximum_room_name_bytes);
  if (!ReadU32(*memory_, owner_address, owner) ||
      !memory_->ReadFast(transform_address, transform_bytes) ||
      !memory_->ReadFast(room_name_address, vehicle_room) ||
      !ReadU32(*memory_, physics_address, physics)) {
    return fail(LocalWorldObservationStatus::read_failed);
  }
  // The simulation copy shares the render copy's layout (basis then Vec3f at
  // the same relative offset). A failed read only disables the preference.
  std::uint32_t simulation_address{};
  const bool simulation_available =
      vehicle_layout.vehicle_simulation_orientation_offset != 0U &&
      vehicle_layout.vehicle_simulation_position_offset -
              vehicle_layout.vehicle_simulation_orientation_offset ==
          vehicle_layout.vehicle_position_offset -
              vehicle_layout.vehicle_orientation_offset &&
      CheckedAdd(vehicle, vehicle_layout.vehicle_simulation_orientation_offset,
                 simulation_address) &&
      memory_->ReadFast(simulation_address, simulation_bytes);
  if (owner != before.local_player || !IsPlausiblePointer(physics)) {
    return fail(LocalWorldObservationStatus::not_ready);
  }
  std::uint32_t physics_reverse_address{};
  std::uint32_t physics_reverse{};
  if (!CheckedAdd(physics, vehicle_layout.physics_vehicle_reverse_offset,
                  physics_reverse_address)) {
    return fail(LocalWorldObservationStatus::invalid_state);
  }
  if (!ReadU32(*memory_, physics_reverse_address, physics_reverse)) {
    return fail(LocalWorldObservationStatus::read_failed);
  }
  if (physics_reverse != moving_item) {
    return fail(LocalWorldObservationStatus::not_ready);
  }
  std::size_t vehicle_room_length{};
  if (!DecodeInlineName(std::span<const std::byte>(vehicle_room),
                        vehicle_room_length)) {
    return fail(LocalWorldObservationStatus::invalid_state);
  }

  const RigidTransformLayout transform_layout{
      0U,
      vehicle_layout.vehicle_position_offset -
          vehicle_layout.vehicle_orientation_offset,
      vehicle_layout.vehicle_transform_read_size,
      vehicle_layout.maximum_absolute_position,
      vehicle_layout.basis_length_tolerance,
      vehicle_layout.basis_dot_tolerance,
  };
  const auto transform_status = DecodeRigidTransformFast(
      std::span<const std::byte>(transform_bytes), transform_layout,
      result.sample.transform);
  if (transform_status != ObservationStatus::sample) {
    return fail(transform_status == ObservationStatus::not_ready
                    ? LocalWorldObservationStatus::not_ready
                    : LocalWorldObservationStatus::invalid_state);
  }
  if (simulation_available) {
    LocalTransformSample simulation_transform;
    if (DecodeRigidTransformFast(std::span<const std::byte>(simulation_bytes),
                                 transform_layout, simulation_transform) ==
        ObservationStatus::sample) {
      result.sample.transform = simulation_transform;
      result.sample.transform_from_simulation_copy = true;
    }
  }

  std::int32_t current_room{-1};
  std::uint32_t matches{};
  const std::array<std::int32_t, 2> endpoint_rooms{room_a, room_b};
  for (std::size_t index = 0; index < endpoint_rooms.size(); ++index) {
    const auto candidate = endpoint_rooms[index];
    if (candidate < 0 ||
        (index != 0U && endpoint_rooms[0] == candidate)) {
      continue;
    }
    bool name_matches{};
    if (!RoomNameMatches(*memory_, before.room_begin,
                         static_cast<std::uint32_t>(candidate), vehicle_layout,
                         std::span<const std::byte>(vehicle_room),
                         vehicle_room_length, name_matches)) {
      return fail(LocalWorldObservationStatus::read_failed);
    }
    if (name_matches) {
      ++matches;
      current_room = candidate;
    }
  }
  if (matches != 1U) return fail(LocalWorldObservationStatus::not_ready);

  WorldCells after;
  ListNode sentinel_after;
  std::uint32_t vehicle_after{};
  std::uint32_t owner_after{};
  std::uint32_t physics_after{};
  std::uint32_t physics_reverse_after{};
  std::array<std::byte, 64> descriptor_after_storage{};
  auto descriptor_after =
      std::span<std::byte>(descriptor_after_storage).first(descriptor_size);
  std::array<std::byte, 1024> player_after_storage{};
  auto player_after =
      std::span<std::byte>(player_after_storage).first(player_size);
  std::array<std::byte, 128> room_after_storage{};
  auto room_after = std::span<std::byte>(room_after_storage)
                        .first(vehicle_layout.maximum_room_name_bytes);
  if (!ReadCells(*memory_, room_registry_begin_cell_address_,
                 room_registry_end_cell_address_,
                 ai_subsystem_initialized_address_, sentinel_cell_address_,
                 local_player_cell_address_, viewer_instance_cell_address_,
                 after) ||
      !ReadNode(*memory_, before.sentinel, sentinel_after) ||
      !ReadU32(*memory_, vehicle_pointer_address, vehicle_after) ||
      !ReadU32(*memory_, owner_address, owner_after) ||
      !ReadU32(*memory_, physics_address, physics_after) ||
      !ReadU32(*memory_, physics_reverse_address, physics_reverse_after) ||
      !memory_->ReadFast(vehicle_descriptor, descriptor_after) ||
      !memory_->ReadFast(player_address, player_after) ||
      !memory_->ReadFast(room_name_address, room_after)) {
    return fail(LocalWorldObservationStatus::read_failed);
  }
  if (after != before || sentinel_after.next != sentinel_before.next ||
      sentinel_after.previous != sentinel_before.previous ||
      vehicle_after != vehicle || owner_after != owner ||
      physics_after != physics || physics_reverse_after != physics_reverse ||
      !std::equal(player_bytes.begin(), player_bytes.end(),
                  player_after.begin()) ||
      !std::equal(descriptor_bytes.begin(), descriptor_bytes.end(),
                  descriptor_after.begin()) ||
      !std::equal(vehicle_room.begin(), vehicle_room.end(),
                  room_after.begin())) {
    return fail(LocalWorldObservationStatus::changed_during_read);
  }

  result.sample.transform.sequence = ++sequence_;
  result.sample.transform.monotonic_time_us = monotonic_time_us;
  result.sample.current_room_id = current_room;
  result.sample.endpoint_room_a = room_a;
  result.sample.endpoint_room_b = room_b;
  result.sample.vehicle_id = static_cast<std::int32_t>(vehicle_id);
  result.sample.vehicle_instance_address = vehicle;
  result.sample.vehicle_type = network_vehicle;
  result.sample.paint_variant = network_paint;
  result.sample.road_id = road_id;
  result.sample.node_id = node_id;
  result.sample.road_distance = road_distance;
  result.sample.road_segment_vector_id = road_segment_vector_id;
  result.sample.road_segment_id = road_segment_id;
  result.sample.aux0 = aux0;
  result.sample.aux1 = aux1;
  std::memcpy(result.sample.raw_position_id.data(),
              player.data() + layout_.player_position_offset,
              result.sample.raw_position_id.size());

  if (have_previous_ && monotonic_time_us > previous_time_us_) {
    const auto elapsed_us = monotonic_time_us - previous_time_us_;
    const auto elapsed_seconds = static_cast<double>(elapsed_us) / 1'000'000.0;
    const auto dx = result.sample.transform.position.x - previous_position_.x;
    const auto dy = result.sample.transform.position.y - previous_position_.y;
    const auto dz = result.sample.transform.position.z - previous_position_.z;
    const auto distance = std::sqrt(LengthSquared(dx, dy, dz));
    result.sample.transform.teleport =
        current_room != previous_room_id_ || distance > 250.0;
    if (elapsed_seconds >= 0.001 && elapsed_seconds <= 1.0 &&
        !result.sample.transform.teleport) {
      result.sample.transform.linear_velocity = {
          dx / elapsed_seconds, dy / elapsed_seconds, dz / elapsed_seconds};
      result.sample.transform.velocity_valid = true;
      const auto& current = result.sample.transform.orientation;
      posemath::Vec3f omega{};
      if (posemath::body_angular_velocity(
              {previous_orientation_.x, previous_orientation_.y,
               previous_orientation_.z, previous_orientation_.w},
              {current.x, current.y, current.z, current.w}, elapsed_seconds,
              omega)) {
        const double magnitude =
            std::sqrt(static_cast<double>(omega[0]) * omega[0] +
                      static_cast<double>(omega[1]) * omega[1] +
                      static_cast<double>(omega[2]) * omega[2]);
        // Faster than 100 rad/s is a transform reset, not a vehicle turning.
        if (magnitude <= 100.0) {
          result.sample.transform.angular_velocity = {omega[0], omega[1],
                                                      omega[2]};
          result.sample.transform.angular_velocity_valid = true;
        }
      }
    }
  }
  previous_time_us_ = monotonic_time_us;
  previous_position_ = result.sample.transform.position;
  previous_orientation_ = result.sample.transform.orientation;
  previous_room_id_ = current_room;
  have_previous_ = true;
  result.status = LocalWorldObservationStatus::sample;
  return result;
}

void LocalWorldObserver::ResetHistory() noexcept {
  previous_time_us_ = 0U;
  previous_position_ = {};
  previous_orientation_ = {};
  previous_room_id_ = -1;
  have_previous_ = false;
}

std::string_view ToString(const LocalWorldObservationStatus value) noexcept {
  switch (value) {
    case LocalWorldObservationStatus::sample:
      return "sample";
    case LocalWorldObservationStatus::not_ready:
      return "not-ready";
    case LocalWorldObservationStatus::read_failed:
      return "read-failed";
    case LocalWorldObservationStatus::invalid_state:
      return "invalid-state";
    case LocalWorldObservationStatus::changed_during_read:
      return "changed-during-read";
  }
  return "unknown";
}

}  // namespace ht2mp::game
