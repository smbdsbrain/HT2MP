#pragma once

#include "ht2mp/game/profile.hpp"
#include "ht2mp/ipc/bootstrap.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace ht2mp::bridge {

// Validates the process-independent bootstrap policy. In particular,
// observer_only is mandatory even when an experimental diagnostic is
// requested; unknown flags and non-zero reserved fields fail closed.
[[nodiscard]] bool ValidateBootstrapRequestFields(
    const ht2mp::ipc::BootstrapV1& bootstrap) noexcept;

// Read-only telemetry plan shared by exact, independently verified profiles.
// The historical type name is retained to avoid changing BootstrapV1 ABI.
struct GogTelemetryPlan final {
  std::uintptr_t module_base{};
  std::uintptr_t transform_address{};
  std::uintptr_t post_ai_tick_address{};
  std::uint32_t transform_rva{};
  std::uint32_t post_ai_tick_rva{};
};

// Addresses for read-only/pass-through actor diagnostics. Resolving this plan
// does not authorize calling, detouring with altered behavior, spawning, or
// writing. It only proves that the exact-GOG safety boundaries and matching
// caller-cleanup ABI records are available to the separately opted-in,
// pass-through observation harness.
struct GogActorDiagnosticPlan final {
  std::uintptr_t module_base{};
  std::uintptr_t moving_item_handle_resolver{};
  std::uintptr_t pair_collision_dispatch{};
  std::uintptr_t hit_player{};
  std::uintptr_t pre_actor_registry_reset{};
  std::uintptr_t pre_save{};
};

struct GogRemoteActorPlan final {
  std::uintptr_t module_base{};
  std::uintptr_t ai_player_list_sentinel_cell{};
  std::uintptr_t local_player_id_cell{};
  std::uintptr_t vehicle_model_registry_begin_cell{};
  std::uintptr_t vehicle_model_registry_end_cell{};
  std::uintptr_t create_new_player{};
  std::uintptr_t acquire_player_id{};
  std::uintptr_t announce_player{};
  std::uintptr_t process_move{};
  std::uintptr_t set_position_id{};
  std::uintptr_t kill_player{};
  std::uintptr_t vehicle_instance_constructor{};
  std::uintptr_t moving_item_handle_resolver{};
  std::uintptr_t moving_item_update{};
  std::uintptr_t vehicle_render_history_update{};
  // Steam-only: VehicleInstance::setMode used to enter the simulated motion
  // mode natively for remote actors (zero on profiles without the symbol).
  std::uintptr_t vehicle_set_mode{};
  // Steam render member +0x110 owns its scene transform at +0x44.
  std::uintptr_t scene_transform_get_world{};
  std::uintptr_t vehicle_ground_effects{};
  std::uint32_t vehicle_scene_node_offset{};
  std::uintptr_t pair_collision_dispatch{};
  std::uintptr_t hit_player{};
  std::uintptr_t pre_actor_registry_reset{};
  std::uintptr_t pre_save{};
};

struct GogAutoEnterPlan final {
  std::uintptr_t module_base{};
  std::uintptr_t main_menu_activate{};
  std::uintptr_t main_menu_input{};
  std::uintptr_t main_menu_event{};
  std::uintptr_t single_player_event{};
  std::uintptr_t single_player_panel_cell{};
  std::uintptr_t main_menu_vtable{};
  std::uintptr_t main_menu_event_vtable{};
  std::uintptr_t single_player_vtable{};
  std::uintptr_t single_player_event_vtable{};
  std::uint32_t event_subobject_offset{};
  std::uint32_t single_player_widget_id{};
  std::uint32_t load_widget_id{};
};

struct SteamOnlineWorldPlan final {
  std::uintptr_t module_base{};
  std::uintptr_t ai_player_list_sentinel_cell{};
  std::uintptr_t local_player_id_cell{};
  std::uintptr_t current_counts_cell{};
  std::uintptr_t target_counts_cell{};
  std::uintptr_t create_players_regular{};
  std::uintptr_t update_ghosts{};
  std::uintptr_t update_dealers{};
  std::uintptr_t kill_player{};
  std::uintptr_t get_assortment{};
  std::uintptr_t order_vehicle{};
  std::uintptr_t go_hire_it{};
  std::uintptr_t focus_loss_pause_handler{};
};

// Converts an offline-verified GOG or Steam report into read-only runtime
// addresses. No memory is touched here, which makes all fail-closed policy
// checks unit-testable. Actor writes are available only through explicit
// exact-build experimental opt-ins; diagnostics remain exact-GOG-only.
[[nodiscard]] bool BuildGogTelemetryPlan(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    GogTelemetryPlan& plan,
    std::string& error);

[[nodiscard]] bool BuildGogActorDiagnosticPlan(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    GogActorDiagnosticPlan& plan,
    std::string& error);

[[nodiscard]] bool BuildGogRemoteActorPlan(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    GogRemoteActorPlan& plan,
    std::string& error);

[[nodiscard]] bool BuildGogAutoEnterPlan(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    GogAutoEnterPlan& plan,
    std::string& error);

[[nodiscard]] bool BuildSteamOnlineWorldPlan(
    const ht2mp::game::ProfileVerification& verification,
    std::string_view requested_profile_id,
    std::uintptr_t module_base,
    SteamOnlineWorldPlan& plan,
    std::string& error);

} // namespace ht2mp::bridge
