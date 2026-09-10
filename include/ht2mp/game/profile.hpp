#pragma once

#include "ht2mp/game/pe_image.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ht2mp::game {

enum class GameEdition : std::uint8_t { gog, steam };
enum class SymbolKind : std::uint8_t {
  read_only_data,
  writable_data,
  read_only_function,
  diagnostic_function,
};
enum class SymbolResolver : std::uint8_t {
  pattern_address,
  absolute_va32_operand,
};
enum class EvidenceLevel : std::uint8_t {
  exact_binary,
  static_analysis,
  live_observed,
};

// Research-only ABI metadata is kept separate from callable symbols. An RVA in
// this table is not permission to call the function: an active profile still
// needs a unique masked signature and the runtime actor gate must pass.
enum class X86CallingConvention : std::uint8_t {
  unknown,
  caller_cleanup,
  member_thiscall,
};

struct LegacyFunctionResearch {
  std::string_view name;
  std::uint32_t rva{};
  X86CallingConvention calling_convention{};
  std::string_view prototype;
  EvidenceLevel evidence{};
  std::string_view evidence_note;
};

struct PeSectionInvariant {
  std::string_view name;
  std::uint32_t virtual_address{};
  std::uint32_t virtual_size{};
  std::uint32_t raw_offset{};
  std::uint32_t raw_size{};
  std::uint32_t characteristics{};
};

struct PeInvariant {
  std::uint64_t file_size{};
  std::uint16_t machine{};
  std::uint16_t number_of_sections{};
  std::uint32_t timestamp{};
  std::uint16_t characteristics{};
  std::uint16_t optional_header_magic{};
  std::uint32_t entry_point_rva{};
  std::uint32_t image_base{};
  std::uint32_t section_alignment{};
  std::uint32_t file_alignment{};
  std::uint32_t size_of_image{};
  std::uint32_t size_of_headers{};
  std::uint32_t checksum{};
  std::uint16_t subsystem{};
  std::uint16_t dll_characteristics{};
  std::string_view text_raw_sha256;
  std::span<const PeSectionInvariant> sections;
};

struct SymbolDescriptor {
  std::string_view name;
  SymbolKind kind{};
  EvidenceLevel evidence{};
  std::string_view scan_section;
  std::string_view ida_pattern;
  SymbolResolver resolver{};
  // Used only by absolute_va32_operand, relative to the pattern start.
  std::uint32_t operand_offset{};
  // Applied after resolving the match/operand. It is useful when an operand
  // points into a structure rather than to its beginning.
  std::int32_t result_addend{};
  std::uint32_t expected_match_rva{};
  std::uint32_t expected_result_rva{};
  bool required{true};
  std::string_view evidence_note;
};

struct ObserverLayout {
  bool enabled{};
  std::string_view transform_symbol;
  std::uint32_t orientation_offset{};
  std::uint32_t position_offset{};
  std::uint32_t read_size{};
  float maximum_absolute_position{};
  float basis_length_tolerance{};
  float basis_dot_tolerance{};
  std::string_view evidence_note;
};

struct LocalVehicleLayout {
  bool observer_enabled{};
  // Exact-build global cell containing Viewer*. The observer follows only the
  // bounded Viewer -> VehicleInstance association described below.
  std::string_view viewer_instance_cell_symbol;
  std::uint32_t viewer_vehicle_offset{};
  std::uint32_t vehicle_owner_player_id_offset{};
  // Contiguous VehicleInstance rigid transform: a 3x3 float basis followed by
  // a Vec3f position. Offsets are relative to VehicleInstance.
  std::uint32_t vehicle_orientation_offset{};
  std::uint32_t vehicle_position_offset{};
  std::uint32_t vehicle_transform_read_size{};
  // Current VehicleInstance simulation transform. The per-frame vehicle
  // update copies this 3x3 basis + Vec3f into the render transform above.
  std::uint32_t vehicle_simulation_orientation_offset{};
  std::uint32_t vehicle_simulation_position_offset{};
  std::uint32_t vehicle_current_room_name_offset{};
  std::uint32_t vehicle_physics_offset{};
  // VehicleInstance embeds its MovingItem base and the lower physics object
  // points back to that exact subobject. This bidirectional identity is a
  // stronger lifetime check than accepting a merely non-null physics pointer.
  std::uint32_t vehicle_moving_item_offset{};
  std::uint32_t physics_vehicle_reverse_offset{};
  // Authoritative rigid transform in the lower physics object. The game
  // copies this into the VehicleInstance render transform later in its frame;
  // writing only the render copy is therefore transient.
  std::uint32_t physics_orientation_offset{};
  std::uint32_t physics_position_offset{};
  // The rigid-body integrator keeps a second body-to-world basis and its
  // transpose. Both must agree with physics_orientation_offset or the native
  // step restores the previous heading between multiplayer updates.
  std::uint32_t physics_body_to_world_orientation_offset{};
  std::uint32_t physics_world_to_body_orientation_offset{};
  // Vehicle motion mode consumed by the render-history update: 1 = physically
  // simulated (the physics body keeps integrating and fights the bridge), 2 =
  // kinematic AI mode (no physics step; the simulation copy is copied straight
  // to the render copy and the AI transform is the source of truth). With the
  // stock setPositionId re-seat suppressed and the AI transform written by the
  // bridge, mode 2 leaves the game nothing to move. Zero disables the write.
  std::uint32_t vehicle_motion_mode_offset{};
  std::uint32_t vehicle_motion_mode_simulated{};
  // Per-vehicle linear/angular velocity fields the game records into its
  // render history; stale values keep animating a pinned actor. Zero disables.
  std::uint32_t vehicle_linear_velocity_offset{};
  std::uint32_t vehicle_angular_velocity_offset{};
  // AI_Room begins with a byte-vector name and stores its numeric RoomId in the
  // same object. Only the local actor's two already-bounded endpoint rooms are
  // inspected; the observer never scans arbitrary process memory.
  std::uint32_t room_name_begin_offset{};
  std::uint32_t room_name_end_offset{};
  std::uint32_t room_name_capacity_offset{};
  std::uint32_t room_id_offset{};
  std::uint32_t maximum_room_name_bytes{};
  float maximum_absolute_position{};
  float basis_length_tolerance{};
  float basis_dot_tolerance{};
  std::string_view evidence_note;
};

struct AiPlayerPoolLayout {
  bool observer_enabled{};
  std::string_view sentinel_cell_symbol;
  // Cached PlayerId selected by the game's own local-player selector. This is
  // a pointer to the intrusive list node, not to the embedded AI_Player.
  std::string_view local_player_cell_symbol;
  // Exact-build, read-only world diagnostics. The room registry is a
  // std::vector<AI_Room*> whose indices are the numeric RoomIds stored by the
  // game. The initialized byte is an AI-subsystem lifecycle flag, not by
  // itself a wire-level in_world predicate.
  std::string_view room_registry_begin_cell_symbol;
  std::string_view room_registry_end_cell_symbol;
  std::string_view ai_subsystem_initialized_symbol;
  LocalVehicleLayout local_vehicle;
  std::uint32_t node_next_offset{};
  std::uint32_t node_previous_offset{};
  // AI_Player is embedded in the list allocation at this node-relative
  // offset; this is not a pointer field.
  std::uint32_t node_player_offset{};
  std::uint32_t player_name_begin_offset{};
  std::uint32_t player_name_end_offset{};
  std::uint32_t player_name_capacity_offset{};
  std::uint32_t player_position_offset{};
  std::uint32_t player_position_size{};
  // AI_Player's world-space double-precision cache. The active remote actor
  // backend writes this only after the exact-build PositionId setter accepts
  // the matching logical location.
  std::uint32_t player_world_position_offset{};
  // AI_Player's own 3x3 double-precision basis (row-major, same layout as the
  // vehicle float basis). The stock AI derives it from the road tangent of
  // the PositionId; AI actor models are rendered from this pair, so the
  // remote backend writes the displayed pose into it every frame. Zero
  // disables the write on profiles where it is not recovered.
  std::uint32_t player_orientation_offset{};
  // VehicleId is the live MovingItem handle, not the visual model selector.
  // Offsets here are relative to the embedded AI_Player, whereas RE notes
  // often quote the equivalent PlayerId-node-relative offsets (which are 8
  // bytes larger).
  std::uint32_t player_vehicle_id_offset{};
  std::uint32_t player_liveness_marker_offset{};
  std::uint32_t player_actor_type_offset{};
  std::uint32_t player_flags_offset{};
  std::uint32_t player_self_id_offset{};
  // updatePlayerRooms/resetPlayerRooms derive these two endpoint RoomIds from
  // the current PositionId. They are diagnostics for the containing road/node
  // endpoints; neither field alone has been proved to be "the current room".
  std::uint32_t player_position_room_a_offset{};
  std::uint32_t player_position_room_b_offset{};
  // Constructor-written diagnostic magic. Live GOG telemetry proved that a
  // selected, moving local actor still contains this value, so it must not be
  // interpreted as a generic dead/alive discriminator.
  std::uint32_t constructor_liveness_marker{};
  std::int32_t required_local_actor_type{};
  std::uint32_t x_controlled_flag_mask{};
  std::uint32_t maximum_nodes{};
  std::uint32_t maximum_rooms{};
  std::uint32_t maximum_world_location_id{};
  double maximum_road_distance{};
  // AI_Player owns a small vehicle descriptor before the VehicleInstance is
  // materialized. Its first dword mirrors VehicleId and the next dword is the
  // exact-build vehicle.tech/model-registry selector used by acquirePlayerId.
  std::uint32_t player_vehicle_descriptor_offset{};
  std::uint32_t vehicle_descriptor_vehicle_id_offset{};
  std::uint32_t vehicle_descriptor_model_selector_offset{};
  // Exact builds initialize this dword to zero in the 0x30-byte descriptor;
  // the stock appearance menu and save loader keep the four-way livery here.
  std::uint32_t vehicle_descriptor_paint_variant_offset{};
  std::uint32_t maximum_vehicle_model_selector{};
  std::uint32_t maximum_vehicle_paint_variant{};
  std::string_view evidence_note;
};

struct LegacyMultiplayerResearch {
  std::uint32_t message_spawn{};
  std::uint32_t message_state_update{};
  std::uint32_t message_server_claim{};
  // The exact GOG binary calls type 8 "ghost" internally, but its stock
  // single-player lifecycle is ambient traffic. This is not a declaration
  // that the type is collisionless or suitable for a remote actor.
  std::int32_t ambient_traffic_actor_type{};
  std::uint32_t x_controlled_flag_offset{};
  std::uint32_t x_controlled_flag_mask{};
  std::uint32_t delete_requested_flag_mask{};
  std::uint32_t destroying_flag_mask{};
  std::uint32_t player_id_size{};
  std::uint32_t position_id_size{};
  std::uint32_t player_self_id_offset{};
  std::uint32_t owner_identifier_offset{};
  std::span<const LegacyFunctionResearch> functions;
  EvidenceLevel evidence{};
  bool runtime_write_validated{};
};

// Exact-build description of the stock menu path used by the optional
// multiplayer bootstrap.  The bridge still verifies every named symbol and
// live function body before it calls anything.  Keeping the object layout and
// widget identifiers here prevents GOG addresses from leaking into the Steam
// port as an assumed delta.
struct AutoEnterWorldLayout {
  bool enabled{};
  std::string_view main_menu_activate_symbol;
  std::string_view main_menu_input_symbol;
  std::string_view main_menu_event_symbol;
  std::string_view single_player_event_symbol;
  std::string_view single_player_panel_cell_symbol;
  std::uint32_t event_subobject_offset{};
  std::uint32_t single_player_widget_id{};
  std::uint32_t load_widget_id{};
  std::uint32_t main_menu_vtable_rva{};
  std::uint32_t main_menu_event_vtable_rva{};
  std::uint32_t single_player_vtable_rva{};
  std::uint32_t single_player_event_vtable_rva{};
  std::string_view evidence_note;
};

struct GameProfile {
  std::string_view id;
  GameEdition edition{};
  std::string_view executable_name;
  std::string_view sha256;
  PeInvariant pe;
  std::span<const SymbolDescriptor> symbols;
  ObserverLayout observer;
  AiPlayerPoolLayout ai_player_pool;
  LegacyMultiplayerResearch legacy_multiplayer;
  AutoEnterWorldLayout auto_enter_world;
};

struct ResolvedSymbol {
  std::string name;
  SymbolKind kind{};
  EvidenceLevel evidence{};
  std::uint32_t match_rva{};
  std::uint32_t result_rva{};
  std::size_t match_count{};
  bool accepted{};
  std::string detail;
};

struct ProfileVerification {
  std::filesystem::path executable_path;
  std::string calculated_sha256;
  const GameProfile* profile{};
  PeMetadata pe{};
  std::vector<ResolvedSymbol> symbols;
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  bool hash_matched{};
  bool pe_matched{};
  bool symbols_matched{};

  [[nodiscard]] bool accepted() const noexcept {
    return profile != nullptr && hash_matched && pe_matched && symbols_matched &&
           errors.empty();
  }
  [[nodiscard]] bool observer_ready() const noexcept;
  [[nodiscard]] const ResolvedSymbol* FindSymbol(
      std::string_view name) const noexcept;
};

[[nodiscard]] std::span<const GameProfile> KnownGameProfiles() noexcept;
[[nodiscard]] const GameProfile* FindGameProfile(std::string_view id) noexcept;
[[nodiscard]] const GameProfile* FindGameProfileBySha256(
    std::string_view sha256) noexcept;

// Verifies the complete file hash before trusting any PE metadata or signature.
// requested_profile_id may be empty to select by SHA-256. Any missing,
// ambiguous, or shifted required symbol makes the report fail closed.
[[nodiscard]] ProfileVerification VerifyGameExecutable(
    const std::filesystem::path& executable,
    std::string_view requested_profile_id = {});

[[nodiscard]] std::string_view ToString(GameEdition value) noexcept;
[[nodiscard]] std::string_view ToString(SymbolKind value) noexcept;
[[nodiscard]] std::string_view ToString(EvidenceLevel value) noexcept;

}  // namespace ht2mp::game
