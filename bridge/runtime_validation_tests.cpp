#include "runtime_validation.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures{};

void check(const bool condition, const char* expression, const int line) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
  }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

ht2mp::ipc::BootstrapV1 valid_bootstrap() {
  ht2mp::ipc::BootstrapV1 bootstrap;
  bootstrap.run_id = 7U;
  constexpr char kProfile[] = "gog-05588140";
  std::copy(std::begin(kProfile), std::end(kProfile),
            bootstrap.profile_id.begin());
  constexpr wchar_t kPipe[] = L"\\\\.\\pipe\\HT2MP-test";
  std::copy(std::begin(kPipe), std::end(kPipe), bootstrap.pipe_name.begin());
  return bootstrap;
}

void bootstrap_flags_fail_closed() {
  auto bootstrap = valid_bootstrap();
  CHECK(ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  bootstrap.flags = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry);
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  bootstrap = valid_bootstrap();
  bootstrap.flags |= static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry);
  CHECK(ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  bootstrap = valid_bootstrap();
  bootstrap.flags |= static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_remote_actors);
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.flags |= static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry);
  CHECK(ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.flags |= static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_actor_diagnostics);
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  bootstrap = valid_bootstrap();
  bootstrap.flags |= static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::auto_enter_world);
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.flags |= static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry);
  CHECK(ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  bootstrap = valid_bootstrap();
  bootstrap.flags |= static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_actor_diagnostics);
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  bootstrap.flags |= static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry);
  CHECK(ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  bootstrap.flags = 0U;
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap = valid_bootstrap();
  bootstrap.flags |= 0x80000000U;
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap = valid_bootstrap();
  bootstrap.reserved[3] = 1U;
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  constexpr auto telemetry = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry);
  constexpr auto remote = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::experimental_gog_remote_actors);
  constexpr auto stock = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::stock_npc_suppression);
  constexpr auto background = static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::background_tick);
  bootstrap = valid_bootstrap();
  bootstrap.flags |= telemetry | remote | stock | background;
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  std::fill(bootstrap.profile_id.begin(), bootstrap.profile_id.end(), '\0');
  constexpr char kSteamProfile[] = "steam-8138acee";
  std::copy(std::begin(kSteamProfile), std::end(kSteamProfile),
            bootstrap.profile_id.begin());
  CHECK(ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.flags &= ~background;
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.flags |= background;
  bootstrap.flags &= ~remote;
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));

  bootstrap = valid_bootstrap();
  bootstrap.flags |= telemetry | remote | stock | background |
                     static_cast<std::uint32_t>(
                         ht2mp::ipc::BootstrapFlags::appearance_override);
  std::fill(bootstrap.profile_id.begin(), bootstrap.profile_id.end(), '\0');
  std::copy(std::begin(kSteamProfile), std::end(kSteamProfile),
            bootstrap.profile_id.begin());
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.vehicle_selector = 62U;
  bootstrap.paint_variant = 3U;
  CHECK(ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.vehicle_selector = 88U;
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.vehicle_selector = 62U;
  bootstrap.paint_variant = 4U;
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
  bootstrap.paint_variant = 0U;
  bootstrap.flags &= ~static_cast<std::uint32_t>(
      ht2mp::ipc::BootstrapFlags::appearance_override);
  CHECK(!ht2mp::bridge::ValidateBootstrapRequestFields(bootstrap));
}

ht2mp::game::ProfileVerification accepted_gog_report() {
  ht2mp::game::ProfileVerification report;
  report.profile = ht2mp::game::FindGameProfile("gog-05588140");
  report.hash_matched = true;
  report.pe_matched = true;
  report.symbols_matched = true;
  report.pe.size_of_image = report.profile->pe.size_of_image;
  report.symbols = {
      {"local_render_transform", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::live_observed, 0x00130d30U, 0x00295920U,
       1U, true, "test"},
      {"ai_player_list_sentinel_cell",
       ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0000a0e0U, 0x0028a8f0U,
       1U, true, "test"},
      {"local_player_id_cell", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00077ba0U, 0x0028c454U,
       1U, true, "test"},
      {"vehicle_model_registry_begin_cell",
       ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022511U, 0x0028bf38U,
       1U, true, "test"},
      {"vehicle_model_registry_end_cell",
       ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022511U, 0x0028bf3cU,
       1U, true, "test"},
      {"post_ai_tick_candidate", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0002d7c0U, 0x0002d7c0U,
       1U, true, "test"},
      {"moving_item_handle_resolver_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0017c0a0U, 0x0017c0a0U,
       1U, true, "test"},
      {"moving_item_update_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00182310U, 0x00182310U,
       1U, true, "test"},
      {"remote_pair_collision_filter_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00181070U, 0x00181070U,
       1U, true, "test"},
      {"hit_player_filter_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00046cf0U, 0x00046cf0U,
       1U, true, "test"},
      {"pre_actor_registry_reset_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00003950U, 0x00003950U,
       1U, true, "test"},
      {"pre_save_remote_purge_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00083470U, 0x00083470U,
       1U, true, "test"},
      {"create_new_player_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00015840U, 0x00015840U,
       1U, true, "test"},
      {"acquire_player_id_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022480U, 0x00022480U,
       1U, true, "test"},
      {"announce_player_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x000200f0U, 0x000200f0U,
       1U, true, "test"},
      {"process_move_active", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0001af30U, 0x0001af30U,
       1U, true, "test"},
      {"set_position_id_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x000742e0U, 0x000742e0U,
       1U, true, "test"},
      {"kill_player_active", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00046670U, 0x00046670U,
       1U, true, "test"},
      {"vehicle_instance_constructor_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00143530U, 0x00143530U,
       1U, true, "test"},
      {"main_menu_activate", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00128000U, 0x00128000U,
       1U, true, "test"},
      {"main_menu_input", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00127070U, 0x00127070U,
       1U, true, "test"},
      {"main_menu_event", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00127370U, 0x00127370U,
       1U, true, "test"},
      {"single_player_event", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0011cb60U, 0x0011cb60U,
       1U, true, "test"},
      {"single_player_panel_cell", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0011cb60U, 0x002cdbfcU,
       1U, true, "test"},
  };
  return report;
}

ht2mp::game::ProfileVerification accepted_steam_report() {
  ht2mp::game::ProfileVerification report;
  report.profile = ht2mp::game::FindGameProfile("steam-8138acee");
  report.hash_matched = true;
  report.pe_matched = true;
  report.symbols_matched = true;
  report.pe.size_of_image = report.profile->pe.size_of_image;
  report.symbols = {
      {"local_render_transform", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00131600U, 0x002969c0U,
       1U, true, "test"},
      {"ai_player_list_sentinel_cell",
       ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0000a180U, 0x0028b990U,
       1U, true, "test"},
      {"local_player_id_cell", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00077b00U, 0x0028d4f4U,
       1U, true, "test"},
      {"vehicle_model_registry_begin_cell",
       ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022421U, 0x0028cfd8U,
       1U, true, "test"},
      {"vehicle_model_registry_end_cell",
       ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022421U, 0x0028cfdcU,
       1U, true, "test"},
      {"post_ai_tick_candidate", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0002d6e0U, 0x0002d6e0U,
       1U, true, "test"},
      {"moving_item_handle_resolver_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0017c9e0U, 0x0017c9e0U,
       1U, true, "test"},
      {"moving_item_update_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00182c30U, 0x00182c30U,
       1U, true, "test"},
      {"vehicle_render_history_update_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00154710U, 0x00154710U,
       1U, true, "test"},
      {"vehicle_set_mode_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0014ca60U, 0x0014ca60U,
       1U, true, "test"},
      {"vehicle_ground_effects_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00152940U, 0x00152940U,
       1U, true, "test"},
      {"scene_transform_get_world_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x001f25c0U, 0x001f25c0U,
       1U, true, "test"},
      {"remote_pair_collision_filter_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00181990U, 0x00181990U,
       1U, true, "test"},
      {"hit_player_filter_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00046d20U, 0x00046d20U,
       1U, true, "test"},
      {"pre_actor_registry_reset_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00003940U, 0x00003940U,
       1U, true, "test"},
      {"pre_save_remote_purge_candidate",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x000833a0U, 0x000833a0U,
       1U, true, "test"},
      {"create_new_player_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00015940U, 0x00015940U,
       1U, true, "test"},
      {"acquire_player_id_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022390U, 0x00022390U,
       1U, true, "test"},
      {"announce_player_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00020000U, 0x00020000U,
       1U, true, "test"},
      {"process_move_active", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0001ae60U, 0x0001ae60U,
       1U, true, "test"},
      {"set_position_id_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00074290U, 0x00074290U,
       1U, true, "test"},
      {"kill_player_active", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x000466a0U, 0x000466a0U,
       1U, true, "test"},
      {"vehicle_instance_constructor_active",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00143e00U, 0x00143e00U,
       1U, true, "test"},
      {"main_menu_activate", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00128820U, 0x00128820U,
       1U, true, "test"},
      {"main_menu_input", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00127890U, 0x00127890U,
       1U, true, "test"},
      {"main_menu_event", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00127b90U, 0x00127b90U,
       1U, true, "test"},
      {"single_player_event", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0011d380U, 0x0011d380U,
       1U, true, "test"},
      {"single_player_panel_cell", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x0011d380U, 0x002cec9cU,
       1U, true, "test"},
      {"create_players_regular_online",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022f20U, 0x00022f20U,
       1U, true, "test"},
      {"stock_actor_current_counts_cell",
       ht2mp::game::SymbolKind::writable_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022f20U, 0x0028cca8U,
       1U, true, "test"},
      {"stock_actor_target_counts_cell",
       ht2mp::game::SymbolKind::writable_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022f20U, 0x0028cf28U,
       1U, true, "test"},
      {"update_ghosts_online", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00020ed0U, 0x00020ed0U,
       1U, true, "test"},
      {"update_dealers_online", ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00022370U, 0x00022370U,
       1U, true, "test"},
      {"parking_get_assortment_online",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00033350U, 0x00033350U,
       1U, true, "test"},
      {"parking_order_vehicle_online",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00033b60U, 0x00033b60U,
       1U, true, "test"},
      {"parking_go_hire_it_online",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x00019030U, 0x00019030U,
       1U, true, "test"},
      {"focus_loss_pause_handler_online",
       ht2mp::game::SymbolKind::diagnostic_function,
       ht2mp::game::EvidenceLevel::static_analysis, 0x001dd330U, 0x001dd330U,
       1U, true, "test"},
  };
  return report;
}

void accepts_only_complete_gog_plan() {
  auto report = accepted_gog_report();
  ht2mp::bridge::GogTelemetryPlan plan;
  std::string error;
  CHECK(ht2mp::bridge::BuildGogTelemetryPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
  CHECK(error.empty());
  CHECK(plan.transform_address == 0x00695920U);
  CHECK(plan.post_ai_tick_address == 0x0042d7c0U);
}

void rejects_steam_and_incomplete_reports() {
  auto report = accepted_gog_report();
  ht2mp::bridge::GogTelemetryPlan plan;
  std::string error;
  CHECK(!ht2mp::bridge::BuildGogTelemetryPlan(
      report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(!error.empty());

  auto steam_report = accepted_steam_report();
  CHECK(ht2mp::bridge::BuildGogTelemetryPlan(
      steam_report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(error.empty());
  CHECK(plan.transform_address == 0x006969c0U);
  CHECK(plan.post_ai_tick_address == 0x0042d6e0U);

  report.symbols.erase(
      std::remove_if(report.symbols.begin(), report.symbols.end(),
                     [](const auto& symbol) {
                       return symbol.name == "post_ai_tick_candidate";
                     }),
      report.symbols.end());
  CHECK(!ht2mp::bridge::BuildGogTelemetryPlan(
      report, "gog-05588140", 0x00400000U, plan, error));

  report = accepted_gog_report();
  report.hash_matched = false;
  CHECK(!ht2mp::bridge::BuildGogTelemetryPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
}

void actor_diagnostics_never_authorize_writes() {
  auto report = accepted_gog_report();
  ht2mp::bridge::GogActorDiagnosticPlan plan;
  std::string error;
  CHECK(ht2mp::bridge::BuildGogActorDiagnosticPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
  CHECK(error.empty());
  CHECK(plan.module_base == 0x00400000U);
  CHECK(plan.moving_item_handle_resolver == 0x0057c0a0U);
  CHECK(plan.pair_collision_dispatch == 0x00581070U);
  CHECK(plan.hit_player == 0x00446cf0U);
  CHECK(plan.pre_actor_registry_reset == 0x00403950U);
  CHECK(plan.pre_save == 0x00483470U);
  CHECK(report.profile != nullptr &&
        !report.profile->legacy_multiplayer.runtime_write_validated);

  report.symbols.erase(
      std::remove_if(report.symbols.begin(), report.symbols.end(),
                     [](const auto& symbol) {
                       return symbol.name == "pre_save_remote_purge_candidate";
                     }),
      report.symbols.end());
  CHECK(!ht2mp::bridge::BuildGogActorDiagnosticPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
  CHECK(plan.module_base == 0U && plan.pre_save == 0U);
  CHECK(!ht2mp::bridge::BuildGogActorDiagnosticPlan(
      accepted_gog_report(), "steam-8138acee", 0x00400000U, plan, error));

  report = accepted_gog_report();
  auto altered_profile = *report.profile;
  auto altered_functions =
      std::vector<ht2mp::game::LegacyFunctionResearch>(
          altered_profile.legacy_multiplayer.functions.begin(),
          altered_profile.legacy_multiplayer.functions.end());
  const auto hit = std::find_if(
      altered_functions.begin(), altered_functions.end(), [](const auto& value) {
        return value.name == "hit_player";
      });
  CHECK(hit != altered_functions.end());
  if (hit != altered_functions.end()) {
    hit->calling_convention = ht2mp::game::X86CallingConvention::member_thiscall;
  }
  altered_profile.legacy_multiplayer.functions = altered_functions;
  report.profile = &altered_profile;
  CHECK(!ht2mp::bridge::BuildGogActorDiagnosticPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
  CHECK(error.find("ABI metadata") != std::string::npos);
  CHECK(plan.module_base == 0U && plan.hit_player == 0U);
}

void remote_actor_plan_requires_every_exact_symbol_and_abi() {
  auto report = accepted_gog_report();
  ht2mp::bridge::GogRemoteActorPlan plan;
  std::string error;
  CHECK(ht2mp::bridge::BuildGogRemoteActorPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
  CHECK(error.empty());
  CHECK(plan.create_new_player == 0x00415840U);
  CHECK(plan.acquire_player_id == 0x00422480U);
  CHECK(plan.announce_player == 0x004200f0U);
  CHECK(plan.process_move == 0x0041af30U);
  CHECK(plan.set_position_id == 0x004742e0U);
  CHECK(plan.kill_player == 0x00446670U);
  CHECK(plan.vehicle_instance_constructor == 0x00543530U);
  CHECK(plan.vehicle_model_registry_begin_cell == 0x0068bf38U);
  CHECK(plan.vehicle_model_registry_end_cell == 0x0068bf3cU);
  CHECK(plan.moving_item_update == 0x00582310U);
  CHECK(plan.vehicle_render_history_update == 0U);
  CHECK(plan.vehicle_set_mode == 0U);
  CHECK(plan.scene_transform_get_world == 0U);
  CHECK(plan.vehicle_ground_effects == 0U);
  CHECK(plan.vehicle_scene_node_offset == 0U);
  CHECK(plan.ai_player_list_sentinel_cell == 0x0068a8f0U);
  CHECK(plan.local_player_id_cell == 0x0068c454U);

  report.symbols.erase(
      std::remove_if(report.symbols.begin(), report.symbols.end(),
                     [](const auto& value) {
                       return value.name == "set_position_id_active";
                     }),
      report.symbols.end());
  CHECK(!ht2mp::bridge::BuildGogRemoteActorPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
  CHECK(plan.module_base == 0U);
  auto steam_report = accepted_steam_report();
  CHECK(ht2mp::bridge::BuildGogRemoteActorPlan(
      steam_report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(error.empty());
  CHECK(plan.create_new_player == 0x00415940U);
  CHECK(plan.acquire_player_id == 0x00422390U);
  CHECK(plan.announce_player == 0x00420000U);
  CHECK(plan.process_move == 0x0041ae60U);
  CHECK(plan.set_position_id == 0x00474290U);
  CHECK(plan.kill_player == 0x004466a0U);
  CHECK(plan.vehicle_instance_constructor == 0x00543e00U);
  CHECK(plan.vehicle_model_registry_begin_cell == 0x0068cfd8U);
  CHECK(plan.vehicle_model_registry_end_cell == 0x0068cfdcU);
  CHECK(plan.moving_item_handle_resolver == 0x0057c9e0U);
  CHECK(plan.moving_item_update == 0x00582c30U);
  CHECK(plan.vehicle_render_history_update == 0x00554710U);
  CHECK(plan.vehicle_set_mode == 0x0054ca60U);
  CHECK(plan.scene_transform_get_world == 0x005f25c0U);
  CHECK(plan.vehicle_ground_effects == 0x00552940U);
  CHECK(plan.vehicle_scene_node_offset == 0x154U);
  CHECK(plan.pair_collision_dispatch == 0x00581990U);
  CHECK(plan.hit_player == 0x00446d20U);
  CHECK(plan.pre_actor_registry_reset == 0x00403940U);
  CHECK(plan.pre_save == 0x004833a0U);
  CHECK(plan.ai_player_list_sentinel_cell == 0x0068b990U);
  CHECK(plan.local_player_id_cell == 0x0068d4f4U);

  steam_report.symbols.erase(
      std::remove_if(steam_report.symbols.begin(), steam_report.symbols.end(),
                     [](const auto& value) {
                       return value.name ==
                              "vehicle_render_history_update_active";
                     }),
      steam_report.symbols.end());
  CHECK(!ht2mp::bridge::BuildGogRemoteActorPlan(
      steam_report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(plan.module_base == 0U && plan.vehicle_render_history_update == 0U);

  steam_report = accepted_steam_report();
  steam_report.symbols.erase(
      std::remove_if(steam_report.symbols.begin(), steam_report.symbols.end(),
                     [](const auto& value) {
                       return value.name == "vehicle_set_mode_active";
                     }),
      steam_report.symbols.end());
  CHECK(!ht2mp::bridge::BuildGogRemoteActorPlan(
      steam_report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(plan.module_base == 0U && plan.vehicle_set_mode == 0U);
  steam_report = accepted_steam_report();
  steam_report.symbols.erase(
      std::remove_if(steam_report.symbols.begin(), steam_report.symbols.end(),
                     [](const auto& value) {
                       return value.name == "scene_transform_get_world_active";
                     }),
      steam_report.symbols.end());
  CHECK(!ht2mp::bridge::BuildGogRemoteActorPlan(
      steam_report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(plan.module_base == 0U && plan.scene_transform_get_world == 0U);
  steam_report = accepted_steam_report();
  steam_report.symbols.erase(
      std::remove_if(steam_report.symbols.begin(), steam_report.symbols.end(),
                     [](const auto& value) {
                       return value.name == "vehicle_ground_effects_active";
                     }),
      steam_report.symbols.end());
  CHECK(!ht2mp::bridge::BuildGogRemoteActorPlan(
      steam_report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(plan.module_base == 0U && plan.vehicle_ground_effects == 0U);
}

void auto_enter_requires_complete_exact_gog_menu_plan() {
  auto report = accepted_gog_report();
  ht2mp::bridge::GogAutoEnterPlan plan;
  std::string error;
  CHECK(ht2mp::bridge::BuildGogAutoEnterPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
  CHECK(error.empty());
  CHECK(plan.main_menu_activate == 0x00528000U);
  CHECK(plan.main_menu_input == 0x00527070U);
  CHECK(plan.main_menu_event == 0x00527370U);
  CHECK(plan.single_player_event == 0x0051cb60U);
  CHECK(plan.single_player_panel_cell == 0x006cdbfcU);
  CHECK(plan.event_subobject_offset == 0x40U);
  CHECK(plan.single_player_widget_id == 0x66U);
  CHECK(plan.load_widget_id == 0x6eU);

  report.symbols.pop_back();
  CHECK(!ht2mp::bridge::BuildGogAutoEnterPlan(
      report, "gog-05588140", 0x00400000U, plan, error));
  CHECK(!ht2mp::bridge::BuildGogAutoEnterPlan(
      accepted_gog_report(), "steam-8138acee", 0x00400000U, plan, error));

  auto steam_report = accepted_steam_report();
  CHECK(ht2mp::bridge::BuildGogAutoEnterPlan(
      steam_report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(plan.main_menu_activate == 0x00528820U);
  CHECK(plan.main_menu_input == 0x00527890U);
  CHECK(plan.main_menu_event == 0x00527b90U);
  CHECK(plan.single_player_event == 0x0051d380U);
  CHECK(plan.single_player_panel_cell == 0x006cec9cU);
  CHECK(plan.main_menu_vtable == 0x0064eb10U);
  CHECK(plan.main_menu_event_vtable == 0x0064ead0U);
  CHECK(plan.single_player_vtable == 0x0064e47cU);
  CHECK(plan.single_player_event_vtable == 0x0064e43cU);
}

void steam_online_world_plan_is_all_or_nothing() {
  auto report = accepted_steam_report();
  ht2mp::bridge::SteamOnlineWorldPlan plan;
  std::string error;
  CHECK(ht2mp::bridge::BuildSteamOnlineWorldPlan(
      report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(error.empty());
  CHECK(plan.ai_player_list_sentinel_cell == 0x0068b990U);
  CHECK(plan.local_player_id_cell == 0x0068d4f4U);
  CHECK(plan.current_counts_cell == 0x0068cca8U);
  CHECK(plan.target_counts_cell == 0x0068cf28U);
  CHECK(plan.create_players_regular == 0x00422f20U);
  CHECK(plan.update_ghosts == 0x00420ed0U);
  CHECK(plan.update_dealers == 0x00422370U);
  CHECK(plan.get_assortment == 0x00433350U);
  CHECK(plan.order_vehicle == 0x00433b60U);
  CHECK(plan.go_hire_it == 0x00419030U);
  CHECK(plan.focus_loss_pause_handler == 0x005dd330U);

  report.symbols.erase(
      std::remove_if(report.symbols.begin(), report.symbols.end(),
                     [](const auto& value) {
                       return value.name == "parking_order_vehicle_online";
                     }),
      report.symbols.end());
  CHECK(!ht2mp::bridge::BuildSteamOnlineWorldPlan(
      report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(plan.module_base == 0U && plan.create_players_regular == 0U &&
        plan.focus_loss_pause_handler == 0U);

  report = accepted_steam_report();
  auto altered_profile = *report.profile;
  auto altered_functions =
      std::vector<ht2mp::game::LegacyFunctionResearch>(
          altered_profile.legacy_multiplayer.functions.begin(),
          altered_profile.legacy_multiplayer.functions.end());
  const auto focus = std::find_if(
      altered_functions.begin(), altered_functions.end(),
      [](const auto& value) {
        return value.name == "focus_loss_pause_handler";
      });
  CHECK(focus != altered_functions.end());
  if (focus != altered_functions.end()) {
    focus->calling_convention =
        ht2mp::game::X86CallingConvention::caller_cleanup;
  }
  altered_profile.legacy_multiplayer.functions = altered_functions;
  report.profile = &altered_profile;
  CHECK(!ht2mp::bridge::BuildSteamOnlineWorldPlan(
      report, "steam-8138acee", 0x00400000U, plan, error));
  CHECK(error.find("focus-loss") != std::string::npos);
  CHECK(!ht2mp::bridge::BuildSteamOnlineWorldPlan(
      accepted_gog_report(), "gog-05588140", 0x00400000U, plan, error));
}

} // namespace

int main() {
  bootstrap_flags_fail_closed();
  accepts_only_complete_gog_plan();
  rejects_steam_and_incomplete_reports();
  actor_diagnostics_never_authorize_writes();
  remote_actor_plan_requires_every_exact_symbol_and_abi();
  auto_enter_requires_complete_exact_gog_menu_plan();
  steam_online_world_plan_is_all_or_nothing();
  if (failures != 0) {
    std::cerr << failures << " bridge runtime validation test(s) failed\n";
    return 1;
  }
  std::cout << "bridge runtime validation tests passed\n";
  return 0;
}
