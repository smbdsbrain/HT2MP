#include "ht2mp/game/ai_player_pool.hpp"
#include "ht2mp/game/local_world_observer.hpp"
#include "ht2mp/game/observer.hpp"
#include "ht2mp/game/pattern.hpp"
#include "ht2mp/game/pe_image.hpp"
#include "ht2mp/game/profile.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestContext {
 public:
  void Expect(bool condition, std::string_view message) {
    if (!condition) {
      ++failures_;
      std::cerr << "FAIL: " << message << '\n';
    }
  }
  [[nodiscard]] int failures() const noexcept { return failures_; }

 private:
  int failures_{};
};

class FakeMemory final : public ht2mp::game::ReadOnlyMemory {
 public:
  struct Segment {
    std::uintptr_t address{};
    std::vector<std::byte> bytes;
  };

  void Put(std::uintptr_t address, std::span<const std::byte> bytes) {
    for (auto& segment : segments_) {
      if (segment.address == address && segment.bytes.size() == bytes.size()) {
        segment.bytes.assign(bytes.begin(), bytes.end());
        return;
      }
    }
    segments_.push_back({address, std::vector<std::byte>(bytes.begin(), bytes.end())});
  }

  template <typename T>
  void PutValue(std::uintptr_t address, const T& value) {
    Put(address, std::as_bytes(std::span<const T>(&value, 1U)));
  }

  bool Read(std::uintptr_t address, std::span<std::byte> destination,
            std::string& error) noexcept override {
    if (ReadFast(address, destination)) return true;
    error = "fake address is unmapped";
    return false;
  }

  bool ReadFast(std::uintptr_t address,
                std::span<std::byte> destination) noexcept override {
    for (const auto& segment : segments_) {
      if (address < segment.address) {
        continue;
      }
      const auto offset = address - segment.address;
      if (offset <= segment.bytes.size() &&
          destination.size() <= segment.bytes.size() - offset) {
        std::memcpy(destination.data(), segment.bytes.data() + offset,
                    destination.size());
        return true;
      }
    }
    return false;
  }

 private:
  std::vector<Segment> segments_;
};

void StoreU32(std::span<std::byte> bytes, std::size_t offset,
              std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value & 0xffU);
  bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
  bytes[offset + 2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
  bytes[offset + 3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}
void StoreFloat(std::span<std::byte> bytes, std::size_t offset, float value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void StoreDouble(std::span<std::byte> bytes, std::size_t offset, double value) {
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

ht2mp::game::ProfileVerification SyntheticGogVerification() {
  ht2mp::game::ProfileVerification report;
  report.profile = ht2mp::game::FindGameProfile("gog-05588140");
  report.hash_matched = true;
  report.pe_matched = true;
  report.symbols_matched = true;
  report.symbols.push_back(
      {"local_render_transform", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::live_observed, 0U, 0x00295920U, 1U, true,
       "synthetic"});
  report.symbols.push_back(
      {"ai_player_list_sentinel_cell",
       ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0U, 0x0028a8f0U, 1U,
       true, "synthetic"});
  report.symbols.push_back(
      {"local_player_id_cell", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0U, 0x0028c454U, 1U,
       true, "synthetic"});
  report.symbols.push_back(
      {"room_registry_begin_cell", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0U, 0x0028c038U, 1U,
       true, "synthetic"});
  report.symbols.push_back(
      {"room_registry_end_cell", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0U, 0x0028c03cU, 1U,
       true, "synthetic"});
  report.symbols.push_back(
      {"ai_subsystem_initialized", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::static_analysis, 0U, 0x0028c04cU, 1U,
       true, "synthetic"});
  report.symbols.push_back(
      {"viewer_instance_cell", ht2mp::game::SymbolKind::read_only_data,
       ht2mp::game::EvidenceLevel::live_observed, 0U, 0x002d0fd8U, 1U, true,
       "synthetic"});
  return report;
}

const ht2mp::game::LegacyFunctionResearch* FindResearchFunction(
    const ht2mp::game::GameProfile& profile, std::string_view name) {
  for (const auto& function : profile.legacy_multiplayer.functions) {
    if (function.name == name) return &function;
  }
  return nullptr;
}

bool HasRuntimeSymbol(const ht2mp::game::GameProfile& profile,
                      std::string_view name) {
  for (const auto& symbol : profile.symbols) {
    if (symbol.name == name) return true;
  }
  return false;
}

void TestLegacyResearchMetadata(TestContext& test) {
  const auto* gog = ht2mp::game::FindGameProfile("gog-05588140");
  test.Expect(gog != nullptr, "find exact-GOG research profile");
  if (gog == nullptr) return;

  const auto& legacy = gog->legacy_multiplayer;
  test.Expect(legacy.message_spawn == 0x3dfU &&
                  legacy.message_state_update == 0x3e0U &&
                  legacy.message_server_claim == 0x3e2U,
              "record periodic 0x3e0 state-update semantics");
  test.Expect(legacy.ambient_traffic_actor_type == 8,
              "classify stock type 8 as ambient traffic metadata");
  test.Expect(legacy.x_controlled_flag_offset == 0x58U &&
                  legacy.x_controlled_flag_mask == 0x100U &&
                  legacy.delete_requested_flag_mask == 0x20000U &&
                  legacy.destroying_flag_mask == 0x40000U,
              "keep ownership, delete-request, and destroy flags distinct");
  test.Expect(!legacy.runtime_write_validated,
              "keep exact-GOG actor writes disabled");

  const auto& pool = gog->ai_player_pool;
  test.Expect(pool.node_player_offset + pool.player_vehicle_id_offset ==
                      0x54U &&
                  pool.node_player_offset + pool.player_liveness_marker_offset ==
                      0x58U &&
                  pool.node_player_offset + pool.player_actor_type_offset ==
                      0x5cU &&
                  pool.node_player_offset + pool.player_flags_offset == 0x60U &&
                  pool.node_player_offset + pool.player_self_id_offset == 0x68U &&
                  pool.player_vehicle_descriptor_offset == 0x1fcU &&
                  pool.vehicle_descriptor_vehicle_id_offset == 0U &&
                  pool.vehicle_descriptor_model_selector_offset == 4U &&
                  pool.vehicle_descriptor_paint_variant_offset == 8U,
              "preserve node-relative versus AI-relative field layout");

  struct ExpectedFunction {
    std::string_view name;
    std::uint32_t rva;
    ht2mp::game::X86CallingConvention convention;
  };
  constexpr std::array expected{
      ExpectedFunction{"ai_mult_message_dispatch", 0x0006d710U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"ai_player_constructor", 0x00072a30U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"insert_named_player_node", 0x00073470U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"create_new_player", 0x00015840U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"stock_create_type8_traffic_at_position", 0x00020da0U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"update_ghosts", 0x00020fc0U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"process_move", 0x0001af30U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"act_on_arrival", 0x00019d80U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"acquire_player_id", 0x00022480U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"announce_player", 0x000200f0U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"set_position_id", 0x000742e0U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"apply_serialized_player_state", 0x000747c0U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"mark_player_for_deletion", 0x00046c50U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"kill_player", 0x00046670U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"erase_player_node", 0x00073520U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"ai_tick", 0x0002d7c0U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"post_ai_callback", 0x00083a40U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"outbound_periodic_state_sender", 0x0006de00U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"vehicle_instance_constructor", 0x00143530U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"moving_item_handle_resolver", 0x0017c0a0U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"moving_item_update", 0x00182310U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"moving_pair_collision_dispatch", 0x00181070U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"moving_pair_impulse", 0x0017e1c0U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"moving_pair_event_dispatch", 0x00185480U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"hit_player", 0x00046cf0U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"clear_actor_registry", 0x00073540U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"pre_actor_registry_reset", 0x00003950U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"save_orchestrator", 0x00083470U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"save_actor_list", 0x00081160U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"save_actor_wrapper", 0x00080f80U,
                       ht2mp::game::X86CallingConvention::caller_cleanup},
      ExpectedFunction{"serialize_ai_player", 0x00075bc0U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"load_actor_wrapper", 0x00080ff0U,
                       ht2mp::game::X86CallingConvention::unknown},
      ExpectedFunction{"main_menu_activate", 0x00128000U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"main_menu_input", 0x00127070U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"main_menu_event", 0x00127370U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
      ExpectedFunction{"single_player_event", 0x0011cb60U,
                       ht2mp::game::X86CallingConvention::member_thiscall},
  };
  test.Expect(legacy.functions.size() == expected.size(),
              "publish the complete bounded exact-GOG research set");
  for (const auto& item : expected) {
    const auto* function = FindResearchFunction(*gog, item.name);
    test.Expect(function != nullptr && function->rva == item.rva &&
                    function->calling_convention == item.convention &&
                    function->evidence ==
                        ht2mp::game::EvidenceLevel::static_analysis,
                item.name);
  }
  const auto& auto_enter = gog->auto_enter_world;
  test.Expect(auto_enter.enabled &&
                  auto_enter.event_subobject_offset == 0x40U &&
                  auto_enter.single_player_widget_id == 0x66U &&
                  auto_enter.load_widget_id == 0x6eU,
              "publish bounded exact-GOG native auto-enter layout");

  constexpr std::array<std::string_view, 8> write_capable_names{
      "create_new_player",
      "stock_create_type8_traffic_at_position",
      "set_position_id",
      "apply_serialized_player_state",
      "mark_player_for_deletion",
      "kill_player",
      "erase_player_node",
      "serialize_ai_player",
  };
  for (const auto name : write_capable_names) {
    test.Expect(!HasRuntimeSymbol(*gog, name),
                "do not promote actor research into runtime symbols");
  }

  struct ExpectedDiagnostic {
    std::string_view name;
    std::uint32_t rva;
  };
  constexpr std::array diagnostics{
      ExpectedDiagnostic{"moving_item_handle_resolver_candidate", 0x0017c0a0U},
      ExpectedDiagnostic{"remote_pair_collision_filter_candidate", 0x00181070U},
      ExpectedDiagnostic{"hit_player_filter_candidate", 0x00046cf0U},
      ExpectedDiagnostic{"pre_actor_registry_reset_candidate", 0x00003950U},
      ExpectedDiagnostic{"pre_save_remote_purge_candidate", 0x00083470U},
  };
  for (const auto& item : diagnostics) {
    const auto symbol = std::find_if(
        gog->symbols.begin(), gog->symbols.end(), [&](const auto& candidate) {
          return candidate.name == item.name;
        });
    test.Expect(symbol != gog->symbols.end() &&
                    symbol->kind ==
                        ht2mp::game::SymbolKind::diagnostic_function &&
                    symbol->expected_match_rva == item.rva &&
                    symbol->expected_result_rva == item.rva,
                item.name);
  }

  const auto* steam = ht2mp::game::FindGameProfile("steam-8138acee");
  test.Expect(steam != nullptr &&
                  steam->legacy_multiplayer.functions.size() == 28U &&
                  steam->legacy_multiplayer.ambient_traffic_actor_type == 8 &&
                  steam->legacy_multiplayer.delete_requested_flag_mask == 0x20000U &&
                  !steam->legacy_multiplayer.runtime_write_validated,
              "publish independently recovered Steam actor/menu ABI metadata behind experimental opt-in");
  if (steam != nullptr) {
    constexpr std::array steam_actor_functions{
        ExpectedFunction{"create_new_player", 0x00015940U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"acquire_player_id", 0x00022390U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"announce_player", 0x00020000U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"process_move", 0x0001ae60U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"set_position_id", 0x00074290U,
                         ht2mp::game::X86CallingConvention::member_thiscall},
        ExpectedFunction{"kill_player", 0x000466a0U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"vehicle_instance_constructor", 0x00143e00U,
                         ht2mp::game::X86CallingConvention::member_thiscall},
        ExpectedFunction{"moving_item_handle_resolver", 0x0017c9e0U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"moving_item_update", 0x00182c30U,
                         ht2mp::game::X86CallingConvention::member_thiscall},
        ExpectedFunction{"vehicle_render_history_update", 0x00154710U,
                         ht2mp::game::X86CallingConvention::member_thiscall},
        ExpectedFunction{"vehicle_set_mode", 0x0014ca60U,
                         ht2mp::game::X86CallingConvention::member_thiscall},
        ExpectedFunction{"scene_transform_get_world", 0x001f25c0U,
                         ht2mp::game::X86CallingConvention::member_thiscall},
        ExpectedFunction{"vehicle_ground_effects", 0x00152940U,
                         ht2mp::game::X86CallingConvention::member_thiscall},
        ExpectedFunction{"moving_pair_collision_dispatch", 0x00181990U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"hit_player", 0x00046d20U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"pre_actor_registry_reset", 0x00003940U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"save_orchestrator", 0x000833a0U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"create_players_regular", 0x00022f20U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"update_ghosts", 0x00020ed0U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"update_dealers", 0x00022370U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"get_assortment", 0x00033350U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"order_vehicle", 0x00033b60U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"go_hire_it", 0x00019030U,
                         ht2mp::game::X86CallingConvention::caller_cleanup},
        ExpectedFunction{"focus_loss_pause_handler", 0x001dd330U,
                         ht2mp::game::X86CallingConvention::member_thiscall},
    };
    for (const auto& item : steam_actor_functions) {
      const auto* function = FindResearchFunction(*steam, item.name);
      test.Expect(function != nullptr && function->rva == item.rva &&
                      function->calling_convention == item.convention,
                  item.name);
    }
    const auto& steam_pool = steam->ai_player_pool;
    test.Expect(steam_pool.observer_enabled &&
                    steam_pool.local_player_cell_symbol == "local_player_id_cell" &&
                    steam_pool.player_vehicle_id_offset == 0x4cU &&
                    steam_pool.player_liveness_marker_offset == 0x50U &&
                    steam_pool.player_actor_type_offset == 0x54U &&
                    steam_pool.player_flags_offset == 0x58U &&
                    steam_pool.player_self_id_offset == 0x60U &&
                    steam_pool.player_world_position_offset == 0x130U &&
                    steam_pool.player_vehicle_descriptor_offset == 0x1fcU &&
                    steam_pool.vehicle_descriptor_vehicle_id_offset == 0U &&
                    steam_pool.vehicle_descriptor_model_selector_offset == 4U &&
                    steam_pool.vehicle_descriptor_paint_variant_offset == 8U &&
                    steam_pool.player_position_room_a_offset == 0x34cU &&
                    steam_pool.player_position_room_b_offset == 0x350U,
                "publish independently recovered exact-Steam read-only AI layout");
    test.Expect(steam_pool.local_vehicle.observer_enabled &&
                    steam_pool.local_vehicle.viewer_vehicle_offset == 0x268U &&
                    steam_pool.local_vehicle.vehicle_orientation_offset == 0x4ef4U &&
                    steam_pool.local_vehicle.vehicle_position_offset == 0x4f18U &&
                    steam_pool.local_vehicle.vehicle_simulation_orientation_offset == 0x204U &&
                    steam_pool.local_vehicle.vehicle_simulation_position_offset == 0x228U &&
                    steam_pool.local_vehicle.vehicle_owner_player_id_offset == 0x509cU &&
                    steam_pool.local_vehicle.vehicle_physics_offset == 0x5460U &&
                    steam_pool.local_vehicle.physics_orientation_offset == 0x10U &&
                    steam_pool.local_vehicle.physics_position_offset == 0x34U &&
                    steam_pool.local_vehicle.physics_body_to_world_orientation_offset ==
                        0x22a4U &&
                    steam_pool.local_vehicle.physics_world_to_body_orientation_offset ==
                        0x2304U,
                "publish independently recovered exact-Steam read-only Vehicle layout");
  }
}

void TestShaAndPattern(TestContext& test) {
  const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
  test.Expect(ht2mp::game::Sha256Hex(abc) ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256 of abc");
  const std::span<const std::uint8_t> empty;
  test.Expect(ht2mp::game::Sha256Hex(empty) ==
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "SHA-256 of empty input");

  ht2mp::game::MaskedPattern pattern;
  std::string error;
  test.Expect(ht2mp::game::ParseMaskedPattern("AA ?? CC", pattern, error),
              "parse masked pattern");
  const std::array<std::uint8_t, 7> bytes{0xaaU, 0x01U, 0xccU, 0xaaU,
                                          0x02U, 0xccU, 0xddU};
  const auto matches = ht2mp::game::FindAllMasked(bytes, pattern);
  test.Expect(matches.size() == 2U && matches[0] == 0U && matches[1] == 3U,
              "find all masked matches");
  test.Expect(!ht2mp::game::ParseMaskedPattern("?? ??", pattern, error),
              "reject all-wildcard pattern");
  test.Expect(!ht2mp::game::ParseMaskedPattern("A", pattern, error),
              "reject half-byte pattern");
}

void TestTransformObserver(TestContext& test) {
  constexpr std::uintptr_t kModuleBase = 0x10000000U;
  constexpr auto kTransformAddress = kModuleBase + 0x00295920U;
  auto verification = SyntheticGogVerification();
  FakeMemory memory;
  std::array<std::byte, 0x40> transform{};
  StoreFloat(transform, 0x10U, 1.0F);
  StoreFloat(transform, 0x20U, 1.0F);
  StoreFloat(transform, 0x30U, 1.0F);
  StoreFloat(transform, 0x34U, 10.0F);
  StoreFloat(transform, 0x38U, 20.0F);
  StoreFloat(transform, 0x3cU, 30.0F);
  memory.Put(kTransformAddress, transform);

  std::string error;
  auto observer = ht2mp::game::LocalTransformObserver::Create(
      verification, kModuleBase, memory, error);
  test.Expect(observer != nullptr, "create exact-GOG transform observer");
  if (!observer) {
    return;
  }
  const auto first = observer->PollFast(1'000'000U);
  test.Expect(first.has_sample(), "read valid identity transform");
  test.Expect(first.sample.position.x == 10.0 &&
                  first.sample.position.y == 20.0 &&
                  first.sample.position.z == 30.0,
              "decode transform position");
  test.Expect(std::abs(first.sample.orientation.w - 1.0F) < 0.0001F,
              "decode identity quaternion");
  test.Expect(!first.sample.velocity_valid, "first sample has no velocity");

  const auto diagnostic = observer->Poll(1'050'000U);
  test.Expect(diagnostic.has_sample() && !diagnostic.detail.empty(),
              "diagnostic poll wraps allocation-free observation");
}

void TestPositionIdDiagnostics(TestContext& test) {
  std::array<std::uint8_t, 32> raw{};
  raw[0] = 0x78U;
  raw[1] = 0x56U;
  raw[2] = 0x34U;
  raw[3] = 0x12U;
  // IEEE-754 1.0 in explicit little-endian order at PositionId bytes 8..15.
  raw[14] = 0xf0U;
  raw[15] = 0x3fU;
  raw[28] = 0xefU;
  raw[29] = 0xbeU;
  raw[30] = 0xadU;
  raw[31] = 0xdeU;

  const auto decoded = ht2mp::game::DecodePositionIdDiagnostics(raw);
  test.Expect(decoded.dwords[0] == 0x12345678U &&
                  decoded.dwords[2] == 0U &&
                  decoded.dwords[3] == 0x3ff00000U &&
                  decoded.dwords[7] == 0xdeadbeefU,
              "decode all PositionId dwords as little-endian values");
  test.Expect(decoded.distance_candidate_finite &&
                  decoded.distance_candidate == 1.0,
              "decode finite PositionId distance candidate safely");

  raw[14] = 0xf0U;
  raw[15] = 0x7fU;
  const auto infinity = ht2mp::game::DecodePositionIdDiagnostics(raw);
  test.Expect(!infinity.distance_candidate_finite &&
                  infinity.distance_candidate == 0.0,
              "reject a non-finite PositionId distance candidate");
}

void TestAiPlayerPool(TestContext& test) {
  constexpr std::uintptr_t kModuleBase = 0x10000000U;
  constexpr auto kCellAddress = kModuleBase + 0x0028a8f0U;
  constexpr auto kLocalCellAddress = kModuleBase + 0x0028c454U;
  constexpr auto kRoomBeginCellAddress = kModuleBase + 0x0028c038U;
  constexpr auto kRoomEndCellAddress = kModuleBase + 0x0028c03cU;
  constexpr auto kInitializedAddress = kModuleBase + 0x0028c04cU;
  constexpr auto kViewerCellAddress = kModuleBase + 0x002d0fd8U;
  constexpr std::uint32_t kSentinel = 0x00200000U;
  constexpr std::uint32_t kLocalNode = 0x00200100U;
  constexpr std::uint32_t kRemoteNode = 0x00200200U;
  constexpr std::uint32_t kLocalPlayer = kLocalNode + 8U;
  constexpr std::uint32_t kRemotePlayer = kRemoteNode + 8U;
  constexpr std::uint32_t kLocalNameAddress = 0x00220000U;
  constexpr std::uint32_t kRemoteNameAddress = 0x00220100U;
  constexpr std::uint32_t kViewer = 0x00230000U;
  constexpr std::uint32_t kVehicle = 0x00240000U;
  constexpr std::uint32_t kPhysics = 0x00250000U;
  constexpr std::uint32_t kVehicleDescriptor = 0x00280000U;
  constexpr std::uint32_t kRoomOne = 0x00260000U;
  constexpr std::uint32_t kRoomTwo = 0x00260100U;
  constexpr std::uint32_t kRoomOneNameAddress = 0x00270000U;
  constexpr std::uint32_t kRoomTwoNameAddress = 0x00270100U;
  constexpr std::string_view kLocalName = "HT2MP_POC";
  constexpr std::string_view kRemoteName = "$$$_AI_7";
  constexpr std::string_view kRoomOneName = "room_test_one";
  constexpr std::string_view kRoomTwoName = "room_test_two";

  auto verification = SyntheticGogVerification();
  FakeMemory memory;
  memory.PutValue(kCellAddress, kSentinel);
  memory.PutValue(kLocalCellAddress, kLocalNode);
  constexpr std::uint32_t kRoomBegin = 0x00300000U;
  constexpr std::uint32_t kRoomEnd = kRoomBegin + 3U * 4U;
  constexpr std::uint8_t kInitialized = 1U;
  memory.PutValue(kRoomBeginCellAddress, kRoomBegin);
  memory.PutValue(kRoomEndCellAddress, kRoomEnd);
  memory.PutValue(kInitializedAddress, kInitialized);
  memory.PutValue(kViewerCellAddress, kViewer);
  memory.PutValue(kViewer + 0x268U, kVehicle);
  memory.PutValue(kVehicle + 0x509cU, kLocalNode);
  memory.PutValue(kVehicle + 0x5460U, kPhysics);
  memory.PutValue(kPhysics + 0x29d4U, kVehicle + 0x10U);
  std::array<std::byte, 0x30> vehicle_transform{};
  StoreFloat(vehicle_transform, 0U, 1.0F);
  StoreFloat(vehicle_transform, 0x10U, 1.0F);
  StoreFloat(vehicle_transform, 0x20U, 1.0F);
  StoreFloat(vehicle_transform, 0x24U, 10.0F);
  StoreFloat(vehicle_transform, 0x28U, 20.0F);
  StoreFloat(vehicle_transform, 0x2cU, 30.0F);
  memory.Put(kVehicle + 0x4ef4U, vehicle_transform);
  std::array<std::byte, 64> vehicle_room_name{};
  std::memcpy(vehicle_room_name.data(), kRoomOneName.data(),
              kRoomOneName.size());
  memory.Put(kVehicle + 0x4f24U, vehicle_room_name);

  std::array<std::byte, 12> room_registry{};
  StoreU32(room_registry, 4U, kRoomOne);
  StoreU32(room_registry, 8U, kRoomTwo);
  memory.Put(kRoomBegin, room_registry);
  std::array<std::byte, 16> room_one{};
  StoreU32(room_one, 0U, kRoomOneNameAddress);
  StoreU32(room_one, 4U,
           kRoomOneNameAddress +
               static_cast<std::uint32_t>(kRoomOneName.size()) + 1U);
  StoreU32(room_one, 8U,
           kRoomOneNameAddress +
               static_cast<std::uint32_t>(kRoomOneName.size()) + 1U);
  StoreU32(room_one, 12U, 1U);
  memory.Put(kRoomOne, room_one);
  std::array<std::byte, 16> room_two{};
  StoreU32(room_two, 0U, kRoomTwoNameAddress);
  StoreU32(room_two, 4U,
           kRoomTwoNameAddress +
               static_cast<std::uint32_t>(kRoomTwoName.size()) + 1U);
  StoreU32(room_two, 8U,
           kRoomTwoNameAddress +
               static_cast<std::uint32_t>(kRoomTwoName.size()) + 1U);
  StoreU32(room_two, 12U, 2U);
  memory.Put(kRoomTwo, room_two);
  std::array<std::byte, 15> room_one_name{};
  std::memcpy(room_one_name.data(), kRoomOneName.data(), kRoomOneName.size());
  memory.Put(kRoomOneNameAddress, room_one_name);
  std::array<std::byte, 15> room_two_name{};
  std::memcpy(room_two_name.data(), kRoomTwoName.data(), kRoomTwoName.size());
  memory.Put(kRoomTwoNameAddress, room_two_name);
  std::array<std::byte, 8> sentinel{};
  StoreU32(sentinel, 0U, kLocalNode);
  StoreU32(sentinel, 4U, kRemoteNode);
  memory.Put(kSentinel, sentinel);
  std::array<std::byte, 8> local_node{};
  StoreU32(local_node, 0U, kRemoteNode);
  StoreU32(local_node, 4U, kSentinel);
  memory.Put(kLocalNode, local_node);
  std::array<std::byte, 8> remote_node{};
  StoreU32(remote_node, 0U, kSentinel);
  StoreU32(remote_node, 4U, kLocalNode);
  memory.Put(kRemoteNode, remote_node);

  std::array<std::byte, 0x354> local_player{};
  StoreU32(local_player, 0U, kLocalNameAddress);
  StoreU32(local_player, 4U,
           kLocalNameAddress + static_cast<std::uint32_t>(kLocalName.size()));
  StoreU32(local_player, 8U, kLocalNameAddress + 32U);
  StoreU32(local_player, 0x20U, 31U);
  StoreU32(local_player, 0x24U, 0xffffffffU);
  StoreDouble(local_player, 0x28U, 0.840673828125);
  StoreU32(local_player, 0x30U, 0U);
  StoreU32(local_player, 0x34U, 0U);
  StoreU32(local_player, 0x38U, 0U);
  StoreU32(local_player, 0x3cU, 0xffffffffU);
  StoreU32(local_player, 0x4cU, 23U);
  StoreU32(local_player, 0x1fcU, kVehicleDescriptor);
  StoreU32(local_player, 0x50U, 0x12345678U);
  StoreU32(local_player, 0x54U, 1U);
  StoreU32(local_player, 0x58U, 0U);
  StoreU32(local_player, 0x60U, kLocalNode);
  StoreU32(local_player, 0x34cU, 1U);
  StoreU32(local_player, 0x350U, 2U);
  memory.Put(kLocalPlayer, local_player);
  std::array<std::byte, 12> vehicle_descriptor{};
  StoreU32(vehicle_descriptor, 0U, 23U);
  StoreU32(vehicle_descriptor, 4U, 62U);
  StoreU32(vehicle_descriptor, 8U, 2U);
  memory.Put(kVehicleDescriptor, vehicle_descriptor);
  memory.Put(kLocalNameAddress,
             std::as_bytes(std::span(kLocalName.data(), kLocalName.size())));

  std::array<std::byte, 0x354> remote_player{};
  StoreU32(remote_player, 0U, kRemoteNameAddress);
  StoreU32(remote_player, 4U,
           kRemoteNameAddress + static_cast<std::uint32_t>(kRemoteName.size()));
  StoreU32(remote_player, 8U, kRemoteNameAddress + 32U);
  StoreU32(remote_player, 0x4cU, 99U);
  StoreU32(remote_player, 0x50U, 0U);
  StoreU32(remote_player, 0x54U, 8U);
  StoreU32(remote_player, 0x58U, 0x100U);
  StoreU32(remote_player, 0x60U, kRemoteNode);
  StoreU32(remote_player, 0x34cU, 0xffffffffU);
  StoreU32(remote_player, 0x350U, 0xffffffffU);
  memory.Put(kRemotePlayer, remote_player);
  memory.Put(kRemoteNameAddress,
             std::as_bytes(std::span(kRemoteName.data(), kRemoteName.size())));

  std::string error;
  auto observer = ht2mp::game::AiPlayerPoolObserver::Create(
      verification, kModuleBase, memory, error);
  test.Expect(observer != nullptr, "create exact-GOG AI player pool observer");
  if (!observer) {
    return;
  }
  const auto snapshot = observer->Snapshot();
  test.Expect(snapshot.has_snapshot(), "collect consistent AI list snapshot");
  test.Expect(snapshot.players.size() == 2U, "enumerate two AI players");
  test.Expect(snapshot.local_player_id == kLocalNode &&
                  snapshot.local_player_valid,
              "validate cached local PlayerId through list membership and type");
  test.Expect(snapshot.ai_subsystem_initialized &&
                  snapshot.room_registry_count == 3U,
              "decode bounded AI room registry lifecycle diagnostics");
  test.Expect(snapshot.local_vehicle.chain_consistent &&
                  snapshot.local_vehicle.owner_matches_local &&
                  snapshot.local_vehicle.physics_available &&
                  snapshot.local_vehicle.moving_item_address ==
                      kVehicle + 0x10U &&
                  snapshot.local_vehicle.physics_reverse_moving_item ==
                      kVehicle + 0x10U &&
                  snapshot.local_vehicle.physics_reverse_matches &&
                  snapshot.local_vehicle.position_finite &&
                  snapshot.local_vehicle.orientation_valid &&
                  std::abs(snapshot.local_vehicle.orientation.w - 1.0F) <
                      0.0001F &&
                  snapshot.local_vehicle.current_room_resolved &&
                  snapshot.local_vehicle.current_room_id == 1 &&
                  snapshot.local_vehicle.current_room_name == kRoomOneName &&
                  snapshot.local_world_ready,
              "resolve the consistent local VehicleInstance and current endpoint room");
  if (snapshot.players.size() == 2U) {
    const auto& local = snapshot.players[0];
    test.Expect(local.name_valid && local.name == kLocalName,
                "decode local PlayerId name vector");
    test.Expect(local.vehicle_id == 23 && local.actor_type == 1,
                "keep VehicleId separate from local actor type");
    test.Expect(local.constructor_marker_matches && local.selected_local &&
                    !local.x_controlled && local.self_matches_node &&
                    local.self_player_id == kLocalNode,
                "report constructor marker without treating it as dead");
    test.Expect(local.raw_position_id[31] == 0xffU,
                "copy opaque 32-byte PositionId from AI_Player+0x20");
    test.Expect(local.position_room_a == 1 && local.position_room_b == 2 &&
                    local.position_rooms_in_bounds,
                "decode PositionId endpoint RoomIds with registry bounds");

    const auto& remote = snapshot.players[1];
    test.Expect(remote.name_valid && remote.name == kRemoteName,
                "decode remote AI PlayerId name vector");
    test.Expect(remote.vehicle_id == 99 && remote.actor_type == 8,
                "keep remote VehicleId separate from ghost actor type");
    test.Expect(remote.flags == 0x100U && remote.x_controlled &&
                    !remote.selected_local && remote.self_matches_node &&
                    remote.self_player_id == kRemoteNode,
                "decode X-controlled flag without selecting remote as local");
    test.Expect(remote.position_room_a == -1 &&
                    remote.position_room_b == -1 &&
                    remote.position_rooms_in_bounds,
                 "allow reset endpoint RoomIds without declaring a current room");
  }

  auto world = ht2mp::game::LocalWorldObserver::Create(
      verification, kModuleBase, memory, error);
  test.Expect(world != nullptr,
              "create allocation-free exact-GOG local-world observer");
  if (world) {
    const auto first = world->PollFast(1'000'000U);
    test.Expect(first.has_sample() && first.sample.current_room_id == 1 &&
                    first.sample.endpoint_room_a == 1 &&
                    first.sample.endpoint_room_b == 2 &&
                    first.sample.vehicle_id == 23 &&
                    first.sample.vehicle_type == 62U &&
                    first.sample.paint_variant == 2U &&
                    first.sample.road_id == 31 &&
                    first.sample.node_id == -1 &&
                    first.sample.road_distance == 0.840673828125 &&
                    first.sample.road_segment_vector_id == 0 &&
                    first.sample.road_segment_id == 0 &&
                    first.sample.aux0 == 0 && first.sample.aux1 == -1 &&
                    first.sample.transform.position.x == 10.0 &&
                    std::abs(first.sample.transform.orientation.w - 1.0F) <
                        0.0001F &&
                    !first.sample.transform.velocity_valid,
                "publish only a fully validated local VehicleInstance sample");

    StoreFloat(vehicle_transform, 0x24U, 11.0F);
    memory.Put(kVehicle + 0x4ef4U, vehicle_transform);
    const auto moved = world->PollFast(1'050'000U);
    test.Expect(moved.has_sample() &&
                    moved.sample.transform.velocity_valid &&
                    std::abs(moved.sample.transform.linear_velocity.x - 20.0) <
                        0.001 &&
                    !moved.sample.transform.teleport,
                "derive bounded velocity from consecutive vehicle samples");
    test.Expect(moved.has_sample() &&
                    moved.sample.transform.angular_velocity_valid &&
                    std::abs(moved.sample.transform.angular_velocity.x) < 1e-6 &&
                    std::abs(moved.sample.transform.angular_velocity.y) < 1e-6 &&
                    std::abs(moved.sample.transform.angular_velocity.z) < 1e-6,
                "report zero angular velocity for an unchanged orientation");

    // Rotate the basis by 0.1 rad about the vertical axis 25 ms later: the
    // body-frame angular velocity must be 4 rad/s about that axis, whatever
    // sign convention the wire uses.
    {
      const float c = std::cos(0.1F);
      const float s = std::sin(0.1F);
      StoreFloat(vehicle_transform, 0x00U, c);
      StoreFloat(vehicle_transform, 0x04U, s);
      StoreFloat(vehicle_transform, 0x0cU, -s);
      StoreFloat(vehicle_transform, 0x10U, c);
      memory.Put(kVehicle + 0x4ef4U, vehicle_transform);
    }
    const auto rotated = world->PollFast(1'075'000U);
    test.Expect(rotated.has_sample() &&
                    rotated.sample.transform.angular_velocity_valid &&
                    std::abs(std::abs(rotated.sample.transform.angular_velocity.z) - 4.0) < 0.05 &&
                    std::abs(rotated.sample.transform.angular_velocity.x) < 0.01 &&
                    std::abs(rotated.sample.transform.angular_velocity.y) < 0.01,
                "derive body-frame angular velocity from consecutive orientations");
    {
      StoreFloat(vehicle_transform, 0x00U, 1.0F);
      StoreFloat(vehicle_transform, 0x04U, 0.0F);
      StoreFloat(vehicle_transform, 0x0cU, 0.0F);
      StoreFloat(vehicle_transform, 0x10U, 1.0F);
      memory.Put(kVehicle + 0x4ef4U, vehicle_transform);
    }
    // A valid simulation copy (+0x204/+0x228) is preferred over the delayed
    // render copy; an invalid one falls back to the render copy.
    {
      auto simulation_transform = vehicle_transform;
      StoreFloat(simulation_transform, 0x24U, 50.0F);
      memory.Put(kVehicle + 0x204U, simulation_transform);
      const auto preferred = world->PollFast(1'090'000U);
      test.Expect(preferred.has_sample() &&
                      preferred.sample.transform_from_simulation_copy &&
                      preferred.sample.transform.position.x == 50.0,
                  "publish the simulation transform copy when it is valid");
      simulation_transform.fill(std::byte{});
      memory.Put(kVehicle + 0x204U, simulation_transform);
      const auto fallback = world->PollFast(1'095'000U);
      test.Expect(fallback.has_sample() &&
                      !fallback.sample.transform_from_simulation_copy &&
                      fallback.sample.transform.position.x == 11.0,
                  "fall back to the render copy when the simulation copy is invalid");
    }

    vehicle_room_name.fill(std::byte{});
    std::memcpy(vehicle_room_name.data(), kRoomTwoName.data(),
                kRoomTwoName.size());
    memory.Put(kVehicle + 0x4f24U, vehicle_room_name);
    const auto changed_room = world->PollFast(1'100'000U);
    test.Expect(changed_room.has_sample() &&
                    changed_room.sample.current_room_id == 2 &&
                    changed_room.sample.transform.teleport,
                "reset interpolation history at a validated room change");

    StoreDouble(local_player, 0x28U,
                std::numeric_limits<double>::infinity());
    memory.Put(kLocalPlayer, local_player);
    const auto invalid_position_id = world->PollFast(1'150'000U);
    test.Expect(invalid_position_id.status ==
                    ht2mp::game::LocalWorldObservationStatus::invalid_state,
                "reject a non-finite PositionId distance");
    StoreDouble(local_player, 0x28U, 0.840673828125);
    memory.Put(kLocalPlayer, local_player);
    vehicle_room_name.fill(std::byte{});
    std::memcpy(vehicle_room_name.data(), kRoomOneName.data(),
                kRoomOneName.size());
    memory.Put(kVehicle + 0x4f24U, vehicle_room_name);

    constexpr std::uint8_t kNotInitialized = 0U;
    memory.PutValue(kInitializedAddress, kNotInitialized);
    const auto menu = world->PollFast(1'200'000U);
    test.Expect(menu.status ==
                    ht2mp::game::LocalWorldObservationStatus::not_ready,
                "hide immediately when the AI world lifecycle is not ready");
    memory.PutValue(kInitializedAddress, kInitialized);

    constexpr std::uint32_t kWrongReverse = kVehicle + 0x20U;
    memory.PutValue(kPhysics + 0x29d4U, kWrongReverse);
    const auto wrong_physics_reverse = world->PollFast(1'225'000U);
    test.Expect(wrong_physics_reverse.status ==
                    ht2mp::game::LocalWorldObservationStatus::not_ready,
                "fast observer rejects a mismatched physics reverse link");
    memory.PutValue(kPhysics + 0x29d4U, kVehicle + 0x10U);

    StoreU32(local_player, 0x60U, kRemoteNode);
    memory.Put(kLocalPlayer, local_player);
    const auto wrong_self_id = world->PollFast(1'230'000U);
    test.Expect(wrong_self_id.status ==
                    ht2mp::game::LocalWorldObservationStatus::not_ready,
                "fast observer rejects a mismatched AI_Player self PlayerId");
    StoreU32(local_player, 0x60U, kLocalNode);
    StoreU32(local_player, 0x50U, 0U);
    memory.Put(kLocalPlayer, local_player);
    const auto wrong_constructor_marker = world->PollFast(1'235'000U);
    test.Expect(wrong_constructor_marker.status ==
                    ht2mp::game::LocalWorldObservationStatus::not_ready,
                "fast observer rejects a mismatched constructor marker");
    StoreU32(local_player, 0x50U, 0x12345678U);
    memory.Put(kLocalPlayer, local_player);
  }

  constexpr std::uint32_t kOversizedRoomEnd =
      kRoomBegin + (4096U + 1U) * 4U;
  memory.PutValue(kRoomEndCellAddress, kOversizedRoomEnd);
  const auto corrupt_rooms = observer->Snapshot();
  test.Expect(corrupt_rooms.status ==
                      ht2mp::game::AiPlayerPoolStatus::corrupt_list &&
                  corrupt_rooms.players.empty(),
              "reject a room registry that exceeds the profile bound");

  memory.PutValue(kRoomEndCellAddress, kRoomEnd);
  memory.PutValue(kVehicle + 0x509cU, kRemoteNode);
  if (world) {
    const auto wrong_fast_owner = world->PollFast(1'250'000U);
    test.Expect(wrong_fast_owner.status ==
                    ht2mp::game::LocalWorldObservationStatus::not_ready,
                "fast observer rejects a vehicle owned by another PlayerId");
  }
  const auto wrong_owner = observer->Snapshot();
  test.Expect(wrong_owner.has_snapshot() &&
                  wrong_owner.local_vehicle.chain_consistent &&
                  !wrong_owner.local_vehicle.owner_matches_local &&
                  !wrong_owner.local_world_ready,
              "do not accept a VehicleInstance owned by another PlayerId");
}

void CheckExactProfile(TestContext& test, const std::filesystem::path& path,
                       std::string_view expected_profile,
                       bool expected_observer_ready,
                       std::uint32_t expected_transform_rva,
                       std::uint32_t expected_sentinel_rva,
                       std::uint32_t expected_local_player_cell_rva,
                       std::size_t expected_symbol_count) {
  if (!std::filesystem::exists(path)) {
    std::cout << "SKIP exact profile (not installed): " << path.string() << '\n';
    return;
  }
  const auto report = ht2mp::game::VerifyGameExecutable(path);
  if (!report.accepted()) {
    for (const auto& error : report.errors) {
      std::cerr << "profile error: " << error << '\n';
    }
  }
  test.Expect(report.accepted(), "exact installed executable passes profile");
  test.Expect(report.profile != nullptr && report.profile->id == expected_profile,
              "select expected exact profile");
  test.Expect(report.observer_ready() == expected_observer_ready,
              "observer readiness matches live-validation policy");
  test.Expect(report.symbols.size() == expected_symbol_count,
              "all profile-specific semantic symbols reported");
  const auto* transform = report.FindSymbol("local_render_transform");
  test.Expect(transform != nullptr && transform->accepted &&
                  transform->result_rva == expected_transform_rva,
              "resolve render transform data anchor");
  const auto* sentinel = report.FindSymbol("ai_player_list_sentinel_cell");
  test.Expect(sentinel != nullptr && sentinel->accepted &&
                  sentinel->result_rva == expected_sentinel_rva,
              "resolve AI list sentinel cell");
  const auto* local_cell = report.FindSymbol("local_player_id_cell");
  if (expected_local_player_cell_rva == 0U) {
    test.Expect(local_cell == nullptr,
                "do not infer a Steam local PlayerId symbol from GOG");
  } else {
    test.Expect(local_cell != nullptr && local_cell->accepted &&
                    local_cell->result_rva == expected_local_player_cell_rva,
                "resolve exact-build local PlayerId cell");
  }
  const auto* tick = report.FindSymbol("post_ai_tick_candidate");
  test.Expect(tick != nullptr && tick->accepted,
              "resolve diagnostic post-AI tick candidate");
}

void TestInstalledExactProfiles(TestContext& test) {
  auto gog = std::filesystem::path(
      R"(D:\Games\Files\GOG_D2\Hard Truck 2\king.exe)");
  auto steam = std::filesystem::path(
      R"(C:\Program Files (x86)\Steam\steamapps\common\Hard Truck 2 King of the Road\king.exe)");
  if (const auto* value = std::getenv("HT2MP_GOG_KING_EXE")) {
    gog = value;
  }
  if (const auto* value = std::getenv("HT2MP_STEAM_KING_EXE")) {
    steam = value;
  }
  CheckExactProfile(test, gog, "gog-05588140", true, 0x00295920U,
                    0x0028a8f0U, 0x0028c454U, 34U);
  CheckExactProfile(test, steam, "steam-8138acee", true, 0x002969c0U,
                    0x0028b990U, 0x0028d4f4U, 58U);
}

}  // namespace

int main() {
  TestContext test;
  TestLegacyResearchMetadata(test);
  TestShaAndPattern(test);
  TestTransformObserver(test);
  TestPositionIdDiagnostics(test);
  TestAiPlayerPool(test);
  TestInstalledExactProfiles(test);
  if (test.failures() != 0) {
    std::cerr << test.failures() << " game test(s) failed\n";
    return 1;
  }
  std::cout << "all game profile/observer tests passed\n";
  return 0;
}
