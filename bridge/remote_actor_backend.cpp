#include "remote_actor_backend.hpp"

#include "hook_thread_guard.hpp"
#include "online_world.hpp"
#include "remote_actor_registry.hpp"
#include "remote_command_queue.hpp"
#include "remote_pose_timeline.hpp"
#include "runtime_validation.hpp"
#include "telemetry.hpp"
#include "replication.hpp"
#include "wheel_animation.hpp"
#include "horn.hpp"

#include "ht2mp/protocol/pose_math.hpp"
#include "ht2mp/protocol/vehicle_catalog.hpp"

#include "ht2mp/game/pattern.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace ht2mp::bridge {
namespace {

using CreateNewPlayer = void*(__cdecl*)(std::int32_t, std::int32_t,
                                         std::int32_t);
using AcquirePlayerId = std::uint32_t*(__cdecl*)(std::uint32_t*, void*);
using AnnouncePlayer = void(__cdecl*)(std::uint32_t);
using ProcessMove = void(__cdecl*)(std::uint32_t);
using SetPositionId = void(__thiscall*)(void*, const void*);
using KillPlayer = void(__cdecl*)(std::uint32_t);
using VehicleConstructor = void*(__thiscall*)(void*, std::uint32_t,
                                               std::uint32_t, std::uint32_t,
                                               std::uint32_t);
using MovingItemResolver = void*(__cdecl*)(std::uint32_t);
using MovingItemUpdate = void(__thiscall*)(void*, std::uint32_t,
                                           std::uint32_t);
using VehicleRenderHistoryUpdate = void(__thiscall*)(void*);
using VehicleSetMode = void(__thiscall*)(void*, int);
using SceneTransformGetWorld = int(__thiscall*)(void*, void*);
using VehicleGroundEffects = void(__thiscall*)(void*);
using PairCollision = void(__cdecl*)(void*, void*, std::uint32_t, float);
using HitPlayer = void(__cdecl*)(std::uint32_t, double, int);
using PreRegistryReset = void(__cdecl*)();
using PreSave = void(__cdecl*)(void*);

struct PositionId final {
  std::int32_t road_id{};
  std::int32_t node_id{};
  double road_distance{};
  std::int32_t road_segment_vector_id{};
  std::int32_t road_segment_id{};
  std::int32_t aux0{};
  std::int32_t aux1{};
};
static_assert(sizeof(PositionId) == 32U);
static_assert(offsetof(PositionId, road_distance) == 8U);

// The pose most recently evaluated on the game thread. Steam advances it at
// the vehicle history boundary as well as at the slower post-AI boundary.
struct DisplayedPose final {
  ht2mp::protocol::WheelSpeeds wheels{};
  std::array<float, 9> matrix{1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                              0.0F, 0.0F, 0.0F, 1.0F};
  std::array<float, 9> inverse{1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                               0.0F, 0.0F, 0.0F, 1.0F};
  std::array<float, 3> position{};
};

struct RemoteSlot final {
  RemoteActorKey key{};
  std::uint16_t vehicle_type{};
  std::uint8_t paint_variant{};
  bool occupied{};
  bool has_state{};
  // Newest authoritative sample: logical location, flags and model. The
  // transform written to the game comes from `displayed`, never from here.
  ht2mp::ipc::PlayerSampleV1 state{};
  RemotePoseTimeline timeline{};
  DisplayedPose displayed{};
  WheelAnimation wheel_animation{};
  bool has_displayed{};
  // Only the post-AI owner may arm frame writes, after validating the binding
  // and initializing native physics. Disarmed before any lifecycle operation.
  bool frame_write_ready{};
  std::uint64_t last_frame_us{};
  std::uint32_t applied_sequence{};
  bool has_applied_sequence{};
  PositionId logical_position{};
  bool has_logical_position{};
  std::uint32_t ai_player{};
  std::uint32_t game_player_id{};
  std::uint32_t vehicle_candidate{};
  std::uint32_t vehicle_instance{};
  std::uint32_t physics{};
  std::uint32_t scene_node{};
  std::uint32_t registry_generation{};
  ULONGLONG retry_after_ms{};
  std::array<float, 3> last_commanded_position{};
  bool has_last_commanded_position{};
  // World position at the last native setPositionId call: the logical
  // location is refreshed by travelled metres, not by the road fraction.
  std::array<double, 3> logical_refresh_position{};
  // Velocity to write next to the transform (world-frame linear from the
  // playback sample, body-frame angular), zero while held.
  std::array<float, 3> displayed_linear_velocity{};
  std::array<float, 3> displayed_angular_velocity{};
};

RemoteCommandQueue g_commands;
// Drained on the game thread only; kept off the stack because the batch holds
// every buffered state of every player.
RemoteCommandBatch g_batch;
RemoteActorRegistry g_registry;
RemoteCollisionIndex g_collision_index;
std::array<RemoteSlot, kMaximumRemotePlayers> g_slots{};
std::array<std::atomic<std::uint32_t>, kMaximumRemotePlayers> g_player_ids{};
std::array<std::atomic<std::uint32_t>, kMaximumRemotePlayers> g_scene_nodes{};
ht2mp::game::AiPlayerPoolLayout g_ai_layout{};
GogRemoteActorPlan g_plan{};

CreateNewPlayer g_create_new_player{};
AcquirePlayerId g_acquire_player_id{};
AnnouncePlayer g_announce_player{};
SetPositionId g_set_position_id{};
KillPlayer g_kill_player{};
MovingItemResolver g_moving_item_resolver{};
MovingItemUpdate g_original_moving_item_update{};
VehicleRenderHistoryUpdate g_original_vehicle_render_history_update{};
VehicleSetMode g_vehicle_set_mode{};
SceneTransformGetWorld g_original_scene_transform_get_world{};
VehicleGroundEffects g_original_vehicle_ground_effects{};
ProcessMove g_original_process_move{};
VehicleConstructor g_original_vehicle_constructor{};
PairCollision g_original_pair_collision{};
HitPlayer g_original_hit_player{};
PreRegistryReset g_original_pre_registry_reset{};
PreSave g_original_pre_save{};

volatile LONG g_ready{};
volatile LONG g_safe_mode_requested{};
volatile LONG g_game_thread_id{};
volatile LONG g_thread_mismatches{};
volatile LONG g_spawns{};
volatile LONG g_despawns{};
volatile LONG g_applied_samples{};
volatile LONG g_suppressed_moves{};
volatile LONG g_suppressed_physics_updates{};
volatile LONG g_corrected_render_updates{};
volatile LONG g_frame_max_gap_us{};
volatile LONG g_scene_reads{};
volatile LONG g_scene_position_error_bits{};
volatile LONG g_scene_orientation_error_bits{};
volatile LONG g_shadow_updates{};
volatile LONG g_shadow_position_error_bits{};
volatile LONG g_shadow_orientation_error_bits{};
volatile LONG g_suppressed_collisions{};
volatile LONG g_suppressed_hits{};
volatile LONG g_validation_failures{};
volatile LONG g_active_slots{};
volatile LONG g_game_actors{};
volatile LONG g_captured_vehicles{};
volatile LONG g_constructor_vehicle_candidates{};
volatile LONG g_resolver_vehicle_candidates{};
std::array<volatile LONG, 4> g_local_vehicle_constructor_args{};
std::array<volatile LONG, 4> g_remote_vehicle_constructor_args{};
volatile LONG g_requested_vehicle_model{};
volatile LONG g_applied_vehicle_model{};
volatile LONG g_selected_local_vehicle{-1};
volatile LONG g_selected_local_paint{-1};
volatile LONG g_exact_steam_appearance{};
// Remote actors are constructed synchronously inside acquirePlayerId. Keep
// the requested appearance visible to the nested vehicle-constructor hook;
// the stock constructor otherwise reuses the model index carried by the
// template's saved POffroad object.
volatile LONG g_pending_remote_vehicle{-1};
volatile LONG g_pending_remote_paint{-1};
volatile LONG g_first_vehicle_instance{};
volatile LONG g_set_position_calls{};
volatile LONG g_dropped_states{};
volatile LONG g_receive_stamps_replaced{};
volatile LONG g_motion_mode_writes{};
volatile LONG g_motion_mode_failures{};
volatile LONG g_suppressed_set_position{};
// Non-zero while the bridge itself is inside setPositionId on the game
// thread, so the detour lets that one call through to the original.
volatile LONG g_bridge_set_position_depth{};
// Seqlock-published copy of the first active slot's timeline statistics.
volatile LONG g_timeline_epoch{};
PoseTimelineStats g_timeline_stats{};
volatile LONG g_transform_writes{};
volatile LONG g_changed_pose_commands{};
volatile LONG g_pose_sequence{};
volatile LONG g_pose_valid_mask{};
volatile LONG g_pose_epoch{};
std::array<volatile LONG, 3> g_commanded_position_bits{};
std::array<volatile LONG, 3> g_pre_render_position_bits{};
std::array<volatile LONG, 3> g_pre_physics_position_bits{};
std::array<volatile LONG, 3> g_post_render_position_bits{};
std::array<volatile LONG, 3> g_post_physics_position_bits{};
volatile LONG g_pre_render_orientation_error_bits{};
volatile LONG g_pre_physics_orientation_error_bits{};
volatile LONG g_post_render_orientation_error_bits{};
volatile LONG g_post_physics_orientation_error_bits{};

static_assert(sizeof(LONG) == sizeof(float));

bool plausible_pointer(const std::uint32_t value) noexcept {
  return value >= 0x00010000U && value < 0x80000000U &&
         (value & 0x3U) == 0U;
}

bool read_bytes(const std::uint32_t address, void* destination,
                const std::size_t size) noexcept {
  if (!plausible_pointer(address) || destination == nullptr || size == 0U) {
    return false;
  }
  __try {
    std::memcpy(destination, reinterpret_cast<const void*>(address), size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool write_bytes(const std::uint32_t address, const void* source,
                 const std::size_t size) noexcept {
  if (!plausible_pointer(address) || source == nullptr || size == 0U) {
    return false;
  }
  __try {
    std::memcpy(reinterpret_cast<void*>(address), source, size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool read_u32(const std::uint32_t address, std::uint32_t& value) noexcept {
  return read_bytes(address, &value, sizeof(value));
}

bool add_address(std::uint32_t base, std::uint32_t offset,
                 std::uint32_t& result) noexcept;
bool vehicle_model_is_loaded(std::uint16_t selector) noexcept;

constexpr std::uint32_t kLastSteamVehicleTechIndex = 38U;
constexpr std::uint32_t kSavedVehicleTechIndexOffset = 0x8ccU;

// VehicleInstance's exact-Steam constructor does not derive the visible model
// from the AI vehicle descriptor. It reads a vehicle.tech record index from
// the persisted animation object at +0x8cc and immediately builds the body
// from it. The template contains POffroad (index 2), so changing only the
// descriptor after load reports Cayman to the network while leaving a Jeep on
// screen. Rewrite the source index before the original constructor consumes
// it and leave it consistent for later native updates that retain this object.
bool override_constructor_appearance(const std::uint32_t model_object,
                                     const std::uint16_t selector,
                                     const std::uint8_t paint) noexcept {
  ht2mp::protocol::SteamNativeVehicleAppearance native;
  if (!ht2mp::protocol::to_steam_native_vehicle(selector, paint, native) ||
      !vehicle_model_is_loaded(native.selector)) {
    return false;
  }
  std::uint32_t index_address{};
  std::uint32_t previous_index{};
  std::uint32_t readback{};
  const auto target_index = static_cast<std::uint32_t>(
      native.vehicle_tech_index);
  if (!add_address(model_object, kSavedVehicleTechIndexOffset, index_address) ||
      !read_u32(index_address, previous_index) ||
      previous_index > kLastSteamVehicleTechIndex ||
      !write_bytes(index_address, &target_index, sizeof(target_index)) ||
      !read_u32(index_address, readback) || readback != target_index) {
    return false;
  }
  return true;
}

void publish_float(volatile LONG& destination, const float value) noexcept {
  InterlockedExchange(&destination, std::bit_cast<LONG>(value));
}

float load_float(volatile LONG& source) noexcept {
  return std::bit_cast<float>(InterlockedCompareExchange(&source, 0, 0));
}

template <std::size_t Size>
void publish_floats(std::array<volatile LONG, Size>& destination,
                    const std::array<float, Size>& values) noexcept {
  for (std::size_t index = 0U; index < Size; ++index) {
    publish_float(destination[index], values[index]);
  }
}

template <std::size_t Size>
void load_floats(std::array<float, Size>& destination,
                 std::array<volatile LONG, Size>& source) noexcept {
  for (std::size_t index = 0U; index < Size; ++index) {
    destination[index] = load_float(source[index]);
  }
}

float maximum_matrix_error(const std::array<float, 9>& actual,
                           const std::array<float, 9>& expected) noexcept {
  float error{};
  for (std::size_t index = 0U; index < actual.size(); ++index) {
    error = std::max(error, std::abs(actual[index] - expected[index]));
  }
  return error;
}

bool add_address(const std::uint32_t base, const std::uint32_t offset,
                 std::uint32_t& result) noexcept {
  if (base > UINT32_MAX - offset) return false;
  result = base + offset;
  return plausible_pointer(result);
}

bool vehicle_model_is_loaded(const std::uint16_t selector) noexcept {
  if (selector > g_ai_layout.maximum_vehicle_model_selector ||
      g_plan.vehicle_model_registry_begin_cell > UINT32_MAX ||
      g_plan.vehicle_model_registry_end_cell > UINT32_MAX) {
    return false;
  }
  std::uint32_t begin{};
  std::uint32_t end{};
  if (!read_u32(
          static_cast<std::uint32_t>(g_plan.vehicle_model_registry_begin_cell),
          begin) ||
      !read_u32(
          static_cast<std::uint32_t>(g_plan.vehicle_model_registry_end_cell),
          end) ||
      !plausible_pointer(begin) || end <= begin ||
      (end - begin) % sizeof(std::uint32_t) != 0U) {
    return false;
  }
  const auto count = (end - begin) / sizeof(std::uint32_t);
  if (count == 0U ||
      count > g_ai_layout.maximum_vehicle_model_selector + 1U ||
      selector >= count ||
      selector > (UINT32_MAX - begin) / sizeof(std::uint32_t)) {
    return false;
  }
  std::uint32_t model{};
  return read_u32(begin + selector * sizeof(std::uint32_t), model) &&
         plausible_pointer(model);
}

bool read_actor_vehicle_appearance(const std::uint32_t ai,
                                   std::uint32_t& selector,
                                   std::uint32_t& paint) noexcept {
  selector = UINT32_MAX;
  paint = UINT32_MAX;
  std::uint32_t descriptor{};
  std::uint32_t descriptor_address{};
  std::uint32_t selector_address{};
  std::uint32_t paint_address{};
  if (!add_address(ai, g_ai_layout.player_vehicle_descriptor_offset,
                   descriptor_address) ||
      !read_u32(descriptor_address, descriptor) ||
      !plausible_pointer(descriptor) ||
      !add_address(descriptor,
                   g_ai_layout.vehicle_descriptor_model_selector_offset,
                   selector_address) ||
      !add_address(descriptor,
                   g_ai_layout.vehicle_descriptor_paint_variant_offset,
                   paint_address)) {
    return false;
  }
  std::uint32_t native_selector{};
  std::uint32_t descriptor_paint{};
  if (!read_u32(selector_address, native_selector) ||
      !read_u32(paint_address, descriptor_paint)) {
    return false;
  }
  if (InterlockedCompareExchange(&g_exact_steam_appearance, 0, 0) == 0) {
    selector = native_selector;
    paint = descriptor_paint;
    return true;
  }
  if (native_selector > UINT16_MAX) return false;
  std::uint8_t native_paint{};
  const auto* vehicle =
      ht2mp::protocol::find_steam_vehicle_by_native_selector(
          static_cast<std::uint16_t>(native_selector), native_paint);
  if (vehicle == nullptr) return false;
  selector = vehicle->selector;
  paint = native_paint;
  return true;
}

bool force_actor_vehicle_appearance(const std::uint32_t ai,
                                    const std::uint16_t selector,
                                    const std::uint8_t paint) noexcept {
  ht2mp::protocol::SteamNativeVehicleAppearance native;
  const bool exact_steam =
      InterlockedCompareExchange(&g_exact_steam_appearance, 0, 0) != 0;
  if (exact_steam &&
      (!ht2mp::protocol::to_steam_native_vehicle(selector, paint, native) ||
       !vehicle_model_is_loaded(native.selector))) {
    return false;
  }
  if (!exact_steam && (!vehicle_model_is_loaded(selector) || paint > 3U)) {
    return false;
  }
  std::uint32_t descriptor{};
  std::uint32_t descriptor_address{};
  std::uint32_t vehicle_id_address{};
  std::uint32_t selector_address{};
  std::uint32_t paint_address{};
  if (!add_address(ai, g_ai_layout.player_vehicle_descriptor_offset,
                   descriptor_address) ||
      !read_u32(descriptor_address, descriptor) ||
      !plausible_pointer(descriptor) ||
      !add_address(descriptor,
                   g_ai_layout.vehicle_descriptor_vehicle_id_offset,
                   vehicle_id_address) ||
      !add_address(descriptor,
                   g_ai_layout.vehicle_descriptor_model_selector_offset,
                   selector_address) ||
      !add_address(descriptor,
                   g_ai_layout.vehicle_descriptor_paint_variant_offset,
                   paint_address)) {
    return false;
  }
  std::uint32_t vehicle_id{};
  const auto value = exact_steam
      ? static_cast<std::uint32_t>(native.selector)
      : static_cast<std::uint32_t>(selector);
  const auto paint_value = static_cast<std::uint32_t>(paint);
  std::uint32_t readback{};
  std::uint32_t paint_readback{};
  // A freshly linked type-2 actor legitimately carries VehicleId=-1 until
  // acquirePlayerId completes its native accounting. Read the field to prove
  // the descriptor layout, but do not treat that transient sentinel as dead.
  if (!read_u32(vehicle_id_address, vehicle_id) ||
      !write_bytes(selector_address, &value, sizeof(value)) ||
      (!exact_steam &&
       !write_bytes(paint_address, &paint_value, sizeof(paint_value))) ||
      !read_u32(selector_address, readback) || readback != value ||
      !read_u32(paint_address, paint_readback) ||
      (!exact_steam && paint_readback != paint_value)) {
    return false;
  }
  InterlockedExchange(&g_applied_vehicle_model, static_cast<LONG>(selector));
  return true;
}

void* call_create_new_player() noexcept {
  const auto function = g_create_new_player;
  if (function == nullptr) return nullptr;
  __try {
    // Type 2 is the dedicated-AI fallback.  Unlike stock type 8, it is not
    // managed by updateGhosts; processMove is suppressed before this call.
    return function(2, 0, -1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool call_acquire_player_id(void* ai, std::uint32_t& player_id) noexcept {
  const auto function = g_acquire_player_id;
  if (function == nullptr || ai == nullptr) return false;
  __try {
    std::uint32_t output{};
    const auto result = function(&output, ai);
    if (result != &output || !plausible_pointer(output)) return false;
    player_id = output;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool call_announce_player(const std::uint32_t player_id) noexcept {
  const auto function = g_announce_player;
  if (function == nullptr || !plausible_pointer(player_id)) return false;
  __try {
    function(player_id);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool call_set_position(const std::uint32_t ai,
                       const PositionId& position) noexcept {
  const auto function = g_set_position_id;
  if (function == nullptr || !plausible_pointer(ai)) return false;
  InterlockedIncrement(&g_bridge_set_position_depth);
  bool ok = false;
  __try {
    function(reinterpret_cast<void*>(ai), &position);
    ok = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  InterlockedDecrement(&g_bridge_set_position_depth);
  return ok;
}

bool call_kill_player(const std::uint32_t player_id) noexcept {
  const auto function = g_kill_player;
  if (function == nullptr || !plausible_pointer(player_id)) return false;
  __try {
    function(player_id);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void* call_resolver(const std::uint32_t handle) noexcept {
  const auto function = g_moving_item_resolver;
  if (function == nullptr || handle == 0U) return nullptr;
  __try {
    return function(handle);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool sequence_newer(const std::uint32_t candidate,
                    const std::uint32_t baseline) noexcept {
  const auto delta = candidate - baseline;
  return delta != 0U && delta < 0x80000000U;
}

RemoteSlot* find_slot(const RemoteActorKey& key) noexcept {
  const auto found = std::find_if(g_slots.begin(), g_slots.end(),
                                  [&key](const auto& slot) {
                                    return slot.occupied && slot.key == key;
                                  });
  return found == g_slots.end() ? nullptr : &*found;
}

RemoteSlot* find_player_slot(const std::uint64_t player_id) noexcept {
  const auto found = std::find_if(g_slots.begin(), g_slots.end(),
                                  [player_id](const auto& slot) {
                                    return slot.occupied &&
                                           slot.key.player_id == player_id;
                                  });
  return found == g_slots.end() ? nullptr : &*found;
}

RemoteSlot* free_slot() noexcept {
  const auto found = std::find_if(g_slots.begin(), g_slots.end(),
                                  [](const auto& slot) {
                                    return !slot.occupied;
                                  });
  return found == g_slots.end() ? nullptr : &*found;
}

void publish_hook_indices() noexcept {
  std::size_t output{};
  std::size_t scene_output{};
  LONG active_slots{};
  LONG game_actors{};
  LONG captured_vehicles{};
  LONG first_vehicle_instance{};
  for (const auto& slot : g_slots) {
    g_scene_nodes[scene_output++].store(
        slot.frame_write_ready ? slot.scene_node : 0U,
        std::memory_order_release);
    if (slot.occupied) ++active_slots;
    if (slot.game_player_id != 0U) ++game_actors;
    if (slot.vehicle_instance != 0U) {
      ++captured_vehicles;
      if (first_vehicle_instance == 0) {
        first_vehicle_instance = static_cast<LONG>(slot.vehicle_instance);
      }
    }
    if (slot.occupied && plausible_pointer(slot.game_player_id) &&
        output < g_player_ids.size()) {
      g_player_ids[output++].store(slot.game_player_id,
                                   std::memory_order_release);
    }
  }
  while (output < g_player_ids.size()) {
    g_player_ids[output++].store(0U, std::memory_order_release);
  }
  InterlockedExchange(&g_active_slots, active_slots);
  InterlockedExchange(&g_game_actors, game_actors);
  InterlockedExchange(&g_captured_vehicles, captured_vehicles);
  InterlockedExchange(&g_first_vehicle_instance, first_vehicle_instance);
  g_collision_index.Publish(g_registry.bindings());
}

bool owns_player_id(const std::uint32_t player_id) noexcept {
  if (player_id == 0U) return false;
  return std::any_of(g_player_ids.begin(), g_player_ids.end(),
                     [player_id](const auto& value) {
                       return value.load(std::memory_order_acquire) == player_id;
                     });
}

bool actor_is_member(const RemoteSlot& slot) noexcept {
  if (!plausible_pointer(slot.ai_player) ||
      !plausible_pointer(slot.game_player_id) ||
      slot.ai_player != slot.game_player_id + g_ai_layout.node_player_offset) {
    return false;
  }
  std::uint32_t sentinel{};
  if (!read_u32(static_cast<std::uint32_t>(g_plan.ai_player_list_sentinel_cell),
                sentinel) || !plausible_pointer(sentinel)) {
    return false;
  }
  std::uint32_t node{};
  if (!read_u32(sentinel + g_ai_layout.node_next_offset, node)) return false;
  std::uint32_t previous = sentinel;
  for (std::uint32_t count = 0U; count < g_ai_layout.maximum_nodes; ++count) {
    if (node == sentinel) return false;
    if (!plausible_pointer(node)) return false;
    std::uint32_t back{};
    std::uint32_t next{};
    if (!read_u32(node + g_ai_layout.node_previous_offset, back) ||
        !read_u32(node + g_ai_layout.node_next_offset, next) ||
        back != previous) {
      return false;
    }
    if (node == slot.game_player_id) {
      std::uint32_t self{};
      std::uint32_t actor_type{};
      return read_u32(slot.ai_player + g_ai_layout.player_self_id_offset,
                      self) &&
             read_u32(slot.ai_player + g_ai_layout.player_actor_type_offset,
                      actor_type) &&
             self == slot.game_player_id && actor_type == 2U;
    }
    previous = node;
    node = next;
  }
  return false;
}

bool vehicle_is_valid(const RemoteSlot& slot, const std::uint32_t vehicle,
                      std::uint32_t& physics) noexcept {
  physics = 0U;
  if (!plausible_pointer(vehicle) || !actor_is_member(slot)) return false;
  const auto& layout = g_ai_layout.local_vehicle;
  std::uint32_t owner{};
  std::uint32_t reverse{};
  std::uint32_t reverse_address{};
  const auto moving_item = vehicle + layout.vehicle_moving_item_offset;
  return read_u32(vehicle + layout.vehicle_owner_player_id_offset, owner) &&
         read_u32(vehicle + layout.vehicle_physics_offset, physics) &&
         owner == slot.game_player_id && plausible_pointer(physics) &&
         add_address(physics, layout.physics_vehicle_reverse_offset,
                     reverse_address) &&
         read_u32(reverse_address, reverse) && reverse == moving_item;
}

// AI actor models are rendered from AI_Player's own transform (double
// position at +0x130 and double basis at +0x220 on exact Steam), which the
// stock AI recomputes from the road. Keep it equal to the displayed pose.
bool write_ai_orientation(const std::uint32_t ai_player,
                          const std::array<float, 9>& matrix) noexcept {
  if (g_ai_layout.player_orientation_offset == 0U) return true;
  std::array<double, 9> basis{};
  for (std::size_t index = 0U; index < basis.size(); ++index) {
    basis[index] = static_cast<double>(matrix[index]);
  }
  return write_bytes(ai_player + g_ai_layout.player_orientation_offset,
                     basis.data(), sizeof(basis));
}

DisplayedPose displayed_from(const PoseOutput& output) noexcept {
  DisplayedPose result;
  result.wheels = output.wheels;
  result.matrix = ht2mp::posemath::quat_to_matrix(output.orientation);
  result.inverse = ht2mp::posemath::transpose(result.matrix);
  result.position = {static_cast<float>(output.position[0]),
                     static_cast<float>(output.position[1]),
                     static_cast<float>(output.position[2])};
  return result;
}

PoseSample pose_sample_from(const ht2mp::ipc::PlayerSampleV1& state) noexcept {
  PoseSample sample;
  sample.send_time_us = state.sample_time_ms * 1'000U;
  sample.receive_time_us = state.receive_time_us;
  sample.sequence = state.sequence;
  sample.room_id = state.location.room_id;
  sample.teleport = (state.flags & 4U) != 0U;
  sample.position = state.position;
  sample.orientation = state.orientation;
  sample.linear_velocity = state.linear_velocity;
  sample.angular_velocity = state.angular_velocity;
  sample.wheels = state.vehicle.wheels;
  return sample;
}

void publish_timeline_stats(const PoseTimelineStats& stats) noexcept {
  InterlockedIncrement(&g_timeline_epoch);
  g_timeline_stats = stats;
  InterlockedIncrement(&g_timeline_epoch);
}

void erase_binding(RemoteSlot& slot) noexcept {
  ReleaseRemoteHorn(slot.vehicle_instance);
  slot.wheel_animation.Reset();
  slot.frame_write_ready = false;
  slot.last_frame_us = 0U;
  slot.scene_node = 0U;
  if (slot.registry_generation != 0U) {
    (void)g_registry.Erase(slot.key, slot.registry_generation);
  }
  slot.registry_generation = 0U;
  slot.vehicle_instance = 0U;
  slot.physics = 0U;
  slot.vehicle_candidate = 0U;
}

void destroy_game_actor(RemoteSlot& slot) noexcept {
  erase_binding(slot);
  publish_hook_indices();
  if (plausible_pointer(slot.game_player_id)) {
    if (!call_kill_player(slot.game_player_id)) {
      InterlockedIncrement(&g_validation_failures);
      InterlockedExchange(&g_safe_mode_requested, 1);
    } else {
      InterlockedIncrement(&g_despawns);
    }
  }
  slot.ai_player = 0U;
  slot.game_player_id = 0U;
  slot.has_applied_sequence = false;
  slot.has_logical_position = false;
  slot.has_last_commanded_position = false;
  slot.has_displayed = false;
  publish_hook_indices();
}

void purge_all_game_actors() noexcept {
  for (auto& slot : g_slots) destroy_game_actor(slot);
  g_registry.Clear();
  g_collision_index.Clear();
  publish_hook_indices();
}

bool valid_world_state(const ht2mp::ipc::PlayerSampleV1& state,
                       const std::int32_t local_room_id) noexcept {
  return (state.flags & 1U) != 0U && local_room_id >= 0 &&
         state.location.room_id == local_room_id &&
         (state.location.road_id >= 0 || state.location.node_id >= 0) &&
         state.location.road_id >= -1 && state.location.node_id >= -1 &&
         state.location.road_segment_vector_id >= -1 &&
         state.location.road_segment_id >= -1 &&
         std::isfinite(state.location.road_distance) &&
         state.location.road_distance >= 0.0 &&
         state.location.road_distance <=
             g_ai_layout.maximum_road_distance &&
         std::all_of(state.position.begin(), state.position.end(),
                     [](const double value) {
                       return std::isfinite(value) &&
                              std::abs(value) <= 10'000'000.0;
                     });
}

bool create_actor(RemoteSlot& slot) noexcept {
  InterlockedExchange(&g_requested_vehicle_model,
                      static_cast<LONG>(slot.vehicle_type));
  const auto ai = reinterpret_cast<std::uintptr_t>(call_create_new_player());
  if (ai > UINT32_MAX || !plausible_pointer(static_cast<std::uint32_t>(ai))) {
    return false;
  }
  slot.ai_player = static_cast<std::uint32_t>(ai);
  if (!force_actor_vehicle_appearance(slot.ai_player, slot.vehicle_type,
                                      slot.paint_variant)) {
    // createNewPlayer has already linked the actor. Complete just enough of
    // the native lifecycle to remove it through killPlayer; never leave a
    // half-created stock actor behind after a failed model override.
    std::uint32_t cleanup_player_id{};
    const auto& source = slot.state.location;
    const PositionId cleanup_position{
        source.road_id, source.node_id, source.road_distance,
        source.road_segment_vector_id, source.road_segment_id,
        source.aux0, source.aux1};
    if (!call_acquire_player_id(reinterpret_cast<void*>(slot.ai_player),
                                cleanup_player_id) ||
        !call_set_position(slot.ai_player, cleanup_position) ||
        !call_kill_player(cleanup_player_id)) {
      InterlockedExchange(&g_safe_mode_requested, 1);
    }
    slot.ai_player = 0U;
    return false;
  }
  // Native creation is a two-step lifecycle. createNewPlayer links the
  // AI_Player, while acquirePlayerId performs the required per-type/world
  // accounting before any appearance notification may be emitted.
  std::uint32_t applied_model{};
  std::uint32_t applied_paint{};
  InterlockedExchange(&g_pending_remote_vehicle,
                      static_cast<LONG>(slot.vehicle_type));
  InterlockedExchange(&g_pending_remote_paint,
                      static_cast<LONG>(slot.paint_variant));
  const auto acquired = call_acquire_player_id(
      reinterpret_cast<void*>(slot.ai_player), slot.game_player_id);
  InterlockedExchange(&g_pending_remote_vehicle, -1);
  InterlockedExchange(&g_pending_remote_paint, -1);
  if (!acquired ||
      !actor_is_member(slot) ||
      !read_actor_vehicle_appearance(slot.ai_player, applied_model, applied_paint) ||
      applied_model != slot.vehicle_type || applied_paint != slot.paint_variant) {
    if (plausible_pointer(slot.game_player_id)) {
      (void)call_kill_player(slot.game_player_id);
    }
    slot.ai_player = 0U;
    slot.game_player_id = 0U;
    return false;
  }
  slot.has_logical_position = false;
  publish_hook_indices();
  InterlockedIncrement(&g_spawns);
  return true;
}

bool apply_logical_state(RemoteSlot& slot) noexcept {
  const auto& source = slot.state.location;
  const PositionId position{source.road_id,
                            source.node_id,
                            source.road_distance,
                            source.road_segment_vector_id,
                            source.road_segment_id,
                            source.aux0,
                            source.aux1};
  const auto same_topology = [](const PositionId& left,
                                const PositionId& right) noexcept {
    return left.road_id == right.road_id && left.node_id == right.node_id &&
           left.road_segment_vector_id == right.road_segment_vector_id &&
           left.road_segment_id == right.road_segment_id &&
           left.aux0 == right.aux0 && left.aux1 == right.aux1;
  };
  // road_distance is a 0..1 fraction of the road, so distance is judged in
  // world metres instead; every ~2 m of travel re-seats the native PositionId.
  constexpr double kLogicalRefreshMetres = 2.0;
  const auto& world = slot.state.position;
  const double moved = slot.has_logical_position
                           ? std::sqrt(
                                 (world[0] - slot.logical_refresh_position[0]) *
                                     (world[0] - slot.logical_refresh_position[0]) +
                                 (world[1] - slot.logical_refresh_position[1]) *
                                     (world[1] - slot.logical_refresh_position[1]) +
                                 (world[2] - slot.logical_refresh_position[2]) *
                                     (world[2] - slot.logical_refresh_position[2]))
                           : 0.0;
  const bool refresh_logical_position =
      !slot.has_logical_position || (slot.state.flags & 4U) != 0U ||
      !same_topology(position, slot.logical_position) ||
      moved >= kLogicalRefreshMetres;
  if (refresh_logical_position) {
    if (!call_set_position(slot.ai_player, position)) return false;
    InterlockedIncrement(&g_set_position_calls);
    slot.logical_position = position;
    slot.logical_refresh_position = world;
    slot.has_logical_position = true;
  }
  std::uint32_t flags{};
  if (!read_u32(slot.ai_player + g_ai_layout.player_flags_offset, flags)) {
    return false;
  }
  flags |= 1U;
  if (!write_bytes(slot.ai_player + g_ai_layout.player_flags_offset, &flags,
                   sizeof(flags)) ||
      !write_bytes(slot.ai_player + g_ai_layout.player_world_position_offset,
                   slot.state.position.data(),
                   sizeof(double) * slot.state.position.size())) {
    return false;
  }
  if (!slot.has_applied_sequence &&
      !call_announce_player(slot.game_player_id)) {
    return false;
  }
  slot.applied_sequence = slot.state.sequence;
  slot.has_applied_sequence = true;
  InterlockedIncrement(&g_applied_samples);
  return true;
}

bool ensure_simulated_motion_mode(RemoteSlot& slot) noexcept;

void evaluate_displayed_pose(RemoteSlot& slot, const std::uint64_t now_us) noexcept {
  const auto playback = slot.timeline.Evaluate(now_us);
  if (playback.state == PosePlaybackState::none) return;
  slot.displayed = displayed_from(playback);
  slot.wheel_animation.Advance(playback.wheels, playback.target_send_time_us,
                               playback.state == PosePlaybackState::held);
  slot.has_displayed = true;
  if (slot.vehicle_instance && slot.state.vehicle.valid)
    UpdateRemoteHorn(slot.vehicle_instance, slot.state.vehicle.horn,
                     slot.state.receive_time_us, slot.displayed.position);
  // Native animation consumes these even with MovingItem::update suppressed.
  // Keep the proven M8 zero velocities; network velocity drives the timeline.
  slot.displayed_linear_velocity = {};
  slot.displayed_angular_velocity = {};
}

// Write-only boundary: no game calls, registry changes, pointer recovery or
// mode switching. Safe to use after the native history update has returned.
// The post-AI caller owns validation and physics initialization.
bool write_vehicle_pose(const RemoteSlot& slot) noexcept {
  const auto& matrix = slot.displayed.matrix;
  const auto& inverse_matrix = slot.displayed.inverse;
  const auto& position = slot.displayed.position;
  const auto& layout = g_ai_layout.local_vehicle;
  const std::array<double, 3> world_position{
      static_cast<double>(position[0]), static_cast<double>(position[1]),
      static_cast<double>(position[2])};
  return
      write_bytes(slot.physics + layout.physics_orientation_offset,
                  matrix.data(), sizeof(matrix)) &&
      write_bytes(slot.physics + layout.physics_position_offset,
                  position.data(), sizeof(position)) &&
      write_bytes(slot.physics + layout.physics_body_to_world_orientation_offset,
                  matrix.data(), sizeof(matrix)) &&
      write_bytes(slot.physics + layout.physics_world_to_body_orientation_offset,
                  inverse_matrix.data(), sizeof(inverse_matrix)) &&
      write_bytes(slot.vehicle_instance + layout.vehicle_simulation_orientation_offset,
                  matrix.data(), sizeof(matrix)) &&
      write_bytes(slot.vehicle_instance + layout.vehicle_simulation_position_offset,
                  position.data(), sizeof(position)) &&
      write_bytes(slot.vehicle_instance + layout.vehicle_orientation_offset,
                  matrix.data(), sizeof(matrix)) &&
      write_bytes(slot.vehicle_instance + layout.vehicle_position_offset,
                  position.data(), sizeof(position)) &&
      (layout.vehicle_linear_velocity_offset == 0U ||
       write_bytes(slot.vehicle_instance + layout.vehicle_linear_velocity_offset,
                   slot.displayed_linear_velocity.data(), sizeof(slot.displayed_linear_velocity))) &&
      (layout.vehicle_angular_velocity_offset == 0U ||
       write_bytes(slot.vehicle_instance + layout.vehicle_angular_velocity_offset,
                   slot.displayed_angular_velocity.data(), sizeof(slot.displayed_angular_velocity))) &&
      write_bytes(slot.ai_player + g_ai_layout.player_world_position_offset,
                  world_position.data(), sizeof(world_position)) &&
      write_ai_orientation(slot.ai_player, matrix);
}

bool apply_vehicle_transform(RemoteSlot& slot) noexcept {
  slot.frame_write_ready = false;
  if (slot.vehicle_instance == 0U || !slot.has_displayed) return true;
  std::uint32_t physics{};
  if (!vehicle_is_valid(slot, slot.vehicle_instance, physics) ||
      physics != slot.physics) {
    erase_binding(slot);
    publish_hook_indices();
    return false;
  }
  const auto& matrix = slot.displayed.matrix;
  const auto& position = slot.displayed.position;
  const auto& layout = g_ai_layout.local_vehicle;

  const bool command_changed =
      !slot.has_last_commanded_position ||
      std::abs(position[0] - slot.last_commanded_position[0]) > 0.0005F ||
      std::abs(position[1] - slot.last_commanded_position[1]) > 0.0005F ||
      std::abs(position[2] - slot.last_commanded_position[2]) > 0.0005F;
  if (command_changed) {
    slot.last_commanded_position = position;
    slot.has_last_commanded_position = true;
    InterlockedIncrement(&g_changed_pose_commands);
  }

  // Publish a seqlock-style diagnostic snapshot. These reads happen on the
  // verified game thread and therefore never make the pipe worker touch game
  // memory. A non-zero pre-write error with a near-zero post-write error proves
  // that native simulation is restoring a transform between our callbacks.
  InterlockedIncrement(&g_pose_epoch);
  publish_floats(g_commanded_position_bits, position);
  LONG valid_mask{};
  std::array<float, 3> observed_position{};
  std::array<float, 9> observed_matrix{};
  if (read_bytes(slot.vehicle_instance + layout.vehicle_position_offset,
                 observed_position.data(), sizeof(observed_position))) {
    publish_floats(g_pre_render_position_bits, observed_position);
    valid_mask |= 0x01;
  }
  if (read_bytes(slot.physics + layout.physics_position_offset,
                 observed_position.data(), sizeof(observed_position))) {
    publish_floats(g_pre_physics_position_bits, observed_position);
    valid_mask |= 0x02;
  }
  if (read_bytes(slot.vehicle_instance + layout.vehicle_orientation_offset,
                 observed_matrix.data(), sizeof(observed_matrix))) {
    publish_float(g_pre_render_orientation_error_bits,
                  maximum_matrix_error(observed_matrix, matrix));
    valid_mask |= 0x10;
  }
  if (read_bytes(slot.physics + layout.physics_orientation_offset,
                 observed_matrix.data(), sizeof(observed_matrix))) {
    publish_float(g_pre_physics_orientation_error_bits,
                  maximum_matrix_error(observed_matrix, matrix));
    valid_mask |= 0x20;
  }

  // VehicleInstance contains the render copy, but the lower physics object is
  // not its only source. The rigid-body integrator also retains a body-to-world
  // basis and its transpose. Drive all verified copies so neither the physics
  // step nor the render copy restores the native heading between our ticks.
  const bool written = write_vehicle_pose(slot);
  if (written) {
    // Keep the actor in the physically simulated motion mode: in the kinematic
    // mode the AI re-seats the simulation copy at the PositionId road point
    // and the bridge and the game fight over the pose. The switch happens
    // after the pose writes so setMode(1) seeds the physics body from them.
    slot.frame_write_ready = ensure_simulated_motion_mode(slot);
    InterlockedIncrement(&g_transform_writes);
    if (read_bytes(slot.vehicle_instance + layout.vehicle_position_offset,
                   observed_position.data(), sizeof(observed_position))) {
      publish_floats(g_post_render_position_bits, observed_position);
      valid_mask |= 0x04;
    }
    if (read_bytes(slot.physics + layout.physics_position_offset,
                   observed_position.data(), sizeof(observed_position))) {
      publish_floats(g_post_physics_position_bits, observed_position);
      valid_mask |= 0x08;
    }
    if (read_bytes(slot.vehicle_instance + layout.vehicle_orientation_offset,
                   observed_matrix.data(), sizeof(observed_matrix))) {
      publish_float(g_post_render_orientation_error_bits,
                    maximum_matrix_error(observed_matrix, matrix));
      valid_mask |= 0x40;
    }
    if (read_bytes(slot.physics + layout.physics_orientation_offset,
                   observed_matrix.data(), sizeof(observed_matrix))) {
      publish_float(g_post_physics_orientation_error_bits,
                    maximum_matrix_error(observed_matrix, matrix));
      valid_mask |= 0x80;
    }
  }
  InterlockedExchange(&g_pose_valid_mask, valid_mask);
  InterlockedExchange(&g_pose_sequence, static_cast<LONG>(slot.state.sequence));
  InterlockedIncrement(&g_pose_epoch);
  return written && slot.frame_write_ready;
}

void attach_vehicle_if_ready(RemoteSlot& slot) noexcept {
  if (slot.vehicle_instance != 0U || slot.vehicle_candidate == 0U) return;
  std::uint32_t physics{};
  if (!vehicle_is_valid(slot, slot.vehicle_candidate, physics)) return;
  RemoteActorBinding inserted;
  const RemoteActorBinding candidate{
      slot.key,
      slot.game_player_id,
      0U,
      slot.vehicle_candidate,
      slot.vehicle_candidate + g_ai_layout.local_vehicle.vehicle_moving_item_offset,
      physics};
  if (g_registry.Insert(candidate, inserted) !=
      RemoteActorRegistryResult::inserted) {
    InterlockedIncrement(&g_validation_failures);
    InterlockedExchange(&g_safe_mode_requested, 1);
    return;
  }
  slot.vehicle_instance = inserted.vehicle_instance;
  slot.physics = inserted.physics;
  slot.registry_generation = inserted.generation;
  publish_hook_indices();
}

void recover_vehicle_candidate_from_handle(RemoteSlot& slot) noexcept {
  if (slot.vehicle_instance != 0U || !actor_is_member(slot)) return;

  std::uint32_t vehicle_id{};
  if (!read_u32(slot.ai_player + g_ai_layout.player_vehicle_id_offset,
                vehicle_id) ||
      vehicle_id == 0U || vehicle_id == UINT32_MAX) {
    return;
  }

  // announcePlayer can finish vehicle creation outside the constructor
  // ownership window. AI_Player retains the native vehicle handle, and the
  // exact-build resolver returns its embedded MovingItem. Recover the owning
  // VehicleInstance from that validated relationship instead of scanning.
  const auto moving_item =
      reinterpret_cast<std::uintptr_t>(call_resolver(vehicle_id));
  const auto offset = g_ai_layout.local_vehicle.vehicle_moving_item_offset;
  if (moving_item > UINT32_MAX || moving_item < offset) return;
  const auto candidate = static_cast<std::uint32_t>(moving_item) - offset;
  if (!plausible_pointer(candidate)) return;
  if (slot.vehicle_candidate != candidate) {
    slot.vehicle_candidate = candidate;
    InterlockedIncrement(&g_resolver_vehicle_candidates);
  }
}

void apply_batch() noexcept {
  auto& batch = g_batch;
  if (!g_commands.TryDrain(batch)) return;
  InterlockedExchange(&g_dropped_states,
                      static_cast<LONG>(batch.dropped_states));
  for (std::size_t index = 0U; index < batch.control_count; ++index) {
    const auto& control = batch.controls[index];
    if (control.kind == RemoteControlKind::spawn) {
      const RemoteActorKey key{control.spawn.player_id,
                               control.spawn.incarnation_id};
      if (auto* old = find_player_slot(key.player_id); old != nullptr &&
          old->key != key) {
        destroy_game_actor(*old);
        *old = {};
      }
      if (find_slot(key) != nullptr) continue;
      auto* slot = free_slot();
      if (slot == nullptr) {
        InterlockedExchange(&g_safe_mode_requested, 1);
        continue;
      }
      slot->occupied = true;
      slot->key = key;
      slot->vehicle_type = control.spawn.vehicle_type;
      slot->paint_variant = control.spawn.paint_variant;
      slot->timeline.Reset();
      slot->has_displayed = false;
    } else {
      auto* slot = find_player_slot(control.despawn.player_id);
      if (slot == nullptr ||
          slot->key.incarnation_id != control.despawn.incarnation_id) {
        continue;
      }
      destroy_game_actor(*slot);
      *slot = {};
    }
  }
  for (std::size_t index = 0U; index < batch.state_count; ++index) {
    const auto& state = batch.states[index];
    auto* slot = find_slot({state.player_id, state.incarnation_id});
    if (slot == nullptr) continue;
    if (state.vehicle_type != slot->vehicle_type ||
        state.paint_variant != slot->paint_variant) {
      InterlockedExchange(&g_safe_mode_requested, 1);
      continue;
    }
    if (slot->has_state &&
        !sequence_newer(state.sequence, slot->state.sequence)) {
      continue;
    }
    slot->state = state;
    slot->has_state = true;
    // Only in-world samples carry a trajectory; a hidden sample is a control
    // signal that valid_world_state() turns into a despawn.
    if ((state.flags & 1U) != 0U) {
      (void)slot->timeline.Push(pose_sample_from(state));
    } else {
      slot->timeline.ResetSamples();
      slot->has_displayed = false;
    }
  }
}

void enter_safe_mode_game_thread() noexcept {
  purge_all_game_actors();
  for (auto& slot : g_slots) slot = {};
  RemoteCommandBatch ignored;
  (void)g_commands.TryDrain(ignored);
  publish_hook_indices();
}

void __cdecl ProcessMoveDetour(const std::uint32_t player_id) {
  if (owns_player_id(player_id)) {
    InterlockedIncrement(&g_suppressed_moves);
    return;
  }
  const auto original = g_original_process_move;
  if (original != nullptr) original(player_id);
}

std::uint32_t* __cdecl AcquirePlayerIdDetour(std::uint32_t* output, void* ai) {
  const auto original = g_acquire_player_id;
  if (original == nullptr) return nullptr;
  if (ai != nullptr) {
    const auto address = reinterpret_cast<std::uintptr_t>(ai);
    std::uint32_t actor_type{};
    const auto selected =
        InterlockedCompareExchange(&g_selected_local_vehicle, 0, 0);
    const auto paint = InterlockedCompareExchange(&g_selected_local_paint, 0, 0);
    if (address <= UINT32_MAX && selected >= 0 && paint >= 0 &&
        read_u32(static_cast<std::uint32_t>(address) +
                     g_ai_layout.player_actor_type_offset,
                 actor_type) &&
        actor_type ==
            static_cast<std::uint32_t>(g_ai_layout.required_local_actor_type) &&
        !force_actor_vehicle_appearance(static_cast<std::uint32_t>(address),
                                        static_cast<std::uint16_t>(selected),
                                        static_cast<std::uint8_t>(paint))) {
      InterlockedExchange(&g_safe_mode_requested, 1);
    }
  }
  return original(output, ai);
}

void* __fastcall VehicleConstructorDetour(
    void* self, void*, const std::uint32_t argument1,
    const std::uint32_t argument2, const std::uint32_t player_id,
    const std::uint32_t argument4) {
  std::uint32_t local_player_id{};
  const bool local_vehicle =
      read_u32(static_cast<std::uint32_t>(g_plan.local_player_id_cell),
               local_player_id) &&
      player_id == local_player_id;
  auto effective_argument4 = argument4;
  bool appearance_requested = false;
  bool appearance_applied = false;
  std::uint16_t requested_selector{};
  std::uint8_t requested_paint{};
  if (local_vehicle) {
    const auto selected =
        InterlockedCompareExchange(&g_selected_local_vehicle, 0, 0);
    const auto paint =
        InterlockedCompareExchange(&g_selected_local_paint, 0, 0);
    if (selected >= 0 && paint >= 0) {
      appearance_requested = true;
      requested_selector = static_cast<std::uint16_t>(selected);
      requested_paint = static_cast<std::uint8_t>(paint);
    }
  } else if (InterlockedCompareExchange(&g_exact_steam_appearance, 0, 0) != 0) {
    const auto selected =
        InterlockedCompareExchange(&g_pending_remote_vehicle, 0, 0);
    const auto paint =
        InterlockedCompareExchange(&g_pending_remote_paint, 0, 0);
    if (selected >= 0 && paint >= 0) {
      appearance_requested = true;
      requested_selector = static_cast<std::uint16_t>(selected);
      requested_paint = static_cast<std::uint8_t>(paint);
    }
  }
  if (appearance_requested) {
    appearance_applied = override_constructor_appearance(
        argument2, requested_selector, requested_paint);
    if (!appearance_applied) {
      InterlockedIncrement(&g_validation_failures);
      InterlockedExchange(&g_safe_mode_requested, 1);
    }
  }

  const auto original = g_original_vehicle_constructor;
  void* result = original == nullptr
                     ? nullptr
                     : original(self, argument1, argument2, player_id,
                                 effective_argument4);
  if (result != nullptr) {
    const std::array<std::uint32_t, 4> arguments{
        argument1, argument2, player_id, effective_argument4};
    if (local_vehicle) {
      for (std::size_t index = 0U; index < arguments.size(); ++index) {
        InterlockedExchange(&g_local_vehicle_constructor_args[index],
                            static_cast<LONG>(arguments[index]));
      }
    }
    if (owns_player_id(player_id)) {
      for (std::size_t index = 0U; index < arguments.size(); ++index) {
        InterlockedExchange(&g_remote_vehicle_constructor_args[index],
                            static_cast<LONG>(arguments[index]));
      }
    }
  }
  if (result != nullptr && appearance_applied) {
    InterlockedExchange(&g_applied_vehicle_model,
                        static_cast<LONG>(requested_selector));
  }
  if (result != nullptr && owns_player_id(player_id)) {
    const auto expected_thread = static_cast<DWORD>(
        InterlockedCompareExchange(&g_game_thread_id, 0, 0));
    if (expected_thread == GetCurrentThreadId()) {
      for (auto& slot : g_slots) {
        if (slot.occupied && slot.game_player_id == player_id) {
          slot.vehicle_candidate = static_cast<std::uint32_t>(
              reinterpret_cast<std::uintptr_t>(result));
          InterlockedIncrement(&g_constructor_vehicle_candidates);
          break;
        }
      }
    } else {
      InterlockedIncrement(&g_thread_mismatches);
      InterlockedExchange(&g_safe_mode_requested, 1);
    }
  }
  return result;
}

// VehicleInstance::setMode(1) initialises the physics body from the current
// simulation copy (+0x204) and the velocity fields, so the mode is entered
// natively. Poking +0x2aac directly left the body uninitialised: residual
// physics kept rotating the vehicle and the vehicle update never reached the
// (suppressed) MovingItem::update path.
bool call_set_vehicle_mode(const std::uint32_t vehicle,
                           const std::uint32_t mode) noexcept {
  const auto function = g_vehicle_set_mode;
  if (function == nullptr || !plausible_pointer(vehicle)) return false;
  __try {
    function(reinterpret_cast<void*>(vehicle), static_cast<int>(mode));
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Native mode switch for an owned vehicle whose pose has just been written.
// setMode(1) refuses the transition while the vehicle is flagged asleep
// (+0x51ac != 0) unless +0xf98 == 3, so the sleep flag is cleared first.
bool ensure_simulated_motion_mode(RemoteSlot& slot) noexcept {
  const auto& layout = g_ai_layout.local_vehicle;
  if (layout.vehicle_motion_mode_offset == 0U || g_vehicle_set_mode == nullptr)
    return true;
  std::uint32_t mode{};
  if (!read_u32(slot.vehicle_instance + layout.vehicle_motion_mode_offset, mode)) {
    InterlockedIncrement(&g_motion_mode_failures);
    return false;
  }
  slot.scene_node = 0U;
  if (g_plan.vehicle_scene_node_offset != 0U) {
    std::uint32_t node{};
    std::uint32_t parent{};
    std::uint32_t kind{};
    // Only a native root transform can be replaced by a world-space pose.
    if (read_u32(slot.vehicle_instance + g_plan.vehicle_scene_node_offset, node) &&
        plausible_pointer(node) && read_u32(node + 0x2cU, parent) && parent == 0U &&
        read_u32(node + 0x38U, kind) && kind <= 1U) {
      slot.scene_node = node;
    }
  }
  if (mode == layout.vehicle_motion_mode_simulated) return true;
  constexpr std::uint32_t kSleepFlagOffset = 0x51acU;
  const std::uint32_t zero = 0U;
  (void)write_bytes(slot.vehicle_instance + kSleepFlagOffset, &zero,
                    sizeof(zero));
  if (!call_set_vehicle_mode(slot.vehicle_instance,
                             layout.vehicle_motion_mode_simulated) ||
      !read_u32(slot.vehicle_instance + layout.vehicle_motion_mode_offset,
                mode) ||
      mode != layout.vehicle_motion_mode_simulated) {
    InterlockedIncrement(&g_motion_mode_failures);
    return false;
  }
  InterlockedIncrement(&g_motion_mode_writes);
  return true;
}

// AI_Player::setPositionId recomputes the cached world position (+0x130) from
// the road point of the PositionId. The stock AI calls it for every actor on
// every tick, which made the model rendered from that cache alternate between
// the road point and the bridge's pose. Owned actors only accept the bridge's
// own calls; the game's periodic re-seat is dropped.
void __fastcall SetPositionIdDetour(void* self, void*, const void* position) {
  const auto ai = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  if (InterlockedCompareExchange(&g_bridge_set_position_depth, 0, 0) == 0 &&
      ai >= g_ai_layout.node_player_offset &&
      owns_player_id(ai - g_ai_layout.node_player_offset)) {
    InterlockedIncrement(&g_suppressed_set_position);
    return;
  }
  const auto original = g_set_position_id;
  if (original != nullptr) original(self, position);
}

void __fastcall MovingItemUpdateDetour(void* self, void*,
                                       const std::uint32_t argument1,
                                       const std::uint32_t argument2) {
  const auto moving_item =
      static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  if (g_collision_index.Contains(moving_item)) {
    InterlockedIncrement(&g_suppressed_physics_updates);
    return;
  }
  const auto original = g_original_moving_item_update;
  if (original != nullptr) original(self, argument1, argument2);
}

void __fastcall VehicleRenderHistoryUpdateDetour(void* self, void*) {
  const auto original = g_original_vehicle_render_history_update;
  if (original != nullptr) original(self);

  const auto vehicle_instance =
      static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  const auto moving_item =
      vehicle_instance + g_ai_layout.local_vehicle.vehicle_moving_item_offset;
  if (!g_collision_index.Contains(moving_item)) return;

  const auto expected_thread = static_cast<DWORD>(
      InterlockedCompareExchange(&g_game_thread_id, 0, 0));
  if (expected_thread == 0U || expected_thread != GetCurrentThreadId()) {
    InterlockedIncrement(&g_thread_mismatches);
    InterlockedExchange(&g_safe_mode_requested, 1);
    return;
  }
  if (InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) != 0) return;

  for (auto& slot : g_slots) {
    if (!slot.occupied || !slot.has_displayed || !slot.frame_write_ready ||
        slot.vehicle_instance != vehicle_instance) {
      continue;
    }
    // M10: advance at vehicle cadence without reentering lifecycle APIs from
    // inside vehicle update (the unsafe part of the reverted M9 experiment).
    const auto now_us = MonotonicMicroseconds();
    evaluate_displayed_pose(slot, now_us);
    if (write_vehicle_pose(slot)) {
      if (slot.last_frame_us != 0U && now_us > slot.last_frame_us) {
        const auto gap = static_cast<LONG>(std::min<std::uint64_t>(
            now_us - slot.last_frame_us, static_cast<std::uint64_t>(LONG_MAX)));
        if (gap > InterlockedCompareExchange(&g_frame_max_gap_us, 0, 0)) {
          InterlockedExchange(&g_frame_max_gap_us, gap);
        }
      }
      slot.last_frame_us = now_us;
      InterlockedIncrement(&g_corrected_render_updates);
    } else {
      slot.frame_write_ready = false;
      InterlockedIncrement(&g_validation_failures);
      InterlockedExchange(&g_safe_mode_requested, 1);
    }
    return;
  }
}

// showNic bypasses SceneTransform::getWorld: it projects the four local
// shadow corners with Vehicle+0x204. Lend it the displayed pose for this call
// only. The native matrix is restored even on exceptional exit; no physics,
// contact planes, scene nodes or playback clock are modified here.
bool call_ground_effects_with_pose(void* self, const std::array<float, 12>& pose,
                                   std::array<float, 12>& saved) {
  const auto address = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(self)) + 0x204U;
  if (!read_bytes(address, saved.data(), sizeof(saved))) return false;
  if (!write_bytes(address, pose.data(), sizeof(pose))) {
    (void)write_bytes(address, saved.data(), sizeof(saved));
    return false;
  }
  bool restored{};
  __try {
    g_original_vehicle_ground_effects(self);
  } __finally {
    restored = write_bytes(address, saved.data(), sizeof(saved));
  }
  return restored;
}

void __fastcall VehicleGroundEffectsDetour(void* self, void*) {
  const auto original = g_original_vehicle_ground_effects;
  if (original == nullptr) return;
  const auto vehicle = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  if (!g_collision_index.Contains(vehicle + g_ai_layout.local_vehicle.vehicle_moving_item_offset)) {
    original(self);
    return;
  }
  const auto expected_thread = static_cast<DWORD>(
      InterlockedCompareExchange(&g_game_thread_id, 0, 0));
  if (expected_thread == 0U || expected_thread != GetCurrentThreadId()) {
    InterlockedIncrement(&g_thread_mismatches);
    InterlockedExchange(&g_safe_mode_requested, 1);
    original(self);
    return;
  }
  if (InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) != 0) {
    original(self);
    return;
  }
  for (const auto& slot : g_slots) {
    if (!slot.occupied || !slot.frame_write_ready || !slot.has_displayed ||
        slot.vehicle_instance != vehicle) continue;
    std::array<float, 12> pose{};
    std::copy(slot.displayed.matrix.begin(), slot.displayed.matrix.end(), pose.begin());
    std::copy(slot.displayed.position.begin(), slot.displayed.position.end(), pose.begin() + 9);
    std::array<float, 12> saved{};
    if (!call_ground_effects_with_pose(self, pose, saved)) {
      InterlockedIncrement(&g_validation_failures);
      InterlockedExchange(&g_safe_mode_requested, 1);
      return;
    }
    float position_error{};
    float orientation_error{};
    for (std::size_t index = 0U; index < 9U; ++index) {
      orientation_error = std::max(orientation_error, std::abs(saved[index] - pose[index]));
    }
    for (std::size_t index = 9U; index < 12U; ++index) {
      const auto delta = saved[index] - pose[index];
      position_error += delta * delta;
    }
    publish_float(g_shadow_position_error_bits, std::sqrt(position_error));
    publish_float(g_shadow_orientation_error_bits, orientation_error);
    InterlockedIncrement(&g_shadow_updates);
    return;
  }
  original(self);
}

int __fastcall SceneTransformGetWorldDetour(void* self, void*, void* output) {
  const auto original = g_original_scene_transform_get_world;
  const int kind = original != nullptr ? original(self, output) : 0;
  const auto node = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  const bool owned = std::any_of(g_scene_nodes.begin(), g_scene_nodes.end(),
      [node](const auto& value) {
        return node != 0U && value.load(std::memory_order_acquire) == node;
      });
  if (!owned) return kind;
  const auto expected_thread = static_cast<DWORD>(
      InterlockedCompareExchange(&g_game_thread_id, 0, 0));
  if (expected_thread == 0U || expected_thread != GetCurrentThreadId()) {
    InterlockedIncrement(&g_thread_mismatches);
    InterlockedExchange(&g_safe_mode_requested, 1);
    return kind;
  }
  if (InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) != 0) return kind;
  for (const auto& slot : g_slots) {
    if (!slot.occupied || !slot.frame_write_ready || !slot.has_displayed ||
        slot.scene_node != node) continue;
    // The renderer reads a separate scene-node matrix, not +0x4ef4. Replace
    // only its returned value: no writes to the node, physics or registry and
    // no native calls. All draw passes see the same displayed pose this tick.
    const auto destination = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(output));
    std::array<float, 12> observed{};
    if ((kind != 0 && kind != 1) ||
        !read_bytes(destination, observed.data(), sizeof(observed))) {
      InterlockedIncrement(&g_validation_failures);
      InterlockedExchange(&g_safe_mode_requested, 1);
      return kind;
    }
    float position_error{};
    float orientation_error{};
    for (std::size_t index = 0U; index < 9U; ++index) {
      orientation_error = std::max(orientation_error,
          std::abs(observed[index] - slot.displayed.matrix[index]));
    }
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      const auto delta = observed[9U + axis] - slot.displayed.position[axis];
      position_error += delta * delta;
    }
    publish_float(g_scene_position_error_bits, std::sqrt(position_error));
    publish_float(g_scene_orientation_error_bits, orientation_error);
    if (write_bytes(destination, slot.displayed.matrix.data(), sizeof(slot.displayed.matrix)) &&
        write_bytes(destination + sizeof(slot.displayed.matrix),
                    slot.displayed.position.data(), sizeof(slot.displayed.position))) {
      InterlockedIncrement(&g_scene_reads);
    } else {
      InterlockedIncrement(&g_validation_failures);
      InterlockedExchange(&g_safe_mode_requested, 1);
    }
    return kind;
  }
  return kind;
}

void __cdecl PairCollisionDetour(void* current, void* contact,
                                 const std::uint32_t flags,
                                 const float sample_time) {
  auto other = static_cast<void*>(nullptr);
  std::uint32_t handle{};
  if (contact != nullptr &&
      read_u32(static_cast<std::uint32_t>(
                   reinterpret_cast<std::uintptr_t>(contact)) + 0x74U,
               handle)) {
    other = call_resolver(handle);
  }
  if (g_collision_index.PairTouchesRemote(
          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(current)),
          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(other)))) {
    InterlockedIncrement(&g_suppressed_collisions);
    return;
  }
  const auto original = g_original_pair_collision;
  if (original != nullptr) original(current, contact, flags, sample_time);
}

void __cdecl HitPlayerDetour(const std::uint32_t player_id,
                             const double impact, const int source) {
  if (owns_player_id(player_id)) {
    InterlockedIncrement(&g_suppressed_hits);
    return;
  }
  const auto original = g_original_hit_player;
  if (original != nullptr) original(player_id, impact, source);
}

bool on_game_thread() noexcept {
  const auto current = GetCurrentThreadId();
  const auto expected = static_cast<DWORD>(InterlockedCompareExchange(
      &g_game_thread_id, static_cast<LONG>(current), 0));
  if (expected == 0U || expected == current) return true;
  InterlockedIncrement(&g_thread_mismatches);
  InterlockedExchange(&g_safe_mode_requested, 1);
  return false;
}

void __cdecl PreRegistryResetDetour() {
  if (on_game_thread()) {
    NotifySteamOnlineWorldRegistryReset();
    purge_all_game_actors();
  }
  const auto original = g_original_pre_registry_reset;
  if (original != nullptr) original();
}

void __cdecl PreSaveDetour(void* archive_or_storage) {
  if (on_game_thread()) purge_all_game_actors();
  const auto original = g_original_pre_save;
  if (original != nullptr) original(archive_or_storage);
}

const ht2mp::game::SymbolDescriptor* descriptor(
    const ht2mp::game::GameProfile& profile,
    const std::string_view name) noexcept {
  const auto found = std::find_if(profile.symbols.begin(), profile.symbols.end(),
                                  [name](const auto& value) {
                                    return value.name == name;
                                  });
  return found == profile.symbols.end() ? nullptr : &*found;
}

bool copy_live_bytes(const std::uintptr_t address, void* destination,
                     const std::size_t size) noexcept {
  __try {
    std::memcpy(destination, reinterpret_cast<const void*>(address), size);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool live_target_matches(const ht2mp::game::GameProfile& profile,
                         const std::string_view name,
                         const std::uintptr_t address,
                         std::string& error) {
  const auto* value = descriptor(profile, name);
  if (value == nullptr) {
    error = "remote actor descriptor is unavailable: ";
    error += name;
    return false;
  }
  ht2mp::game::MaskedPattern pattern;
  if (!ht2mp::game::ParseMaskedPattern(value->ida_pattern, pattern, error) ||
      pattern.empty() || pattern.size() > 96U) {
    if (error.empty()) error = "remote actor signature has invalid size";
    return false;
  }
  MEMORY_BASIC_INFORMATION memory{};
  if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                   sizeof(memory)) != sizeof(memory) ||
      memory.State != MEM_COMMIT ||
      (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
    error = "remote actor target is not committed executable memory";
    return false;
  }
  const DWORD protection = memory.Protect & 0xFFU;
  if (protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
      protection != PAGE_EXECUTE_READWRITE &&
      protection != PAGE_EXECUTE_WRITECOPY) {
    error = "remote actor target page is not executable";
    return false;
  }
  std::array<std::uint8_t, 96> live{};
  if (!copy_live_bytes(address, live.data(), pattern.size())) {
    error = "exception while validating live remote actor bytes";
    return false;
  }
  for (std::size_t index = 0U; index < pattern.size(); ++index) {
    if (pattern.mask[index] != 0U && pattern.bytes[index] != live[index]) {
      error = "live remote actor bytes differ for ";
      error += name;
      return false;
    }
  }
  return true;
}

void remove_hooks(const std::span<const LPVOID> targets) noexcept {
  for (const auto target : targets) {
    (void)MH_DisableHook(target);
    (void)MH_RemoveHook(target);
  }
}

bool initialize_impl(const ht2mp::game::ProfileVerification& verification,
                     const std::string_view requested_profile_id,
                     const std::uintptr_t module_base,
                     const std::uint32_t selected_vehicle,
                     const std::uint32_t selected_paint,
                     std::string& error) {
  if (InterlockedCompareExchange(&g_ready, 0, 0) != 0) {
    error = "remote actors are already initialized";
    return false;
  }
  const bool appearance_selected = selected_vehicle != UINT32_MAX;
  if ((appearance_selected &&
       (requested_profile_id != "steam-8138acee" || selected_vehicle < 62U ||
        selected_vehicle > 87U || selected_paint > 3U)) ||
      (!appearance_selected && selected_paint != 0U)) {
    error = "local appearance request is invalid";
    return false;
  }
  GogRemoteActorPlan plan;
  if (!BuildGogRemoteActorPlan(verification, requested_profile_id, module_base,
                               plan, error)) {
    return false;
  }
  struct HookTarget final {
    std::string_view name;
    LPVOID address;
    LPVOID detour;
  };
  std::array<HookTarget, 12> hooks{{
      HookTarget{"process_move_active",
                 reinterpret_cast<LPVOID>(plan.process_move),
                 reinterpret_cast<LPVOID>(&ProcessMoveDetour)},
      HookTarget{"vehicle_instance_constructor_active",
                 reinterpret_cast<LPVOID>(plan.vehicle_instance_constructor),
                 reinterpret_cast<LPVOID>(&VehicleConstructorDetour)},
      HookTarget{"moving_item_update_active",
                 reinterpret_cast<LPVOID>(plan.moving_item_update),
                 reinterpret_cast<LPVOID>(&MovingItemUpdateDetour)},
      HookTarget{"remote_pair_collision_filter_candidate",
                 reinterpret_cast<LPVOID>(plan.pair_collision_dispatch),
                 reinterpret_cast<LPVOID>(&PairCollisionDetour)},
      HookTarget{"hit_player_filter_candidate",
                 reinterpret_cast<LPVOID>(plan.hit_player),
                 reinterpret_cast<LPVOID>(&HitPlayerDetour)},
      HookTarget{"pre_actor_registry_reset_candidate",
                 reinterpret_cast<LPVOID>(plan.pre_actor_registry_reset),
                 reinterpret_cast<LPVOID>(&PreRegistryResetDetour)},
      HookTarget{"pre_save_remote_purge_candidate",
                 reinterpret_cast<LPVOID>(plan.pre_save),
                 reinterpret_cast<LPVOID>(&PreSaveDetour)},
      HookTarget{"set_position_id_active",
                 reinterpret_cast<LPVOID>(plan.set_position_id),
                 reinterpret_cast<LPVOID>(&SetPositionIdDetour)},
  }};
  std::size_t hook_count = 8U;
  std::size_t history_hook_index = SIZE_MAX;
  std::size_t scene_hook_index = SIZE_MAX;
  std::size_t ground_hook_index = SIZE_MAX;
  if (plan.vehicle_render_history_update != 0U) {
    history_hook_index = hook_count;
    hooks[hook_count++] = HookTarget{
        "vehicle_render_history_update_active",
        reinterpret_cast<LPVOID>(plan.vehicle_render_history_update),
        reinterpret_cast<LPVOID>(&VehicleRenderHistoryUpdateDetour)};
  }
  if (plan.scene_transform_get_world != 0U) {
    scene_hook_index = hook_count;
    hooks[hook_count++] = HookTarget{
        "scene_transform_get_world_active",
        reinterpret_cast<LPVOID>(plan.scene_transform_get_world),
        reinterpret_cast<LPVOID>(&SceneTransformGetWorldDetour)};
  }
  if (plan.vehicle_ground_effects != 0U) {
    ground_hook_index = hook_count;
    hooks[hook_count++] = HookTarget{
        "vehicle_ground_effects_active",
        reinterpret_cast<LPVOID>(plan.vehicle_ground_effects),
        reinterpret_cast<LPVOID>(&VehicleGroundEffectsDetour)};
  }
  const auto acquire_hook_index = hook_count;
  hooks[hook_count++] = HookTarget{
      "acquire_player_id_active",
      reinterpret_cast<LPVOID>(plan.acquire_player_id),
      reinterpret_cast<LPVOID>(&AcquirePlayerIdDetour)};
  const auto hook_span = std::span(hooks).first(hook_count);
  const std::array callable_names{
      std::pair{"create_new_player_active", plan.create_new_player},
      std::pair{"announce_player_active", plan.announce_player},
      std::pair{"kill_player_active", plan.kill_player},
      std::pair{"moving_item_handle_resolver_candidate",
                plan.moving_item_handle_resolver},
  };
  for (const auto& item : callable_names) {
    if (!live_target_matches(*verification.profile, item.first, item.second,
                             error)) {
      return false;
    }
  }
  for (const auto& hook : hook_span) {
    if (!live_target_matches(*verification.profile, hook.name,
                             reinterpret_cast<std::uintptr_t>(hook.address),
                             error)) {
      return false;
    }
  }

  std::array<LPVOID, hooks.size()> originals{};
  std::array<LPVOID, hooks.size()> created{};
  std::size_t created_count{};
  for (std::size_t index = 0U; index < hook_count; ++index) {
    const auto status = MH_CreateHook(hooks[index].address, hooks[index].detour,
                                      &originals[index]);
    if (status != MH_OK) {
      remove_hooks(std::span(created).first(created_count));
      error = "cannot create remote actor hook ";
      error += hooks[index].name;
      error += ": ";
      error += MH_StatusToString(status);
      return false;
    }
    created[created_count++] = hooks[index].address;
  }

  g_plan = plan;
  g_ai_layout = verification.profile->ai_player_pool;
  InterlockedExchange(&g_exact_steam_appearance,
                      requested_profile_id == "steam-8138acee" ? 1 : 0);
  InterlockedExchange(&g_selected_local_vehicle,
                      appearance_selected ? static_cast<LONG>(selected_vehicle)
                                          : -1);
  InterlockedExchange(&g_selected_local_paint,
                      appearance_selected ? static_cast<LONG>(selected_paint)
                                          : -1);
  InterlockedExchange(&g_pending_remote_vehicle, -1);
  InterlockedExchange(&g_pending_remote_paint, -1);
  g_create_new_player = reinterpret_cast<CreateNewPlayer>(plan.create_new_player);
  g_acquire_player_id =
      reinterpret_cast<AcquirePlayerId>(originals[acquire_hook_index]);
  g_announce_player = reinterpret_cast<AnnouncePlayer>(plan.announce_player);
  // The bridge's own setPositionId calls go through the trampoline so the
  // detour above can tell them apart from the stock AI's periodic re-seat.
  g_set_position_id = reinterpret_cast<SetPositionId>(originals[7]);
  g_kill_player = reinterpret_cast<KillPlayer>(plan.kill_player);
  g_moving_item_resolver =
      reinterpret_cast<MovingItemResolver>(plan.moving_item_handle_resolver);
  g_original_process_move = reinterpret_cast<ProcessMove>(originals[0]);
  g_original_vehicle_constructor =
      reinterpret_cast<VehicleConstructor>(originals[1]);
  g_original_moving_item_update =
      reinterpret_cast<MovingItemUpdate>(originals[2]);
  g_original_pair_collision = reinterpret_cast<PairCollision>(originals[3]);
  g_original_hit_player = reinterpret_cast<HitPlayer>(originals[4]);
  g_original_pre_registry_reset =
      reinterpret_cast<PreRegistryReset>(originals[5]);
  g_original_pre_save = reinterpret_cast<PreSave>(originals[6]);
  g_original_vehicle_render_history_update =
      plan.vehicle_render_history_update == 0U
          ? nullptr
          : reinterpret_cast<VehicleRenderHistoryUpdate>(
                originals[history_hook_index]);
  g_vehicle_set_mode = plan.vehicle_set_mode == 0U
                           ? nullptr
                           : reinterpret_cast<VehicleSetMode>(plan.vehicle_set_mode);
  g_original_scene_transform_get_world = plan.scene_transform_get_world == 0U
      ? nullptr
      : reinterpret_cast<SceneTransformGetWorld>(originals[scene_hook_index]);
  g_original_vehicle_ground_effects = plan.vehicle_ground_effects == 0U
      ? nullptr
      : reinterpret_cast<VehicleGroundEffects>(originals[ground_hook_index]);
  for (auto& slot : g_slots) slot = {};
  g_commands.Clear();
  g_registry.Clear();
  publish_hook_indices();
  InterlockedExchange(&g_safe_mode_requested, 0);
  InterlockedExchange(&g_game_thread_id, 0);
  InterlockedExchange(&g_transform_writes, 0);
  InterlockedExchange(&g_changed_pose_commands, 0);
  InterlockedExchange(&g_corrected_render_updates, 0);
  InterlockedExchange(&g_frame_max_gap_us, 0);
  InterlockedExchange(&g_scene_reads, 0);
  InterlockedExchange(&g_shadow_updates, 0);
  publish_float(g_shadow_position_error_bits, 0.0F);
  publish_float(g_shadow_orientation_error_bits, 0.0F);
  publish_float(g_scene_position_error_bits, 0.0F);
  publish_float(g_scene_orientation_error_bits, 0.0F);
  InterlockedExchange(&g_first_vehicle_instance, 0);
  InterlockedExchange(&g_set_position_calls, 0);
  InterlockedExchange(&g_dropped_states, 0);
  InterlockedExchange(&g_receive_stamps_replaced, 0);
  InterlockedExchange(&g_motion_mode_writes, 0);
  InterlockedExchange(&g_motion_mode_failures, 0);
  InterlockedExchange(&g_suppressed_set_position, 0);
  InterlockedExchange(&g_bridge_set_position_depth, 0);
  publish_timeline_stats({});
  InterlockedExchange(&g_pose_sequence, 0);
  InterlockedExchange(&g_pose_valid_mask, 0);
  InterlockedExchange(&g_pose_epoch, 0);
  for (std::size_t index = 0U; index < 3U; ++index) {
    InterlockedExchange(&g_commanded_position_bits[index], 0);
    InterlockedExchange(&g_pre_render_position_bits[index], 0);
    InterlockedExchange(&g_pre_physics_position_bits[index], 0);
    InterlockedExchange(&g_post_render_position_bits[index], 0);
    InterlockedExchange(&g_post_physics_position_bits[index], 0);
  }
  InterlockedExchange(&g_pre_render_orientation_error_bits, 0);
  InterlockedExchange(&g_pre_physics_orientation_error_bits, 0);
  InterlockedExchange(&g_post_render_orientation_error_bits, 0);
  InterlockedExchange(&g_post_physics_orientation_error_bits, 0);

  for (const auto& hook : hook_span) {
    const auto status = MH_QueueEnableHook(hook.address);
    if (status != MH_OK) {
      remove_hooks(std::span(created).first(created_count));
      error = "cannot queue remote actor hook";
      return false;
    }
  }
  const auto applied = MH_ApplyQueued();
  if (applied != MH_OK) {
    remove_hooks(std::span(created).first(created_count));
    error = "cannot enable remote actor hooks: ";
    error += MH_StatusToString(applied);
    return false;
  }
  InterlockedExchange(&g_ready, 1);
  return true;
}

bool queue_result_ok(const RemoteQueueResult result) noexcept {
  if (result == RemoteQueueResult::queued ||
      result == RemoteQueueResult::dropped_oldest ||
      result == RemoteQueueResult::stale) {
    return true;
  }
  InterlockedExchange(&g_safe_mode_requested, 1);
  return false;
}

} // namespace

bool InitializeGogRemoteActors(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base,
    const std::uint32_t selected_vehicle,
    const std::uint32_t selected_paint,
    std::string& error) noexcept {
  try {
    error.clear();
    return initialize_impl(verification, requested_profile_id, module_base,
                           selected_vehicle, selected_paint, error);
  } catch (...) {
    error = "exception while initializing remote actor backend";
    return false;
  }
}

bool QueueRemoteSpawn(const ht2mp::ipc::SpawnRemoteV1& command) noexcept {
  return RemoteActorsReady() &&
         InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) == 0 &&
         queue_result_ok(g_commands.PushSpawn(command));
}

bool QueueRemoteSample(const ht2mp::ipc::PlayerSampleV1& command) noexcept {
  if (!RemoteActorsReady() ||
      InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) != 0) {
    return false;
  }
  // The sidecar stamps the datagram arrival on the shared QPC clock. A missing
  // or implausible stamp (different clock domain, >5 s away) is replaced by the
  // worker's own arrival time so the timeline never trusts a foreign epoch.
  auto stamped = command;
  const auto now_us = MonotonicMicroseconds();
  const auto difference = now_us > stamped.receive_time_us
                              ? now_us - stamped.receive_time_us
                              : stamped.receive_time_us - now_us;
  if (stamped.receive_time_us == 0U || difference > 5'000'000U) {
    if (stamped.receive_time_us != 0U) {
      InterlockedIncrement(&g_receive_stamps_replaced);
    }
    stamped.receive_time_us = now_us == 0U ? 1U : now_us;
  }
  return queue_result_ok(g_commands.PushState(stamped));
}

bool QueueRemoteDespawn(const ht2mp::ipc::DespawnRemoteV1& command) noexcept {
  return RemoteActorsReady() &&
         InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) == 0 &&
         queue_result_ok(g_commands.PushDespawn(command));
}

void TickGogRemoteActors(const bool local_world_ready,
                         const std::int32_t local_room_id) noexcept {
  if (!RemoteActorsReady()) return;
  const auto current = GetCurrentThreadId();
  const auto previous = static_cast<DWORD>(
      InterlockedCompareExchange(&g_game_thread_id,
                                 static_cast<LONG>(current), 0));
  if (previous != 0U && previous != current) {
    InterlockedIncrement(&g_thread_mismatches);
    InterlockedExchange(&g_safe_mode_requested, 1);
  }
  if (InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) != 0) {
    enter_safe_mode_game_thread();
    return;
  }
  apply_batch();
  if (!local_world_ready) {
    purge_all_game_actors();
    return;
  }

  bool published_timeline = false;
  for (auto& slot : g_slots) {
    slot.frame_write_ready = false;
    if (!slot.occupied || !slot.has_state) continue;
    if (!valid_world_state(slot.state, local_room_id)) {
      destroy_game_actor(slot);
      continue;
    }
    if (slot.ai_player != 0U && !actor_is_member(slot)) {
      erase_binding(slot);
      slot.ai_player = 0U;
      slot.game_player_id = 0U;
      slot.has_applied_sequence = false;
      slot.has_logical_position = false;
      InterlockedIncrement(&g_validation_failures);
      publish_hook_indices();
    }
    if (slot.ai_player == 0U) {
      const auto now = GetTickCount64();
      if (now < slot.retry_after_ms) continue;
      if (!create_actor(slot)) {
        slot.retry_after_ms = now + 1'000U;
        InterlockedIncrement(&g_validation_failures);
        continue;
      }
    }
    recover_vehicle_candidate_from_handle(slot);
    attach_vehicle_if_ready(slot);
    if (!slot.has_applied_sequence ||
        sequence_newer(slot.state.sequence, slot.applied_sequence)) {
      if (!apply_logical_state(slot)) {
        InterlockedIncrement(&g_validation_failures);
        InterlockedExchange(&g_safe_mode_requested, 1);
        break;
      }
    }
    // Seed new bindings immediately; GOG also retains this playback boundary.
    evaluate_displayed_pose(slot, MonotonicMicroseconds());
    if (!published_timeline) {
      publish_timeline_stats(slot.timeline.stats());
      published_timeline = true;
    }
    if (!apply_vehicle_transform(slot) && slot.vehicle_instance != 0U) {
      InterlockedIncrement(&g_validation_failures);
      InterlockedExchange(&g_safe_mode_requested, 1);
      break;
    }
  }
  publish_hook_indices();
}

void RequestRemoteActorSafeMode() noexcept {
  DisableReplication();
  InterlockedExchange(&g_safe_mode_requested, 1);
}

bool RemoteActorOwnsPlayerId(const std::uint32_t player_id) noexcept {
  return owns_player_id(player_id);
}

bool GetRemoteVehicleState(const std::uint32_t vehicle, ht2mp::protocol::VehicleState& state,
                          std::array<float, ht2mp::protocol::kMaxVehicleWheels>& wheel_phases) noexcept {
  if (!g_collision_index.Contains(vehicle + g_ai_layout.local_vehicle.vehicle_moving_item_offset)) return false;
  if (InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) != 0 ||
      static_cast<DWORD>(InterlockedCompareExchange(&g_game_thread_id, 0, 0)) != GetCurrentThreadId()) return false;
  for (const auto& slot : g_slots) {
    if (slot.occupied && slot.frame_write_ready && slot.vehicle_instance == vehicle) {
      std::uint32_t physics{};
      if (!vehicle_is_valid(slot, vehicle, physics) || physics != slot.physics) return false;
      state = slot.state.vehicle;
      state.wheels = slot.displayed.wheels;
      wheel_phases = slot.wheel_animation.phases();
      return true;
    }
  }
  return false;
}

bool RemoteActorsReady() noexcept {
  return InterlockedCompareExchange(&g_ready, 0, 0) != 0;
}

RemoteActorBackendStats GetRemoteActorBackendStats() noexcept {
  RemoteActorBackendStats result;
  result.ready = RemoteActorsReady();
  result.safe_mode_requested =
      InterlockedCompareExchange(&g_safe_mode_requested, 0, 0) != 0;
  result.active_slots = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_active_slots, 0, 0));
  result.game_actors = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_game_actors, 0, 0));
  result.captured_vehicles = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_captured_vehicles, 0, 0));
  result.constructor_vehicle_candidates = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_constructor_vehicle_candidates, 0, 0));
  result.resolver_vehicle_candidates = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_resolver_vehicle_candidates, 0, 0));
  for (std::size_t index = 0U;
       index < result.local_vehicle_constructor_args.size(); ++index) {
    result.local_vehicle_constructor_args[index] = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_local_vehicle_constructor_args[index], 0,
                                   0));
    result.remote_vehicle_constructor_args[index] = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_remote_vehicle_constructor_args[index], 0,
                                   0));
  }
  result.spawns = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_spawns, 0, 0));
  result.despawns = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_despawns, 0, 0));
  result.applied_samples = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_applied_samples, 0, 0));
  result.suppressed_moves = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_suppressed_moves, 0, 0));
  result.suppressed_physics_updates = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_suppressed_physics_updates, 0, 0));
  result.frame_max_gap_us = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_frame_max_gap_us, 0, 0));
  result.scene_reads = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_scene_reads, 0, 0));
  result.scene_position_error = load_float(g_scene_position_error_bits);
  result.scene_orientation_error = load_float(g_scene_orientation_error_bits);
  result.shadow_updates = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_shadow_updates, 0, 0));
  result.shadow_position_error = load_float(g_shadow_position_error_bits);
  result.shadow_orientation_error = load_float(g_shadow_orientation_error_bits);
  result.corrected_render_updates = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_corrected_render_updates, 0, 0));
  result.suppressed_collisions = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_suppressed_collisions, 0, 0));
  result.suppressed_hits = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_suppressed_hits, 0, 0));
  result.requested_vehicle_model = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_requested_vehicle_model, 0, 0));
  result.applied_vehicle_model = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_applied_vehicle_model, 0, 0));
  result.vehicle_instance_address = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_first_vehicle_instance, 0, 0));
  result.set_position_calls = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_set_position_calls, 0, 0));
  result.transform_writes = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_transform_writes, 0, 0));
  result.changed_pose_commands = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_changed_pose_commands, 0, 0));
  for (std::size_t attempt = 0U; attempt < 4U; ++attempt) {
    const auto before = InterlockedCompareExchange(&g_pose_epoch, 0, 0);
    if ((before & 1) != 0) continue;
    result.pose_sequence = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_pose_sequence, 0, 0));
    result.pose_valid_mask = static_cast<std::uint32_t>(
        InterlockedCompareExchange(&g_pose_valid_mask, 0, 0));
    load_floats(result.commanded_position, g_commanded_position_bits);
    load_floats(result.pre_render_position, g_pre_render_position_bits);
    load_floats(result.pre_physics_position, g_pre_physics_position_bits);
    load_floats(result.post_render_position, g_post_render_position_bits);
    load_floats(result.post_physics_position, g_post_physics_position_bits);
    result.pre_render_orientation_error =
        load_float(g_pre_render_orientation_error_bits);
    result.pre_physics_orientation_error =
        load_float(g_pre_physics_orientation_error_bits);
    result.post_render_orientation_error =
        load_float(g_post_render_orientation_error_bits);
    result.post_physics_orientation_error =
        load_float(g_post_physics_orientation_error_bits);
    const auto after = InterlockedCompareExchange(&g_pose_epoch, 0, 0);
    if (before == after && (after & 1) == 0) break;
  }
  result.validation_failures = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_validation_failures, 0, 0));
  result.thread_mismatches = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_thread_mismatches, 0, 0));
  result.dropped_states = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_dropped_states, 0, 0));
  result.receive_stamps_replaced = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_receive_stamps_replaced, 0, 0));
  result.motion_mode_writes = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_motion_mode_writes, 0, 0));
  result.motion_mode_failures = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_motion_mode_failures, 0, 0));
  result.suppressed_set_position = static_cast<std::uint32_t>(
      InterlockedCompareExchange(&g_suppressed_set_position, 0, 0));
  for (std::size_t attempt = 0U; attempt < 4U; ++attempt) {
    const auto before = InterlockedCompareExchange(&g_timeline_epoch, 0, 0);
    if ((before & 1) != 0) continue;
    result.timeline = g_timeline_stats;
    const auto after = InterlockedCompareExchange(&g_timeline_epoch, 0, 0);
    if (before == after && (after & 1) == 0) break;
  }
  return result;
}

} // namespace ht2mp::bridge
