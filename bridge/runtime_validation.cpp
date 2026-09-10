#include "runtime_validation.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace ht2mp::bridge {

bool ValidateBootstrapRequestFields(
    const ht2mp::ipc::BootstrapV1& bootstrap) noexcept {
  constexpr auto kObserverOnly =
      static_cast<std::uint32_t>(ht2mp::ipc::BootstrapFlags::observer_only);
  constexpr auto kKnownFlags =
      kObserverOnly |
      static_cast<std::uint32_t>(
          ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry) |
      static_cast<std::uint32_t>(
          ht2mp::ipc::BootstrapFlags::experimental_gog_actor_diagnostics) |
      static_cast<std::uint32_t>(
          ht2mp::ipc::BootstrapFlags::auto_enter_world) |
      static_cast<std::uint32_t>(
          ht2mp::ipc::BootstrapFlags::experimental_gog_remote_actors) |
      static_cast<std::uint32_t>(
          ht2mp::ipc::BootstrapFlags::stock_npc_suppression) |
      static_cast<std::uint32_t>(
          ht2mp::ipc::BootstrapFlags::background_tick);
  constexpr auto kAppearance = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::appearance_override);
  constexpr auto kKnownFlagsWithAppearance = kKnownFlags | kAppearance;
  constexpr auto kRemoteActors = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_remote_actors);
  constexpr auto kTelemetry = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry);
  constexpr auto kActorDiagnostics = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_actor_diagnostics);
  constexpr auto kAutoEnter = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::auto_enter_world);
  constexpr auto kStockNpcSuppression = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::stock_npc_suppression);
  constexpr auto kBackgroundTick = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::background_tick);
  const auto online_flags = bootstrap.flags &
                            (kStockNpcSuppression | kBackgroundTick);
  if (bootstrap.run_id == 0U || (bootstrap.flags & kObserverOnly) == 0U ||
      (bootstrap.flags & ~kKnownFlagsWithAppearance) != 0U ||
      ((bootstrap.flags & kActorDiagnostics) != 0U &&
       (bootstrap.flags & kTelemetry) == 0U) ||
      ((bootstrap.flags & kAutoEnter) != 0U &&
       (bootstrap.flags & kTelemetry) == 0U) ||
      ((bootstrap.flags & kRemoteActors) != 0U &&
       (bootstrap.flags & kTelemetry) == 0U) ||
      ((bootstrap.flags & kRemoteActors) != 0U &&
       (bootstrap.flags & kActorDiagnostics) != 0U) ||
      (online_flags != 0U &&
       online_flags != (kStockNpcSuppression | kBackgroundTick)) ||
      (online_flags != 0U &&
       ((bootstrap.flags & kTelemetry) == 0U ||
        (bootstrap.flags & kRemoteActors) == 0U)) ||
      ((bootstrap.flags & kAppearance) != 0U &&
       (bootstrap.vehicle_selector < 62U || bootstrap.vehicle_selector > 87U ||
        bootstrap.paint_variant > 3U)) ||
      ((bootstrap.flags & kAppearance) == 0U &&
       (bootstrap.vehicle_selector != 0xffffffffU || bootstrap.paint_variant != 0U)) ||
      bootstrap.pipe_name.back() != L'\0' ||
      std::any_of(bootstrap.reserved.begin(), bootstrap.reserved.end(),
                  [](const std::uint32_t value) { return value != 0U; })) {
    return false;
  }
  const auto profile_end =
      std::find(bootstrap.profile_id.begin(), bootstrap.profile_id.end(), '\0');
  if (profile_end == bootstrap.profile_id.begin() ||
      profile_end == bootstrap.profile_id.end()) {
    return false;
  }
  const std::string_view profile(
      bootstrap.profile_id.data(),
      static_cast<std::size_t>(profile_end - bootstrap.profile_id.begin()));
  if ((online_flags != 0U || (bootstrap.flags & kAppearance) != 0U) &&
      profile != "steam-8138acee") return false;
  return profile == "gog-05588140" || profile == "steam-8138acee";
}

bool BuildGogRemoteActorPlan(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base, GogRemoteActorPlan& plan,
    std::string& error) {
  plan = {};
  error.clear();
  const bool supported_profile = requested_profile_id == "gog-05588140" ||
                                 requested_profile_id == "steam-8138acee";
  if (!supported_profile || !verification.accepted() ||
      verification.profile == nullptr ||
      verification.profile->id != requested_profile_id) {
    error = "remote actors require an exact verified supported profile";
    return false;
  }
  if (module_base == 0U || verification.pe.size_of_image == 0U ||
      module_base > std::numeric_limits<std::uintptr_t>::max() -
                        verification.pe.size_of_image) {
    error = "runtime module address range is invalid";
    return false;
  }
  const auto& ai_layout = verification.profile->ai_player_pool;
  const auto& vehicle_layout = ai_layout.local_vehicle;
  if (!ai_layout.observer_enabled ||
      ai_layout.player_world_position_offset == 0U ||
      ai_layout.player_world_position_offset > 0x10000U - 3U * sizeof(double) ||
      ai_layout.player_vehicle_descriptor_offset == 0U ||
      ai_layout.player_vehicle_descriptor_offset > 0x10000U - 4U ||
      ai_layout.vehicle_descriptor_vehicle_id_offset > 60U ||
      ai_layout.vehicle_descriptor_model_selector_offset > 60U ||
      ai_layout.vehicle_descriptor_paint_variant_offset > 60U ||
      ai_layout.vehicle_descriptor_vehicle_id_offset ==
          ai_layout.vehicle_descriptor_model_selector_offset ||
      ai_layout.vehicle_descriptor_vehicle_id_offset ==
          ai_layout.vehicle_descriptor_paint_variant_offset ||
      ai_layout.vehicle_descriptor_model_selector_offset ==
          ai_layout.vehicle_descriptor_paint_variant_offset ||
      ai_layout.maximum_vehicle_model_selector == 0U ||
      ai_layout.maximum_vehicle_paint_variant != 3U ||
      vehicle_layout.vehicle_simulation_orientation_offset == 0U ||
      vehicle_layout.vehicle_simulation_position_offset <=
          vehicle_layout.vehicle_simulation_orientation_offset ||
      vehicle_layout.vehicle_simulation_position_offset -
                  vehicle_layout.vehicle_simulation_orientation_offset !=
              9U * sizeof(float) ||
      vehicle_layout.physics_orientation_offset == 0U ||
      vehicle_layout.physics_position_offset <=
          vehicle_layout.physics_orientation_offset ||
      vehicle_layout.physics_orientation_offset >
          0x10000U - 9U * sizeof(float) ||
      vehicle_layout.physics_position_offset >
          0x10000U - 3U * sizeof(float) ||
      vehicle_layout.physics_body_to_world_orientation_offset == 0U ||
      vehicle_layout.physics_body_to_world_orientation_offset >
          0x10000U - 9U * sizeof(float) ||
      vehicle_layout.physics_world_to_body_orientation_offset == 0U ||
      vehicle_layout.physics_world_to_body_orientation_offset >
          0x10000U - 9U * sizeof(float) ||
      vehicle_layout.physics_body_to_world_orientation_offset ==
          vehicle_layout.physics_world_to_body_orientation_offset) {
    error = "remote actor world-position layout is unavailable";
    return false;
  }

  struct Required final {
    std::string_view symbol_name;
    std::string_view abi_name;
    ht2mp::game::X86CallingConvention convention;
    std::uintptr_t* destination;
  };
  std::array required{
      Required{"create_new_player_active", "create_new_player",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.create_new_player},
      Required{"acquire_player_id_active", "acquire_player_id",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.acquire_player_id},
      Required{"announce_player_active", "announce_player",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.announce_player},
      Required{"process_move_active", "process_move",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.process_move},
      Required{"set_position_id_active", "set_position_id",
               ht2mp::game::X86CallingConvention::member_thiscall,
               &plan.set_position_id},
      Required{"kill_player_active", "kill_player",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.kill_player},
      Required{"vehicle_instance_constructor_active",
               "vehicle_instance_constructor",
               ht2mp::game::X86CallingConvention::member_thiscall,
               &plan.vehicle_instance_constructor},
      Required{"moving_item_handle_resolver_candidate",
               "moving_item_handle_resolver",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.moving_item_handle_resolver},
      Required{"moving_item_update_active", "moving_item_update",
               ht2mp::game::X86CallingConvention::member_thiscall,
               &plan.moving_item_update},
      Required{"remote_pair_collision_filter_candidate",
               "moving_pair_collision_dispatch",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.pair_collision_dispatch},
      Required{"hit_player_filter_candidate", "hit_player",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.hit_player},
      Required{"pre_actor_registry_reset_candidate",
               "pre_actor_registry_reset",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.pre_actor_registry_reset},
      Required{"pre_save_remote_purge_candidate", "save_orchestrator",
               ht2mp::game::X86CallingConvention::caller_cleanup,
               &plan.pre_save},
  };
  const auto resolve_required = [&](const Required& item) {
    const auto* symbol = verification.FindSymbol(item.symbol_name);
    if (symbol == nullptr || !symbol->accepted || symbol->match_count != 1U ||
        symbol->kind != ht2mp::game::SymbolKind::diagnostic_function ||
        symbol->result_rva >= verification.pe.size_of_image) {
      error = "required exact-profile remote actor symbol is unavailable: ";
      error += item.symbol_name;
      plan = {};
      return false;
    }
    const auto abi = std::find_if(
        verification.profile->legacy_multiplayer.functions.begin(),
        verification.profile->legacy_multiplayer.functions.end(),
        [&item](const auto& value) { return value.name == item.abi_name; });
    if (abi == verification.profile->legacy_multiplayer.functions.end() ||
        abi->rva != symbol->result_rva ||
        abi->calling_convention != item.convention) {
      error = "remote actor ABI metadata is missing or inconsistent: ";
      error += item.abi_name;
      plan = {};
      return false;
    }
    *item.destination = module_base + symbol->result_rva;
    return true;
  };
  for (const auto& item : required) {
    if (!resolve_required(item)) return false;
  }
  if (requested_profile_id == "steam-8138acee") {
    const Required vehicle_update{
        "vehicle_render_history_update_active", "vehicle_render_history_update",
        ht2mp::game::X86CallingConvention::member_thiscall,
        &plan.vehicle_render_history_update};
    if (!resolve_required(vehicle_update)) return false;
    const Required vehicle_set_mode{
        "vehicle_set_mode_active", "vehicle_set_mode",
        ht2mp::game::X86CallingConvention::member_thiscall,
        &plan.vehicle_set_mode};
    if (!resolve_required(vehicle_set_mode)) return false;
    const Required scene_transform{
        "scene_transform_get_world_active", "scene_transform_get_world",
        ht2mp::game::X86CallingConvention::member_thiscall,
        &plan.scene_transform_get_world};
    if (!resolve_required(scene_transform)) return false;
    const Required ground_effects{
        "vehicle_ground_effects_active", "vehicle_ground_effects",
        ht2mp::game::X86CallingConvention::member_thiscall,
        &plan.vehicle_ground_effects};
    if (!resolve_required(ground_effects)) return false;
    plan.vehicle_scene_node_offset = 0x154U;
  }
  const auto resolve_read_only_cell =
      [&](const std::string_view symbol_name, std::uintptr_t& destination,
          const std::string_view diagnostic) {
        const auto* symbol = verification.FindSymbol(symbol_name);
        if (symbol == nullptr || !symbol->accepted ||
            symbol->match_count != 1U ||
            symbol->kind != ht2mp::game::SymbolKind::read_only_data ||
            symbol->result_rva >= verification.pe.size_of_image) {
          error.assign(diagnostic);
          return false;
        }
        destination = module_base + symbol->result_rva;
        return true;
      };
  if (!resolve_read_only_cell(
          verification.profile->ai_player_pool.sentinel_cell_symbol,
          plan.ai_player_list_sentinel_cell,
          "verified AI player sentinel cell is unavailable") ||
      !resolve_read_only_cell(
          verification.profile->ai_player_pool.local_player_cell_symbol,
          plan.local_player_id_cell,
          "verified local PlayerId cell is unavailable") ||
      !resolve_read_only_cell(
          "vehicle_model_registry_begin_cell",
          plan.vehicle_model_registry_begin_cell,
          "verified vehicle model registry begin cell is unavailable") ||
      !resolve_read_only_cell(
          "vehicle_model_registry_end_cell",
          plan.vehicle_model_registry_end_cell,
          "verified vehicle model registry end cell is unavailable")) {
    plan = {};
    return false;
  }
  plan.module_base = module_base;
  return true;
}

bool BuildGogTelemetryPlan(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base,
    GogTelemetryPlan& plan,
    std::string& error) {
  plan = {};
  error.clear();
  if (!verification.accepted() || verification.profile == nullptr) {
    error = "game executable did not pass full fail-closed verification";
    return false;
  }
  if (verification.profile->id != requested_profile_id) {
    error = "verified profile does not match the requested telemetry profile";
    return false;
  }
  if (!verification.observer_ready()) {
    error = "local transform observer is not live-validated for this profile";
    return false;
  }
  const auto* transform = verification.FindSymbol(
      verification.profile->observer.transform_symbol);
  const auto* tick = verification.FindSymbol("post_ai_tick_candidate");
  if (transform == nullptr || !transform->accepted ||
      transform->kind != ht2mp::game::SymbolKind::read_only_data) {
    error = "verified local render transform symbol is unavailable";
    return false;
  }
  if (tick == nullptr || !tick->accepted || tick->match_count != 1U ||
      tick->kind != ht2mp::game::SymbolKind::diagnostic_function) {
    error = "verified post-AI tick candidate is unavailable or ambiguous";
    return false;
  }
  if (module_base == 0U || verification.pe.size_of_image == 0U ||
      module_base > std::numeric_limits<std::uintptr_t>::max() -
                        verification.pe.size_of_image) {
    error = "runtime module address range is invalid";
    return false;
  }
  if (transform->result_rva >= verification.pe.size_of_image ||
      tick->result_rva >= verification.pe.size_of_image) {
    error = "verified telemetry RVA lies outside SizeOfImage";
    return false;
  }
  plan.module_base = module_base;
  plan.transform_rva = transform->result_rva;
  plan.post_ai_tick_rva = tick->result_rva;
  plan.transform_address = module_base + transform->result_rva;
  plan.post_ai_tick_address = module_base + tick->result_rva;
  return true;
}

bool BuildSteamOnlineWorldPlan(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base, SteamOnlineWorldPlan& plan,
    std::string& error) {
  plan = {};
  error.clear();
  if (requested_profile_id != "steam-8138acee" ||
      !verification.accepted() || verification.profile == nullptr ||
      verification.profile->id != requested_profile_id ||
      verification.profile->edition != ht2mp::game::GameEdition::steam) {
    error = "online world requires exact verified profile steam-8138acee";
    return false;
  }
  if (module_base == 0U || verification.pe.size_of_image == 0U ||
      module_base > std::numeric_limits<std::uintptr_t>::max() -
                        verification.pe.size_of_image) {
    error = "runtime module address range is invalid";
    return false;
  }

  struct RequiredFunction final {
    std::string_view symbol_name;
    std::string_view abi_name;
    std::uintptr_t* destination;
  };
  std::array functions{
      RequiredFunction{"create_players_regular_online",
                       "create_players_regular", &plan.create_players_regular},
      RequiredFunction{"update_ghosts_online", "update_ghosts",
                       &plan.update_ghosts},
      RequiredFunction{"update_dealers_online", "update_dealers",
                       &plan.update_dealers},
      RequiredFunction{"kill_player_active", "kill_player", &plan.kill_player},
      RequiredFunction{"parking_get_assortment_online", "get_assortment",
                       &plan.get_assortment},
      RequiredFunction{"parking_order_vehicle_online", "order_vehicle",
                       &plan.order_vehicle},
      RequiredFunction{"parking_go_hire_it_online", "go_hire_it",
                       &plan.go_hire_it},
  };
  for (const auto& item : functions) {
    const auto* symbol = verification.FindSymbol(item.symbol_name);
    const auto abi = std::find_if(
        verification.profile->legacy_multiplayer.functions.begin(),
        verification.profile->legacy_multiplayer.functions.end(),
        [&](const auto& value) { return value.name == item.abi_name; });
    if (symbol == nullptr || !symbol->accepted || symbol->match_count != 1U ||
        symbol->kind != ht2mp::game::SymbolKind::diagnostic_function ||
        symbol->result_rva >= verification.pe.size_of_image ||
        abi == verification.profile->legacy_multiplayer.functions.end() ||
        abi->rva != symbol->result_rva ||
        abi->calling_convention !=
            ht2mp::game::X86CallingConvention::caller_cleanup) {
      error = "online-world function signature/ABI is unavailable: ";
      error += item.symbol_name;
      plan = {};
      return false;
    }
    *item.destination = module_base + symbol->result_rva;
  }

  const auto* focus = verification.FindSymbol(
      "focus_loss_pause_handler_online");
  const auto focus_abi = std::find_if(
      verification.profile->legacy_multiplayer.functions.begin(),
      verification.profile->legacy_multiplayer.functions.end(),
      [](const auto& value) {
        return value.name == "focus_loss_pause_handler";
      });
  if (focus == nullptr || !focus->accepted || focus->match_count != 1U ||
      focus->kind != ht2mp::game::SymbolKind::diagnostic_function ||
      focus->result_rva >= verification.pe.size_of_image ||
      focus_abi == verification.profile->legacy_multiplayer.functions.end() ||
      focus_abi->rva != focus->result_rva ||
      focus_abi->calling_convention !=
          ht2mp::game::X86CallingConvention::member_thiscall) {
    error = "online-world focus-loss handler signature/ABI is unavailable";
    plan = {};
    return false;
  }
  plan.focus_loss_pause_handler = module_base + focus->result_rva;

  struct RequiredData final {
    std::string_view symbol_name;
    ht2mp::game::SymbolKind kind;
    std::uintptr_t* destination;
  };
  std::array data{
      RequiredData{"ai_player_list_sentinel_cell",
                   ht2mp::game::SymbolKind::read_only_data,
                   &plan.ai_player_list_sentinel_cell},
      RequiredData{"local_player_id_cell",
                   ht2mp::game::SymbolKind::read_only_data,
                   &plan.local_player_id_cell},
      RequiredData{"stock_actor_current_counts_cell",
                   ht2mp::game::SymbolKind::writable_data,
                   &plan.current_counts_cell},
      RequiredData{"stock_actor_target_counts_cell",
                   ht2mp::game::SymbolKind::writable_data,
                   &plan.target_counts_cell},
  };
  for (const auto& item : data) {
    const auto* symbol = verification.FindSymbol(item.symbol_name);
    if (symbol == nullptr || !symbol->accepted || symbol->match_count != 1U ||
        symbol->kind != item.kind ||
        symbol->result_rva >= verification.pe.size_of_image) {
      error = "online-world data symbol is unavailable: ";
      error += item.symbol_name;
      plan = {};
      return false;
    }
    *item.destination = module_base + symbol->result_rva;
  }

  const std::array addresses{
      plan.ai_player_list_sentinel_cell, plan.local_player_id_cell,
      plan.current_counts_cell, plan.target_counts_cell,
      plan.create_players_regular, plan.update_ghosts, plan.update_dealers,
      plan.kill_player, plan.get_assortment, plan.order_vehicle,
      plan.go_hire_it, plan.focus_loss_pause_handler,
  };
  for (std::size_t i = 0; i < addresses.size(); ++i) {
    for (std::size_t j = i + 1; j < addresses.size(); ++j) {
      if (addresses[i] == addresses[j]) {
        error = "online-world plan contains aliased required addresses";
        plan = {};
        return false;
      }
    }
  }
  plan.module_base = module_base;
  return true;
}

bool BuildGogActorDiagnosticPlan(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base, GogActorDiagnosticPlan& plan,
    std::string& error) {
  plan = {};
  error.clear();
  if (requested_profile_id != "gog-05588140") {
    error = "actor diagnostics are defined only for gog-05588140";
    return false;
  }
  if (!verification.accepted() || verification.profile == nullptr ||
      verification.profile->id != requested_profile_id ||
      verification.profile->edition != ht2mp::game::GameEdition::gog) {
    error = "actor diagnostics require the exact verified GOG profile";
    return false;
  }
  if (verification.profile->legacy_multiplayer.runtime_write_validated) {
    error = "diagnostic resolver cannot serve as actor-write authorization";
    return false;
  }
  if (module_base == 0U || verification.pe.size_of_image == 0U ||
      module_base > std::numeric_limits<std::uintptr_t>::max() -
                        verification.pe.size_of_image) {
    error = "runtime module address range is invalid";
    return false;
  }

  struct Required final {
    std::string_view name;
    std::string_view abi_name;
    std::uintptr_t* destination;
  };
  std::array required{
      Required{"moving_item_handle_resolver_candidate",
               "moving_item_handle_resolver",
               &plan.moving_item_handle_resolver},
      Required{"remote_pair_collision_filter_candidate",
               "moving_pair_collision_dispatch",
               &plan.pair_collision_dispatch},
      Required{"hit_player_filter_candidate", "hit_player",
               &plan.hit_player},
      Required{"pre_actor_registry_reset_candidate",
               "pre_actor_registry_reset",
               &plan.pre_actor_registry_reset},
      Required{"pre_save_remote_purge_candidate", "save_orchestrator",
               &plan.pre_save},
  };
  for (const auto& item : required) {
    const auto* symbol = verification.FindSymbol(item.name);
    if (symbol == nullptr || !symbol->accepted || symbol->match_count != 1U ||
        symbol->kind != ht2mp::game::SymbolKind::diagnostic_function ||
        symbol->result_rva >= verification.pe.size_of_image) {
      error = "required exact-GOG actor diagnostic is unavailable: ";
      error += item.name;
      plan = {};
      return false;
    }
    const auto abi = std::find_if(
        verification.profile->legacy_multiplayer.functions.begin(),
        verification.profile->legacy_multiplayer.functions.end(),
        [&item](const auto& function) { return function.name == item.abi_name; });
    if (abi == verification.profile->legacy_multiplayer.functions.end() ||
        abi->rva != symbol->result_rva ||
        abi->calling_convention !=
            ht2mp::game::X86CallingConvention::caller_cleanup) {
      error = "actor diagnostic ABI metadata is missing or inconsistent: ";
      error += item.abi_name;
      plan = {};
      return false;
    }
    *item.destination = module_base + symbol->result_rva;
  }
  plan.module_base = module_base;
  return true;
}

bool BuildGogAutoEnterPlan(
    const ht2mp::game::ProfileVerification& verification,
    const std::string_view requested_profile_id,
    const std::uintptr_t module_base, GogAutoEnterPlan& plan,
    std::string& error) {
  plan = {};
  error.clear();
  if (!verification.accepted() || verification.profile == nullptr ||
      verification.profile->id != requested_profile_id ||
      !verification.profile->auto_enter_world.enabled) {
    error = "auto-enter requires an exact verified menu profile";
    return false;
  }
  if (module_base == 0U || verification.pe.size_of_image == 0U ||
      module_base > std::numeric_limits<std::uintptr_t>::max() -
                        verification.pe.size_of_image) {
    error = "runtime module address range is invalid";
    return false;
  }

  const auto& layout = verification.profile->auto_enter_world;
  struct Required final {
    std::string_view symbol_name;
    std::string_view abi_name;
    ht2mp::game::SymbolKind kind;
    std::uintptr_t* destination;
  };
  std::array required{
      Required{layout.main_menu_activate_symbol, "main_menu_activate",
               ht2mp::game::SymbolKind::diagnostic_function,
               &plan.main_menu_activate},
      Required{layout.main_menu_input_symbol, "main_menu_input",
               ht2mp::game::SymbolKind::diagnostic_function,
               &plan.main_menu_input},
      Required{layout.main_menu_event_symbol, "main_menu_event",
               ht2mp::game::SymbolKind::diagnostic_function,
               &plan.main_menu_event},
      Required{layout.single_player_event_symbol, "single_player_event",
               ht2mp::game::SymbolKind::diagnostic_function,
               &plan.single_player_event},
  };
  for (const auto& item : required) {
    const auto* symbol = verification.FindSymbol(item.symbol_name);
    if (symbol == nullptr || !symbol->accepted || symbol->match_count != 1U ||
        symbol->kind != item.kind ||
        symbol->result_rva >= verification.pe.size_of_image) {
      error = "required exact-profile auto-enter symbol is unavailable: ";
      error += item.symbol_name;
      plan = {};
      return false;
    }
    const auto abi = std::find_if(
        verification.profile->legacy_multiplayer.functions.begin(),
        verification.profile->legacy_multiplayer.functions.end(),
        [&item](const auto& function) { return function.name == item.abi_name; });
    if (abi == verification.profile->legacy_multiplayer.functions.end() ||
        abi->rva != symbol->result_rva ||
        abi->calling_convention !=
            ht2mp::game::X86CallingConvention::member_thiscall) {
      error = "auto-enter ABI metadata is missing or inconsistent: ";
      error += item.abi_name;
      plan = {};
      return false;
    }
    *item.destination = module_base + symbol->result_rva;
  }

  const auto* panel_cell =
      verification.FindSymbol(layout.single_player_panel_cell_symbol);
  if (panel_cell == nullptr || !panel_cell->accepted ||
      panel_cell->match_count != 1U ||
      panel_cell->kind != ht2mp::game::SymbolKind::read_only_data ||
      panel_cell->result_rva >= verification.pe.size_of_image) {
    error = "exact-profile single-player panel cell is unavailable";
    plan = {};
    return false;
  }
  const std::array vtables{
      layout.main_menu_vtable_rva, layout.main_menu_event_vtable_rva,
      layout.single_player_vtable_rva,
      layout.single_player_event_vtable_rva};
  if (layout.event_subobject_offset == 0U ||
      layout.single_player_widget_id == 0U || layout.load_widget_id == 0U ||
      std::any_of(vtables.begin(), vtables.end(),
                  [&](const std::uint32_t rva) {
                    return rva >= verification.pe.size_of_image;
                  })) {
    error = "exact-profile auto-enter object layout is invalid";
    plan = {};
    return false;
  }

  plan.module_base = module_base;
  plan.single_player_panel_cell = module_base + panel_cell->result_rva;
  plan.main_menu_vtable = module_base + layout.main_menu_vtable_rva;
  plan.main_menu_event_vtable =
      module_base + layout.main_menu_event_vtable_rva;
  plan.single_player_vtable = module_base + layout.single_player_vtable_rva;
  plan.single_player_event_vtable =
      module_base + layout.single_player_event_vtable_rva;
  plan.event_subobject_offset = layout.event_subobject_offset;
  plan.single_player_widget_id = layout.single_player_widget_id;
  plan.load_widget_id = layout.load_widget_id;
  return true;
}

} // namespace ht2mp::bridge
