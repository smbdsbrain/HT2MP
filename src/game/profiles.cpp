#include "ht2mp/game/profile.hpp"

#include "ht2mp/game/pattern.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>

namespace ht2mp::game {
namespace {

constexpr std::array<PeSectionInvariant, 4> kGogSections{{
    {".text", 0x00001000U, 0x00248e7aU, 0x00001000U, 0x00249000U,
     0x60000020U},
    {".rdata", 0x0024a000U, 0x000201acU, 0x0024a000U, 0x00021000U,
     0x40000040U},
    {".data", 0x0026b000U, 0x00113a64U, 0x0026b000U, 0x0001f000U,
     0xc0000040U},
    {".rsrc", 0x0037f000U, 0x0003b4e0U, 0x0028a000U, 0x0003c000U,
     0x40000040U},
}};

constexpr std::array<PeSectionInvariant, 4> kSteamSections{{
    {".text", 0x00001000U, 0x0024978aU, 0x00001000U, 0x0024a000U,
     0x60000020U},
    {".rdata", 0x0024b000U, 0x000201eeU, 0x0024b000U, 0x00021000U,
     0x40000040U},
    {".data", 0x0026c000U, 0x00113b04U, 0x0026c000U, 0x0001f000U,
     0xc0000040U},
    {".rsrc", 0x00380000U, 0x0003b4e0U, 0x0028b000U, 0x0003c000U,
     0x40000040U},
}};

constexpr std::string_view kRenderTransformPattern =
    "B9 0C 00 00 00 8B F0 BF ?? ?? ?? ?? F3 A5 B9 0C 00 00 00 BE "
    "?? ?? ?? ?? BF ?? ?? ?? ?? F3 A5";

constexpr std::string_view kAiPlayerListSentinelPattern =
    "51 A1 ?? ?? ?? ?? 56 8B 08 3B C8 89 4C 24 04 74 33 51 8D 44 24 "
    "08 8B F4 51 8B CC 50 E8 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? E8 13 "
    "FE FF FF 8B 4C 24 08 83 C4 04 8B 01 8B 0D ?? ?? ?? ?? 3B C1 89 "
    "44 24 04 75 CD 5E 59 C3";

// Exact GOG setter for the cached local PlayerId. The final `89 35 imm32`
// stores the selected intrusive-list node. Absolute addresses and call
// displacements are masked; full-file fingerprinting and both expected RVAs
// are still checked before the operand is trusted.
constexpr std::string_view kGogLocalPlayerIdCellPattern =
    "A1 ?? ?? ?? ?? 56 8B 74 24 08 3B F0 74 3D 51 8B C4 89 30 E8 "
    "?? ?? ?? ?? 83 C4 04 84 C0 75 2C 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? "
    "68 ?? ?? ?? ?? 68 B5 06 00 00 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? 6A "
    "01 6A 00 E8 ?? ?? ?? ?? 83 C4 20 5E C3 89 35 ?? ?? ?? ?? 5E C3";

// A single vector-size calculation resolves both cells of the exact-GOG
// std::vector<AI_Room*>. The descriptors deliberately share this pattern and
// extract different absolute operands.
constexpr std::string_view kGogRoomRegistryCellsPattern =
    "A1 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 2B C1 33 DB 33 FF C1 F8 02 0F "
    "84 ?? ?? ?? ?? 89 5C 24 10 8B 04 B9";

// This byte gates the legacy network dispatcher. It is a useful read-only
// lifecycle signal, but is not sufficient to assert wire-level in_world.
constexpr std::string_view kGogAiSubsystemInitializedPattern =
    "81 FE DF 03 00 00 0F 8C ?? ?? ?? ?? 81 FE E7 03 00 00 0F 8F ?? "
    "?? ?? ?? A0 ?? ?? ?? ?? 84 C0 75 27";

// Exact-GOG Viewer construction stores the newly allocated instance in this
// global cell immediately after the constructor returns.
constexpr std::string_view kGogViewerInstanceCellPattern =
    "E8 ?? ?? ?? ?? EB 02 33 C0 A3 ?? ?? ?? ?? 8B B0 7C 05 00 00 81 "
    "CE 80 00 00 00";

constexpr std::string_view kAiMultDispatchPattern =
    "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 "
    "81 EC 00 04 00 00 56 8B B4 24 14 04 00 00 81 FE DF 03 00 00 0F "
    "8C ?? ?? ?? ?? 81 FE E7 03 00 00 0F 8F ?? ?? ?? ?? A0 ?? ?? ?? ??";

constexpr std::string_view kGetPositionIdPlayerPattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "83 EC 0C A1 ?? ?? ?? ?? 55 56 57 85 C0 C7 44 24 10 ?? ?? ?? ?? "
    "C7 44 24 14 ?? ?? ?? ?? 74 32 8B 0D ?? ?? ?? ?? 8B 35 ?? ?? ?? ??";

constexpr std::string_view kFindPlayerPositionPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 83 EC 74 53 56 57 6A 08 68 ?? ?? ?? ?? B9 ?? "
    "?? ?? ?? C7 44 24 40 ?? ?? ?? ?? C7 44 24 44 ?? ?? ?? ?? E8 ?? "
    "?? ?? ??";

constexpr std::string_view kGetOrientationSmoothedPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 81 EC 34 03 00 00 53 56 57 6A 08 68 ?? ?? ?? "
    "?? B9 ?? ?? ?? ?? C7 84 24 88 00 00 00 ?? ?? ?? ?? C7 84 24 8C "
    "00 00 00 ?? ?? ?? ??";

constexpr std::string_view kGetPlayerDataPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 83 EC 1C 53 56 57 6A 08 68 ?? ?? ?? ?? B9 ?? "
    "?? ?? ?? C7 44 24 1C ?? ?? ?? ?? C7 44 24 20 ?? ?? ?? ?? E8 ?? "
    "?? ?? ??";

constexpr std::string_view kGogPostAiTickPattern =
    "83 EC 08 53 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? DD 1D ?? ?? ?? ?? "
    "C6 05 ?? ?? ?? ?? 01 E8 ?? ?? ?? ?? DD 15 ?? ?? ?? ?? DC 25 "
    "?? ?? ?? ?? DD 1D ?? ?? ?? ??";

// Exact-GOG research anchors for the dedicated-actor safety gate. They remain
// diagnostic symbols: resolving one is not permission to call or detour it.
constexpr std::string_view kGogMovingItemResolverPattern =
    "8B 44 24 04 85 C0 75 01 C3 50 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? "
    "83 C4 08 C3";
constexpr std::string_view kVehicleModelRegistryCellsPattern =
    "A1 ?? ?? ?? ?? 53 55 56 57 8B 3D ?? ?? ?? ?? 2B C7 8D 4C 24 "
    "3C C1 F8 02 8B F0 8D 44 24 3B";
constexpr std::string_view kMovingItemUpdatePattern =
    "55 8B EC 83 E4 F8 81 EC 64 08 00 00 53 8B D9 56 57 8B 83 98 "
    "05 00 00 8D 8B AC 01 00 00 05 AA 00 00 00 8D B3 B8 05 00 00 "
    "68 00 40 1C 46 68 ?? ?? ?? ?? 8D 04 40 8D 14 83";
constexpr std::string_view kSteamVehicleRenderHistoryUpdatePattern =
    "53 56 57 8B D9 E8 ?? ?? ?? ?? D8 A3 B4 2A 00 00 D8 9B B0 2A "
    "00 00 DF E0 F6 C4 41 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 83 "
    "B8 2A 00 00 B9 32 00 00 00";
// VehicleInstance::setMode(int): compares +0x2aac with the requested mode,
// tears down the physics body when leaving mode 1 and, on entering mode 1,
// initialises it from the simulation copy (+0x204) and the velocity fields
// (+0x1b0/+0x1bc) before storing the mode.
constexpr std::string_view kSteamVehicleSetModePattern =
    "53 8B 5C 24 08 56 8B F1 8B 86 AC 2A 00 00 48 75 10 83 FB 01 "
    "74 0B 8B 8E 60 54 00 00 E8 ?? ?? ?? ?? 8B C3 48 74 50 48 75 "
    "42 8B 0D ?? ?? ?? ?? 8D 86 D0 50 00 00";
constexpr std::string_view kGogPairCollisionDispatchPattern =
    "81 EC 14 01 00 00 53 55 8B AC 24 24 01 00 00 56 57 8B 45 74 50 "
    "E8 ?? ?? ?? ?? 8B D8 83 C4 04 85 DB 0F 84 ?? ?? ?? ?? 8B 83 88 "
    "01 00 00 83 F8 02 74 09 83 F8 01";
constexpr std::string_view kGogHitPlayerPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 81 EC 9C 00 00 00 53 56 57 6A 08 68 ?? ?? ?? "
    "?? B9 ?? ?? ?? ?? C7 44 24 28 20 E4 66 00";
constexpr std::string_view kSteamHitPlayerPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 81 EC 9C 00 00 00 53 56 57 6A 08 68 ?? ?? ?? "
    "?? B9 ?? ?? ?? ?? C7 44 24 28 20 F4 66 00";
constexpr std::string_view kGogPreRegistryResetPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 83 EC 30 53 55 56 57 B9 37 00 00 00 33 C0 BF "
    "?? ?? ?? ?? 33 DB BD 86 EC A4 09";
constexpr std::string_view kGogPreSavePattern =
    "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 64 89 25 00 00 00 00 "
    "81 EC 24 02 00 00 8D 44 24 1C 53 56 57 68 04 01 00 00 50 6A FF "
    "33 DB";

// Exact-GOG active actor backend.  Every callable has its own unique masked
// signature; the full-file hash and expected RVA are checked before these
// symbols can be turned into runtime addresses.
constexpr std::string_view kGogCreateNewPlayerPattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "83 EC 40 53 55 56 57 6A 08 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? C7 44 "
    "24 40 ?? ?? ?? ?? C7 44 24 44 ?? ?? ?? ??";
constexpr std::string_view kGogAcquirePlayerIdPattern =
    "8B 4C 24 08 8B 81 FC 01 00 00 85 C0 74 0F 8B 40 04 8B 15 ?? "
    "?? ?? ?? 8B 04 82 FF 40 6C 8B 41 54 8B 15 ?? ?? ?? ?? 8D 04 "
    "82 8B 10 42 89 10 8B 44 24 04 8B 49 60 89 08 C3";
constexpr std::string_view kGogAnnouncePlayerPattern =
    "6A 00 6A 01 51 8D 44 24 10 8B CC 50 E8 ?? ?? ?? ?? E8 ?? ?? "
    "?? ?? 83 C4 0C 8D 54 24 04 6A 00 6A 09 51 8B CC 52 E8 ?? ?? "
    "?? ?? E8 ?? ?? ?? ?? 83 C4 0C C3";
constexpr std::string_view kGogProcessMovePattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "83 EC 14 56 6A 08 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? C7 44 24 10 ?? "
    "?? ?? ?? C7 44 24 14 ?? ?? ?? ??";
constexpr std::string_view kGogSetPositionIdPattern =
    "55 8B EC 83 E4 F8 83 EC 24 53 8B 5D 08 56 89 4C 24 08 57 B9 08 "
    "00 00 00 8B F3 8D 7C 24 10 F3 A5 8D 4C 24 10 E8 ?? ?? ?? ?? "
    "85 C0 75 0D";
constexpr std::string_view kGogKillPlayerPattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "83 EC 24 56 57 6A 08 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? C7 44 24 1C "
    "?? ?? ?? ?? C7 44 24 20 ?? ?? ?? ??";
constexpr std::string_view kGogVehicleConstructorPattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "81 EC B0 00 00 00 53 55 56 57 8B E9 68 ?? ?? ?? ?? 89 6C 24 30 "
    "E8 ?? ?? ?? ?? 8B 94 24 D4 00 00 00";

// Exact-Steam online-world boundaries. The count-table operands are resolved
// from the same unique createPlayersRegular body that consumes them, avoiding
// unanchored writable-data scans.
constexpr std::string_view kSteamCreatePlayersRegularPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 81 EC C8 02 00 00 53 55 56 57 6A 08 68 ?? ?? "
    "?? ?? B9 ?? ?? ?? ?? C7 84 24 80 00 00 00 ?? ?? ?? ?? C7 84 24 "
    "84 00 00 00 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? B9 ?? ?? "
    "?? ?? E8 ?? ?? ?? ?? 8D 44 24 30 33 ED 68 ?? ?? ?? ?? 50 89 AC "
    "24 E8 02 00 00 E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? A1 ?? ?? ?? ?? "
    "83 C4 08 8B 51 04 8B 48 04 3B D1";
constexpr std::string_view kSteamUpdateGhostsPattern =
    "55 8B EC 83 E4 F8 81 EC 40 01 00 00 A0 ?? ?? ?? ?? 53 55 56 84 "
    "C0 57 0F 84 ?? ?? ?? ?? 8D 44 24 44 50 E8 ?? ?? ?? ?? 8B 44 24 "
    "48 8B 0D ?? ?? ?? ??";
constexpr std::string_view kSteamUpdateDealersPattern =
    "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 68 00 E0 85 40 6A 00 E8 ?? ?? ?? "
    "?? 83 C4 08 C3 90 90 90 90 90 90";
constexpr std::string_view kSteamGetAssortmentPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 83 EC 4C 53 56 57 6A 08 68 ?? ?? ?? ?? B9 ?? "
    "?? ?? ?? C7 44 24 1C ?? ?? ?? ??";
constexpr std::string_view kSteamOrderVehiclePattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "81 EC 88 00 00 00 53 56 57 6A 08 68 ?? ?? ?? ?? B9 ?? ?? ?? ?? "
    "C7 44 24 4C ?? ?? ?? ?? C7 44 24 50 ?? ?? ?? ??";
constexpr std::string_view kSteamGoHireItPattern =
    "55 8B EC 83 E4 F8 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 "
    "89 25 00 00 00 00 83 EC 78 56 57 6A 08 68 ?? ?? ?? ?? B9 ?? ?? "
    "?? ?? C7 44 24 20 ?? ?? ?? ?? C7 44 24 24 ?? ?? ?? ??";
constexpr std::string_view kSteamFocusLossHandlerPattern =
    "A1 ?? ?? ?? ?? 56 85 C0 8B F1 74 36 8B 0D ?? ?? ?? ?? 8B 44 24 "
    "08 89 81 50 01 00 00 8B 15 ?? ?? ?? ?? 89 82 48 01 00 00 A1 ?? "
    "?? ?? ?? 8B 88 48 01 00 00 85 C9 74 0B 8B 88 FC 00 00 00 8B 01 "
    "FF 50 3C 8B CE E8 ?? ?? ?? ?? 5E C2 08 00";
// Stock UI path used by the explicit GOG auto-enter experiment.  The first
// hook is the main-menu activation method; the second is its Windows-message
// fallback.  Both eventually dispatch the same native widget events that a
// click would generate.  The single-player pattern is also used to decode the
// exact global cell holding that panel.
constexpr std::string_view kGogMainMenuActivatePattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "51 A1 ?? ?? ?? ?? 53 85 C0 8B D9 0F 84 ?? ?? ?? ?? 68 E0 01 00 00";
constexpr std::string_view kSteamMainMenuActivatePattern =
    "64 A1 00 00 00 00 6A FF 68 ?? ?? ?? ?? 50 A1 ?? ?? ?? ?? 64 "
    "89 25 00 00 00 00 83 EC 0C 85 C0 53 8B D9 0F 84 ?? ?? ?? ?? "
    "68 E0 01 00 00";
constexpr std::string_view kGogMainMenuInputPattern =
    "56 8B 74 24 08 57 8B F9 66 81 3E 00 02 75 ?? 0F BF 46 02 2D 90 "
    "01 00 00 99 8B C8 0F BF 46 04 33 CA 2D 47 01 00 00 2B CA 99 33 "
    "C2 2B C2 8B D0 0F AF D0 8B C1 0F AF C1 03 D0 89 54 24 0C DB "
    "44 24 0C D9 FA DC 1D ?? ?? ?? ?? DF E0 F6 C4 01 74 15";
constexpr std::string_view kGogMainMenuEventPattern =
    "8B 44 24 04 56 83 C0 9B 8B F1 83 F8 07 0F 87 ?? ?? ?? ?? FF 24 "
    "85 ?? ?? ?? ??";
constexpr std::string_view kGogSinglePlayerEventPattern =
    "8B 44 24 04 83 EC 20 83 F8 6F 57 8B F9 0F 8F ?? ?? ?? ?? 0F 84 "
    "?? ?? ?? ?? 83 E8 02 0F 84 ?? ?? ?? ?? 83 E8 6B 0F 84 ?? ?? ?? "
    "?? 48 0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ??";

// getCoordinates and getTangent have nearly identical bodies. The exact-build
// debug string operand is retained to disambiguate them; all other addresses
// remain masked. Full-file SHA verification happens before this pattern is used.
constexpr std::string_view kGogGetCoordinatesPattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "83 EC 28 A1 ?? ?? ?? ?? 53 56 57 85 C0 C7 44 24 0C 78 D6 66 00 "
    "C7 44 24 10 ?? ?? ?? ?? 74 32";
constexpr std::string_view kSteamGetCoordinatesPattern =
    "6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 "
    "83 EC 28 A1 ?? ?? ?? ?? 53 56 57 85 C0 C7 44 24 0C 78 E6 66 00 "
    "C7 44 24 10 ?? ?? ?? ?? 74 32";

constexpr std::array<SymbolDescriptor, 34> kGogSymbols{{
    {"local_render_transform", SymbolKind::read_only_data,
     EvidenceLevel::live_observed, ".text", kRenderTransformPattern,
     SymbolResolver::absolute_va32_operand, 8U, -0x10, 0x00130d30U,
     0x00295920U, true,
     "KB live capture: matrix at object+0x10, world position at object+0x34"},
    {"ai_player_list_sentinel_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kAiPlayerListSentinelPattern,
     SymbolResolver::absolute_va32_operand, 2U, 0, 0x0000a0e0U,
     0x0028a8f0U, true,
     "Ai_main intrusive-list traversals reference the same global cell"},
    {"local_player_id_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogLocalPlayerIdCellPattern,
     SymbolResolver::absolute_va32_operand, 0x4dU, 0, 0x00077ba0U,
     0x0028c454U, true,
     "exact GOG setter stores the selected PlayerId node; its selector requires actor type 1 and flags&0x100 == 0"},
    {"vehicle_model_registry_begin_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text",
     kVehicleModelRegistryCellsPattern,
     SymbolResolver::absolute_va32_operand, 11U, 0, 0x00022511U,
     0x0028bf38U, true,
     "exact-GOG std::vector<VehicleModel*> begin cell consumed by acquirePlayerId"},
    {"vehicle_model_registry_end_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text",
     kVehicleModelRegistryCellsPattern,
     SymbolResolver::absolute_va32_operand, 1U, 0, 0x00022511U,
     0x0028bf3cU, true,
     "exact-GOG model-registry end cell used to bound native selectors"},
    {"room_registry_end_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogRoomRegistryCellsPattern,
     SymbolResolver::absolute_va32_operand, 1U, 0, 0x00002c05U,
     0x0028c03cU, true,
     "exact GOG room registry vector end cell; read-only lifecycle diagnostic"},
    {"room_registry_begin_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogRoomRegistryCellsPattern,
     SymbolResolver::absolute_va32_operand, 7U, 0, 0x00002c05U,
     0x0028c038U, true,
     "exact GOG room registry vector begin cell; indices are RoomId values"},
    {"ai_subsystem_initialized", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text",
     kGogAiSubsystemInitializedPattern,
     SymbolResolver::absolute_va32_operand, 25U, 0, 0x0006d733U,
     0x0028c04cU, true,
      "exact GOG AI subsystem lifecycle byte; not by itself an in-world gate"},
    {"viewer_instance_cell", SymbolKind::read_only_data,
     EvidenceLevel::live_observed, ".text", kGogViewerInstanceCellPattern,
     SymbolResolver::absolute_va32_operand, 10U, 0, 0x000e0756U,
     0x002d0fd8U, true,
     "exact GOG Viewer* cell; live read-only chain Viewer+0x268 -> VehicleInstance validated against local PlayerId and world-scale position"},
    {"ai_mult_message_dispatch", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kAiMultDispatchPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0006d710U, 0x0006d710U,
     true, "ai_mult.cpp handler for legacy message IDs 0x3df..0x3e7"},
    {"get_position_id_player", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kGetPositionIdPlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00037600U, 0x00037600U,
     true, "debug xref: getPositionId(PlayerId)"},
    {"find_player_position", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kFindPlayerPositionPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0003d2d0U, 0x0003d2d0U,
     true, "debug xref: findPlayerPosition"},
    {"get_coordinates_position", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kGogGetCoordinatesPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00039660U, 0x00039660U,
     true, "debug xref: getCoordinates(PositionId)"},
    {"get_orientation_smoothed_player", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kGetOrientationSmoothedPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00039dc0U, 0x00039dc0U,
     true, "debug xref: getOrientationSmoothed(PlayerId)"},
    {"get_player_data", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kGetPlayerDataPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0003fd20U, 0x0003fd20U,
     true, "debug xref: getPlayerData(PlayerId, GamePlayerData)"},
    {"post_ai_tick_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogPostAiTickPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0002d7c0U, 0x0002d7c0U,
     true,
     "direct no-argument AI tick; one static caller at 0x483a43 ignores its return; invocation rate remains a live gate"},
    {"moving_item_handle_resolver_candidate",
     SymbolKind::diagnostic_function, EvidenceLevel::static_analysis, ".text",
     kGogMovingItemResolverPattern, SymbolResolver::pattern_address, 0U, 0,
     0x0017c0a0U, 0x0017c0a0U, true,
     "research-only one-argument handle resolver used by the pair-collision dispatcher; EAX carries the MovingItem pointer"},
    {"moving_item_update_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kMovingItemUpdatePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00182310U, 0x00182310U,
     true,
     "exact-GOG shared MovingItem physics update; thiscall receiver plus two stack arguments and ret 8"},
    {"remote_pair_collision_filter_candidate",
     SymbolKind::diagnostic_function, EvidenceLevel::static_analysis, ".text",
     kGogPairCollisionDispatchPattern, SymbolResolver::pattern_address, 0U, 0,
     0x00181070U, 0x00181070U, true,
     "research-only pair/wall contact dispatcher; early return is only a candidate until the full live collision and thread gate passes"},
    {"hit_player_filter_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogHitPlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00046cf0U, 0x00046cf0U,
     true,
     "research-only PlayerId damage/economy path; its sole static caller is periodic damageCargo, not physical contact"},
    {"pre_actor_registry_reset_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogPreRegistryResetPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00003950U, 0x00003950U,
     true,
     "research-only reset entry reached by both world teardown and world initialization before the actor registry is freed"},
    {"pre_save_remote_purge_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogPreSavePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00083470U, 0x00083470U,
     true,
     "research-only save entry before the unfiltered actor-list serializer; save-thread and purge ordering remain live gates"},
    {"create_new_player_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogCreateNewPlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00015840U, 0x00015840U,
     true,
     "exact-GOG allocator/registry insertion used only by the explicit dedicated-actor experiment"},
    {"acquire_player_id_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogAcquirePlayerIdPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00022480U, 0x00022480U,
     true,
     "exact-GOG post-create accounting step; increments the actor/type counters and copies AI_Player::self PlayerId"},
    {"announce_player_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogAnnouncePlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x000200f0U, 0x000200f0U,
     true,
     "exact-GOG internal appearance notifications sent after position and active flags are initialized"},
    {"process_move_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogProcessMovePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0001af30U, 0x0001af30U,
     true,
     "exact-GOG AI movement entry; the active detour suppresses it only for bridge-owned PlayerIds"},
    {"set_position_id_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogSetPositionIdPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x000742e0U, 0x000742e0U,
     true,
     "exact-GOG AI_Player PositionId setter with thiscall ABI and ret 4"},
    {"kill_player_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogKillPlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00046670U, 0x00046670U,
     true,
     "exact-GOG actor destruction path used only for bridge-owned PlayerIds"},
    {"vehicle_instance_constructor_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogVehicleConstructorPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00143530U, 0x00143530U,
     true,
     "exact-GOG VehicleInstance base constructor; pass-through detour captures owner association after construction"},
    {"main_menu_activate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogMainMenuActivatePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00128000U, 0x00128000U,
     true,
     "exact-GOG main-menu activation method; optional auto-enter detours it pass-through and dispatches only after the original returns"},
    {"main_menu_input", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogMainMenuInputPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00127070U, 0x00127070U,
     true,
     "exact-GOG main-menu message handler used only as a same-thread fallback if injection occurs after activation"},
    {"main_menu_event", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogMainMenuEventPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00127370U, 0x00127370U,
     true,
     "exact-GOG native widget dispatcher; widget 0x66 opens the single-player panel"},
    {"single_player_event", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogSinglePlayerEventPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0011cb60U, 0x0011cb60U,
     true,
     "exact-GOG single-player widget dispatcher; widget 0x6e loads the selected driver"},
    {"single_player_panel_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogSinglePlayerEventPattern,
     SymbolResolver::absolute_va32_operand, 0x33U, 0, 0x0011cb60U,
     0x002cdbfcU, true,
     "absolute operand in the verified load-event branch resolves the exact-GOG SinglePlayerPanel* cell"},
}};

constexpr std::array<SymbolDescriptor, 58> kSteamSymbols{{
    {"input_binding_query", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text",
     "8B 54 24 04 8B 84 91 08 01 00 00 85 C0 74 0E 8A 00 84 C0 74 08 25 FF 00 00 00",
     SymbolResolver::pattern_address, 0U, 0, 0x00130070U, 0x00130070U, true,
     "exact-Steam InputMap::query(int), ECX receiver, ret 4, EAX zero-extended key state; horn index 0x15 queried by Viewer at 0x53a489"},
    {"vehicle_horn_sound", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text",
     "8B 81 64 53 00 00 85 C0 74 06 A1 70 F2 6E 00 C3 8B 81 B4 28 00 00",
     SymbolResolver::pattern_address, 0U, 0, 0x00143bb0U, 0x00143bb0U, true,
     "exact-Steam VehicleInstance horn getter, ECX receiver, ret 0; selects car/truck/upgraded template without playing it"},
    {"sound_spatial_play", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text",
     "55 8B EC 83 EC 10 83 7D 08 00 75 02 EB 5F 8D 45 FC 50 8D 4D F4 51 8B 55 14",
     SymbolResolver::pattern_address, 0U, 0, 0x000e5140U, 0x000e5140U, true,
     "exact-Steam cdecl playSpatial(SoundRef*, float radius, float gain, float* listenerRelative), ret 0; used by the local horn with radius 15 and gain 1"},
    {"sound_frame_update", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text",
     "A1 28 3B 72 00 85 C0 74 63 56 57 BF A8 3A 72 00 8B 37 85 F6 74 39",
     SymbolResolver::pattern_address, 0U, 0, 0x001fd160U, 0x001fd160U, true,
     "exact-Steam cdecl sound frame maintenance, no arguments, ret 0; expires unrenewed continuous voices in the 32-entry pool then increments frame counter"},
    {"sound_stop_channel", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text",
     "8B 44 24 04 83 F8 FF 75 05 E9 A2 F2 FF FF 50 E8 BC F1 FF FF 59 C3",
     SymbolResolver::pattern_address, 0U, 0, 0x00204fe0U, 0x00204fe0U, true,
     "exact-Steam cdecl stopChannel(int), ret 0; -1 stops all voices and MUST NOT be used by a remote horn; validate channel ownership first"},
    {"local_render_transform", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kRenderTransformPattern,
     SymbolResolver::absolute_va32_operand, 8U, -0x10, 0x00131600U,
     0x002969c0U, true,
     "independent exact-Steam code xref; live transform validation still required"},
    {"ai_player_list_sentinel_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kAiPlayerListSentinelPattern,
     SymbolResolver::absolute_va32_operand, 2U, 0, 0x0000a180U,
     0x0028b990U, true,
     "independent exact-Steam intrusive-list xref"},
    {"local_player_id_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogLocalPlayerIdCellPattern,
     SymbolResolver::absolute_va32_operand, 0x4dU, 0, 0x00077b00U,
     0x0028d4f4U, true,
     "independent exact-Steam signature scan; field semantics still require live validation"},
    {"vehicle_model_registry_begin_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text",
     kVehicleModelRegistryCellsPattern,
     SymbolResolver::absolute_va32_operand, 11U, 0, 0x00022421U,
     0x0028cfd8U, true,
     "exact-Steam std::vector<VehicleModel*> begin cell consumed directly by acquirePlayerId"},
    {"vehicle_model_registry_end_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text",
     kVehicleModelRegistryCellsPattern,
     SymbolResolver::absolute_va32_operand, 1U, 0, 0x00022421U,
     0x0028cfdcU, true,
     "exact-Steam model-registry end cell used to bound synchronized native selectors"},
    {"room_registry_end_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogRoomRegistryCellsPattern,
     SymbolResolver::absolute_va32_operand, 1U, 0, 0x00002c05U,
     0x0028d0dcU, true,
     "independent exact-Steam signature scan of the room registry end cell"},
    {"room_registry_begin_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogRoomRegistryCellsPattern,
     SymbolResolver::absolute_va32_operand, 7U, 0, 0x00002c05U,
     0x0028d0d8U, true,
     "independent exact-Steam signature scan of the room registry begin cell"},
    {"ai_subsystem_initialized", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogAiSubsystemInitializedPattern,
     SymbolResolver::absolute_va32_operand, 25U, 0, 0x0006d813U,
     0x0028d0ecU, true,
     "independent exact-Steam signature scan; lifecycle semantics require live validation"},
    {"viewer_instance_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogViewerInstanceCellPattern,
     SymbolResolver::absolute_va32_operand, 10U, 0, 0x000e07f6U,
     0x002d2078U, true,
     "independent exact-Steam Viewer cell signature; no GOG address delta was used"},
    {"ai_mult_message_dispatch", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kAiMultDispatchPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0006d7f0U, 0x0006d7f0U,
     true, "exact Steam ai_mult.cpp handler"},
    {"get_position_id_player", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kGetPositionIdPlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00037520U, 0x00037520U,
     true, "independently scanned Steam body"},
    {"find_player_position", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kFindPlayerPositionPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0003d1f0U, 0x0003d1f0U,
     true, "independently scanned Steam body"},
    {"get_coordinates_position", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kSteamGetCoordinatesPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00039580U, 0x00039580U,
     true, "Steam-specific debug-string anchor disambiguates getTangent"},
    {"get_orientation_smoothed_player", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kGetOrientationSmoothedPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00039ce0U, 0x00039ce0U,
     true, "independently scanned Steam body"},
    {"get_player_data", SymbolKind::read_only_function,
     EvidenceLevel::static_analysis, ".text", kGetPlayerDataPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0003fc40U, 0x0003fc40U,
     true, "independently scanned Steam body"},
    {"post_ai_tick_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogPostAiTickPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0002d6e0U, 0x0002d6e0U,
     true,
     "independently scanned exact-Steam no-argument AI tick; its sole static caller is the 0x83970 callback; invocation rate and thread ownership remain live gates"},
    {"moving_item_handle_resolver_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogMovingItemResolverPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0017c9e0U, 0x0017c9e0U,
     true, "independent exact-Steam unique body scan; active use requires the Steam remote-actor opt-in"},
    {"moving_item_update_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kMovingItemUpdatePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00182c30U, 0x00182c30U,
     true,
     "exact-Steam MovingItem virtual update at vehicle vtable slot +0x78; thiscall receiver, two stack arguments, ret 8"},
    {"vehicle_render_history_update_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text",
     kSteamVehicleRenderHistoryUpdatePattern, SymbolResolver::pattern_address,
     0U, 0, 0x00154710U, 0x00154710U, true,
     "exact-Steam VehicleInstance render-history update; ECX is VehicleInstance, no stack arguments, and the body selects history before writing the render transform"},
    {"vehicle_ground_effects_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text",
     "55 8B EC 83 E4 F8 81 EC 90 0A 00 00 53 55 8B E9 33 DB 56 57 39 9D 90 25 00 00",
     SymbolResolver::pattern_address, 0U, 0, 0x00152940U, 0x00152940U, true,
     "exact-Steam VehicleInstance::showNic; ECX=VehicleInstance, no stack arguments, ret 0; transforms four local shadow corners at +0xf08 with simulation matrix +0x204 before ground projection"},
    {"scene_transform_get_world_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text",
     "56 8B F1 57 8B 4E 2C 85 C9 74 07 C7 46 38 02 00 00 00 8B 46 38",
     SymbolResolver::pattern_address, 0U, 0, 0x001f25c0U, 0x001f25c0U, true,
     "exact-Steam scene-node world transform getter; thiscall(node, float[12]* output), ret 4, returns transform kind; Vehicle render member +0x110 reads node at +0x44 (Vehicle+0x154) before drawing"},
    {"vehicle_set_mode_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamVehicleSetModePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0014ca60U, 0x0014ca60U, true,
     "exact-Steam VehicleInstance motion-mode switch; ECX is VehicleInstance, one stack argument (mode) and ret 4; entering mode 1 initialises the physics body from the simulation copy"},
    {"remote_pair_collision_filter_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogPairCollisionDispatchPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00181990U, 0x00181990U,
     true, "independent exact-Steam unique body scan; live collision gate required before promotion"},
    {"hit_player_filter_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamHitPlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00046d20U, 0x00046d20U,
     true,
     "exact-Steam hitPlayer entry independently recovered from its prototype string xrefs and instruction listing"},
    {"pre_actor_registry_reset_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogPreRegistryResetPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00003940U, 0x00003940U,
     true, "independent exact-Steam unique body scan; active use requires the Steam remote-actor opt-in"},
    {"pre_save_remote_purge_candidate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogPreSavePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x000833a0U, 0x000833a0U,
     true, "independent exact-Steam unique body scan; active use requires the Steam remote-actor opt-in"},
    {"create_new_player_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogCreateNewPlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00015940U, 0x00015940U,
     true, "independent exact-Steam unique body scan; called only by the gated dedicated-actor backend"},
    {"acquire_player_id_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogAcquirePlayerIdPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00022390U, 0x00022390U,
     true, "independent exact-Steam unique body scan; called only by the gated dedicated-actor backend"},
    {"announce_player_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogAnnouncePlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00020000U, 0x00020000U,
     true, "independent exact-Steam unique body scan; called only by the gated dedicated-actor backend"},
    {"process_move_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogProcessMovePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0001ae60U, 0x0001ae60U,
     true, "independent exact-Steam unique body scan; AI is suppressed only for bridge-owned PlayerIds"},
    {"set_position_id_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogSetPositionIdPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00074290U, 0x00074290U,
     true, "independent exact-Steam unique body scan; thiscall/ret-4 ABI validated before active use"},
    {"kill_player_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogKillPlayerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x000466a0U, 0x000466a0U,
     true, "independent exact-Steam unique body scan; called only for bridge-owned PlayerIds"},
    {"vehicle_instance_constructor_active", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogVehicleConstructorPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00143e00U, 0x00143e00U,
     true, "independent exact-Steam unique body scan; pass-through capture associates the spawned vehicle with its owner"},
    {"main_menu_activate", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamMainMenuActivatePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00128820U, 0x00128820U,
     true, "independent exact-Steam vtable xref and unique activation body"},
    {"main_menu_input", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogMainMenuInputPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00127890U, 0x00127890U,
     true, "independent exact-Steam unique body scan; used only by explicit native auto-enter"},
    {"main_menu_event", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogMainMenuEventPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00127b90U, 0x00127b90U,
     true, "independent exact-Steam unique body scan; used only by explicit native auto-enter"},
    {"single_player_event", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kGogSinglePlayerEventPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x0011d380U, 0x0011d380U,
     true, "independent exact-Steam unique body scan; used only by explicit native auto-enter"},
    {"single_player_panel_cell", SymbolKind::read_only_data,
     EvidenceLevel::static_analysis, ".text", kGogSinglePlayerEventPattern,
     SymbolResolver::absolute_va32_operand, 0x33U, 0, 0x0011d380U,
     0x002cec9cU, true,
     "independent exact-Steam panel-cell signature; live object/vtable checks gate native auto-enter"},
    {"create_players_regular_online", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamCreatePlayersRegularPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00022f20U, 0x00022f20U,
     true,
     "exact-Steam createPlayersRegular; sole stock count-driven creation boundary and known caller of createNewPlayer"},
    {"stock_actor_current_counts_cell", SymbolKind::writable_data,
     EvidenceLevel::static_analysis, ".text", kSteamCreatePlayersRegularPattern,
     SymbolResolver::absolute_va32_operand, 0x75U, 0, 0x00022f20U,
     0x0028cca8U, true,
     "current-count pointer cell consumed directly by createPlayersRegular"},
    {"stock_actor_target_counts_cell", SymbolKind::writable_data,
     EvidenceLevel::static_analysis, ".text", kSteamCreatePlayersRegularPattern,
     SymbolResolver::absolute_va32_operand, 0x7aU, 0, 0x00022f20U,
     0x0028cf28U, true,
     "target-count pointer cell temporarily clamped and always restored by the online detour"},
    {"update_ghosts_online", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamUpdateGhostsPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00020ed0U, 0x00020ed0U,
     true, "exact-Steam updateGhosts registered callback; owns type-8 traffic lifecycle"},
    {"update_dealers_online", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamUpdateDealersPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00022370U, 0x00022370U,
     true, "exact-Steam no-argument dealer update callback registered by name"},
    {"parking_get_assortment_online", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamGetAssortmentPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00033350U, 0x00033350U,
     true, "exact-Steam getAssortment three-argument caller-cleaned transaction gate"},
    {"parking_order_vehicle_online", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamOrderVehiclePattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00033b60U, 0x00033b60U,
     true, "exact-Steam orderVehicle entry before money and state mutation"},
    {"parking_go_hire_it_online", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamGoHireItPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x00019030U, 0x00019030U,
     true, "exact-Steam goHireIt entry before hiring state mutation"},
    {"focus_loss_pause_handler_online", SymbolKind::diagnostic_function,
     EvidenceLevel::static_analysis, ".text", kSteamFocusLossHandlerPattern,
     SymbolResolver::pattern_address, 0U, 0, 0x001dd330U, 0x001dd330U,
     true,
     "exact-Steam MFC WM_ACTIVATEAPP message-map target at VA 0x005dd330; two stack arguments and ret 8"},
    {"vehicle_get_condition", SymbolKind::diagnostic_function, EvidenceLevel::static_analysis,
     ".text", "56 57 8B 7C 24 0C 8B F1 33 C0 8B CF 8B 96 40 25 00 00 83 C0",
     SymbolResolver::pattern_address, 0U, 0, 0x00146a30U, 0x00146a30U, true,
     "int __thiscall Vehicle::get_state(float[49]); ECX=this, ret 4"},
    {"physics_delta_time", SymbolKind::diagnostic_function, EvidenceLevel::static_analysis,
     ".text", "55 8B EC 83 EC 0C 0F BF 05 C2 74 69 00 85 C0 74 18 0F BF 0D C2 74 69 00",
     SymbolResolver::pattern_address, 0U, 0, 0x000e2749U, 0x000e2749U, true,
     "float __cdecl physics_dt(); no args, x87 result, ret 0; preserves active substep override"},
    {"physics_wheel_step", SymbolKind::diagnostic_function, EvidenceLevel::static_analysis,
     ".text", "55 8B EC 83 E4 F8 81 EC 80 00 00 00 53 55 56 57 8B F9 E8 42 F1 FE FF",
     SymbolResolver::pattern_address, 0U, 0, 0x000f35f0U, 0x000f35f0U, true,
     "void __thiscall Car_V::run2(); ECX=physics, ret 0; +0xebc per-step wheel increments, +0xdd4 count"},
    {"vehicle_set_condition", SymbolKind::diagnostic_function, EvidenceLevel::static_analysis,
     ".text", "53 55 8B 6C 24 0C 56 57 8B F1 33 FF 8B DD 8B 03 8B 8E 40 25 00 00",
     SymbolResolver::pattern_address, 0U, 0, 0x00146900U, 0x00146900U, true,
     "int __thiscall Vehicle::set_state(const float[49]); ECX=this, ret 4"},
    {"vehicle_lamp_render", SymbolKind::diagnostic_function, EvidenceLevel::static_analysis,
     ".text", "56 8B F1 57 8B BE A4 51 00 00 F7 D7 C1 EF 07 83 E7 01 75 10 8B 86",
     SymbolResolver::pattern_address, 0U, 0, 0x00146d10U, 0x00146d10U, true,
     "void __thiscall Vehicle::renderLamps(); ECX=Vehicle root, ret 0; called before wheel transforms"},
    {"weather_update", SymbolKind::diagnostic_function, EvidenceLevel::static_analysis,
     ".text", "55 8B EC 83 E4 F8 83 EC 34 53 56 8B F1 57 DD 46 58",
     SymbolResolver::pattern_address, 0U, 0, 0x001a9dc0U, 0x001a9dc0U, true,
     "void __thiscall WeatherAndTime::update(); ECX=this, ret 0; singleton RVA 0x3025b8"},
}};

constexpr PeInvariant kGogPe{
    0x002c6000ULL,
    0x014cU,
    4U,
    0x3c970ff7U,
    0x010fU,
    0x010bU,
    0x0021f57aU,
    0x00400000U,
    0x1000U,
    0x1000U,
    0x003bb000U,
    0x1000U,
    0U,
    2U,
    0U,
    "c8de896a9507f117f435a078e0d6590d097f9fccc94907e17821f6b9aa08d791",
    kGogSections};

constexpr PeInvariant kSteamPe{
    0x002c7000ULL,
    0x014cU,
    4U,
    0x400502eaU,
    0x010fU,
    0x010bU,
    0x0021fecaU,
    0x00400000U,
    0x1000U,
    0x1000U,
    0x003bc000U,
    0x1000U,
    0U,
    2U,
    0U,
    "d8cf29bf132a675ea206a096b624508a393147e0795ded0014a5f80e7a38fb4b",
    kSteamSections};

constexpr ObserverLayout kGogObserver{
    true,
    "local_render_transform",
    0x10U,
    0x34U,
    0x40U,
    10'000'000.0F,
    0.45F,
    0.45F,
    "live-observed in the exact GOG build; 3x3 float basis + Vec3f position"};

constexpr ObserverLayout kSteamObserver{
    true,
    "local_render_transform",
    0x10U,
    0x34U,
    0x40U,
    10'000'000.0F,
    0.45F,
    0.45F,
    "independently resolved exact-Steam render transform; runtime values still require the read-only live gate"};

constexpr AiPlayerPoolLayout kGogAiPool{
    true,
    "ai_player_list_sentinel_cell",
    "local_player_id_cell",
    "room_registry_begin_cell",
    "room_registry_end_cell",
    "ai_subsystem_initialized",
    {true,
     "viewer_instance_cell",
     0x268U,
     0x509cU,
     0x4ef4U,
     0x4f18U,
     0x30U,
     0x204U,
     0x228U,
     0x4f24U,
     0x5460U,
     0x10U,
     0x29d4U,
     0x10U,
     0x34U,
     0x22a4U,
     0x2304U,
        0U,  // vehicle_motion_mode_offset (not recovered on GOG)
        0U,  // vehicle_motion_mode_simulated
        0U,  // vehicle_linear_velocity_offset
        0U,  // vehicle_angular_velocity_offset
     0U,
     4U,
     8U,
     0x0cU,
     64U,
     10'000'000.0F,
     0.45F,
     0.45F,
     "exact GOG live-observed Viewer -> VehicleInstance owner/rigid-transform/room-name/physics chain; lower physics+0x29d4 must point back to VehicleInstance+0x10 MovingItem; 3x3 basis at +0x4ef4 is orthonormal and position follows at +0x4f18; current numeric room is resolved only by matching the two bounded endpoint AI_Room names"},
    0U,
    4U,
    8U,
    0U,
    4U,
    8U,
    0x20U,
    32U,
    0x130U,
    0U,      // player_orientation_offset (not recovered on GOG)
    0x4cU,
    0x50U,
    0x54U,
    0x58U,
    0x60U,
    0x34cU,
    0x350U,
    0x12345678U,
    1,
    0x100U,
    512U,
    4096U,
    10'000'000U,
    10'000'000.0,
    0x1fcU,
    0U,
    4U,
    8U,
    4095U,
    3U,
    "exact GOG Ai_main list: node+0x28 PositionId, node+0x54 runtime VehicleId, node+0x58 constructor marker, node+0x5c actor type, node+0x60 flags; AI+0x1fc points to the descriptor whose +4 dword selects the vehicle model; AI+0x34c/+0x350 are read-only endpoint RoomId research fields; cached local PlayerId must remain a type-1 non-X node in this consistent list"};

constexpr AiPlayerPoolLayout kSteamAiPool{
    true,
    "ai_player_list_sentinel_cell",
    "local_player_id_cell",
    "room_registry_begin_cell",
    "room_registry_end_cell",
    "ai_subsystem_initialized",
    {true,
     "viewer_instance_cell",
     0x268U,
     0x509cU,
     0x4ef4U,
     0x4f18U,
     0x30U,
     0x204U,
     0x228U,
     0x4f24U,
     0x5460U,
     0x10U,
     0x29d4U,
     0x10U,
     0x34U,
     0x22a4U,
     0x2304U,
        0x2aacU,  // vehicle_motion_mode_offset
        1U,       // vehicle_motion_mode_simulated: physically simulated, entered via VehicleInstance::setMode
        0x1b0U,   // vehicle_linear_velocity_offset (render-history source)
        0x1bcU,   // vehicle_angular_velocity_offset (render-history source)
     0U,
     4U,
     8U,
     0x0cU,
     64U,
     10'000'000.0F,
     0.45F,
     0.45F,
     "exact-Steam static chain independently confirmed from Viewer users, Vehicle constructor and room registry constructors; live observer remains read-only until values pass all bounded invariants"},
    0U,
    4U,
    8U,
    0U,
    4U,
    8U,
    0x20U,
    32U,
    0x130U,
    0x220U,  // player_orientation_offset (AI-side double basis)
    0x4cU,
    0x50U,
    0x54U,
    0x58U,
    0x60U,
    0x34cU,
    0x350U,
    0x12345678U,
    1,
    0x100U,
    512U,
    4096U,
    10'000'000U,
    10'000'000.0,
    0x1fcU,
    0U,
    4U,
    8U,
    4095U,
    3U,
    "exact-Steam layouts independently recovered from its local selector, AI_Player constructor, PositionId setter, vehicle descriptor/model registry, room vector initialization and Vehicle/Viewer users; live read-only validation is still required before any Steam actor writes"};

// These entries describe the exact GOG image only. They are deliberately not
// part of the callable symbol set. A small subset has diagnostic masked
// signatures above, but active actor support still needs signatures for every
// callable plus the lifecycle/collision/save gates.
constexpr std::array<LegacyFunctionResearch, 36> kGogLegacyFunctions{{
    {"ai_mult_message_dispatch", 0x0006d710U,
     X86CallingConvention::caller_cleanup,
     "int __cdecl dispatch(uint32_t message_id, uint32_t arg1, const void* packet_cursor)",
     EvidenceLevel::static_analysis,
     "plain ret; first argument selects 0x3df..0x3e7; the latter two argument semantics remain incomplete"},
    {"ai_player_constructor", 0x00072a30U,
     X86CallingConvention::member_thiscall,
     "AI_Player* __thiscall AI_Player::AI_Player(const char* name_like)",
     EvidenceLevel::static_analysis,
     "ECX is the embedded AI_Player and one name-like stack argument is passed; initializes VehicleId=-1, liveness marker 0x12345678, type=-1, flags=0, and the registry sentinel self handle"},
    {"insert_named_player_node", 0x00073470U,
     X86CallingConvention::caller_cleanup,
     "AI_Player* __cdecl insertNamedPlayer(const char* name_like)",
     EvidenceLevel::static_analysis,
     "semantic prototype is provisional; calls the constructor, inserts a node in the global PlayerId list, writes AI_Player+0x60 self handle, and returns node+8"},
    {"create_new_player", 0x00015840U,
     X86CallingConvention::caller_cleanup,
     "AI_Player* __cdecl createNewPlayer(int type, int order, VehicleId veh)",
     EvidenceLevel::static_analysis,
     "prototype is present verbatim in the exact binary; type 8 enters the stock ambient-traffic branch and is not a collisionless multiplayer guarantee"},
    {"stock_create_type8_traffic_at_position", 0x00020da0U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl createType8TrafficAtPosition(PositionId position_by_value)",
     EvidenceLevel::static_analysis,
     "semantic name is provisional; body calls createNewPlayer(8, 0, -1), resolves PlayerId, and calls setPositionId; stock updateGhosts uses it for ambient traffic"},
    {"update_ghosts", 0x00020fc0U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl updateGhosts()",
     EvidenceLevel::static_analysis,
     "name literal is exact; registered only in the single-player initialization path and manages type-8 ambient traffic in the global registry"},
    {"process_move", 0x0001af30U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl processMove(PlayerId player)",
     EvidenceLevel::static_analysis,
     "prototype string is exact; main AI tick calls it without a type-8 exclusion"},
    {"act_on_arrival", 0x00019d80U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl actOnArrival(PlayerId player)",
     EvidenceLevel::static_analysis,
     "prototype string is exact; type 8 takes the traceToArbitraryDestination traffic branch, proving only arrival-side special handling"},
    {"acquire_player_id", 0x00022480U,
     X86CallingConvention::caller_cleanup,
     "PlayerId* __cdecl acquirePlayerId(PlayerId* out, AI_Player* player)",
     EvidenceLevel::static_analysis,
     "mechanically confirmed stack ABI; updates reference counters and copies AI_Player+0x60 to out; it is not the registry insertion"},
    {"announce_player", 0x000200f0U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl announcePlayer(PlayerId player)",
     EvidenceLevel::static_analysis,
     "called by every native creation path after acquirePlayerId, setPositionId, and flags|=1; emits internal event kinds 1 and 9"},
    {"set_position_id", 0x000742e0U,
     X86CallingConvention::member_thiscall,
     "void __thiscall AI_Player::setPositionId(const PositionId* value)",
     EvidenceLevel::static_analysis,
     "ECX is AI_Player, one explicit pointer argument, ret 4; validates and copies the 32-byte value"},
    {"apply_serialized_player_state", 0x000747c0U,
     X86CallingConvention::member_thiscall,
     "void __thiscall AI_Player::applySerializedState(void* packet_cursor)",
     EvidenceLevel::static_analysis,
     "semantic argument type is provisional; used by both 0x3df spawn and 0x3e0 periodic update paths; ret 4"},
    {"mark_player_for_deletion", 0x00046c50U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl markPlayerForDeletion(PlayerId player)",
     EvidenceLevel::static_analysis,
     "semantic name is provisional; validates PlayerId and sets AI_Player+0x58 bit 0x20000; updateGhosts later sweeps it"},
    {"kill_player", 0x00046670U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl killPlayer(PlayerId player)",
     EvidenceLevel::static_analysis,
     "prototype is present verbatim; bit 0x40000 is the destroy-in-progress guard; teardown reaches accounting and registry removal"},
    {"erase_player_node", 0x00073520U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl erasePlayerNode(PlayerId player)",
     EvidenceLevel::static_analysis,
     "semantic name is provisional; called by killPlayer and delegates to the unlink/destruct/free routine"},
    {"ai_tick", 0x0002d7c0U, X86CallingConvention::caller_cleanup,
     "void __cdecl aiTick()", EvidenceLevel::static_analysis,
     "no arguments, no entry ECX dependency, plain ret, and exactly one static code xref at 0x483a43; native multiplayer skips the per-player pipeline when flags contain 0x100"},
    {"post_ai_callback", 0x00083a40U,
     X86CallingConvention::member_thiscall,
     "void __thiscall postAiCallback()",
     EvidenceLevel::static_analysis,
     "vtable 0x64ade8 slot +0x14; ECX is a callback object, no stack args, plain ret; scheduler 0x5d2f70 discards EAX; calls aiTick then 0x5d29a0(1), and frequency is not live-validated"},
    {"outbound_periodic_state_sender", 0x0006de00U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl sendPeriodicPlayerState()",
     EvidenceLevel::static_analysis,
     "semantic name is provisional; aiTick calls it near its tail and it sends legacy message 0x3e0; it is outbound, not a remote-state apply boundary"},
    {"vehicle_instance_constructor", 0x00143530U,
     X86CallingConvention::member_thiscall,
     "VehicleInstance* __thiscall constructVehicle(/* four provisional stack arguments */)",
     EvidenceLevel::static_analysis,
     "ret 0x10; writes the PlayerId argument at VehicleInstance+0x509c, the lower vehicle pointer at +0x5460, and lower+0x29d4 back to VehicleInstance+0x10"},
    {"moving_item_handle_resolver", 0x0017c0a0U,
     X86CallingConvention::caller_cleanup,
     "MovingItem* __cdecl resolveMovingItem(uint32_t handle)",
     EvidenceLevel::static_analysis,
     "one stack argument and plain ret; the decompiler loses the EAX return type, but callers consume EAX as the resolved MovingItem pointer"},
    {"moving_item_update", 0x00182310U,
     X86CallingConvention::member_thiscall,
     "void __thiscall MovingItem::update(uint32_t frame, float sample_time)",
     EvidenceLevel::static_analysis,
     "shared vehicle vtable slot +0x78; ECX receiver, two stack arguments and ret 8; remote kinematic detour never changes local/non-owned calls"},
    {"moving_pair_collision_dispatch", 0x00181070U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl dispatchContact(MovingItem* current, ContactRecord* contact, uint32_t flags, float sample_time)",
     EvidenceLevel::static_analysis,
     "four stack arguments, plain ret, sole direct call at 0x5832d9; resolves contact+0x74, applies pair impulse at 0x57e1c0, then invokes virtual slot +0xc8"},
    {"moving_pair_impulse", 0x0017e1c0U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl applyPairImpulse(MovingItem* current, ContactRecord* contact, MovingItem* other)",
     EvidenceLevel::static_analysis,
     "sole direct caller is dispatchContact; mutates both MovingItem objects, but suppressing only this function would still leave the +0xc8 collision event callback"},
    {"moving_pair_event_dispatch", 0x00185480U,
     X86CallingConvention::member_thiscall,
     "void __thiscall dispatchPairEvent(MovingItem* other, ContactRecord* contact, float sample_time)",
     EvidenceLevel::static_analysis,
     "Vehicle MovingItem vtable slot +0xc8 reaches the thunk at 0x58d970 and then this function; dispatchContact early return skips the whole path"},
    {"hit_player", 0x00046cf0U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl hitPlayer(PlayerId player, double impact, int source)",
     EvidenceLevel::static_analysis,
     "prototype string is exact; 16 caller-cleaned stack bytes; only static caller 0x420c80 is periodic damageCargo, so this is a secondary gameplay filter rather than a contact-impulse filter"},
    {"clear_actor_registry", 0x00073540U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl clearActorRegistry()",
     EvidenceLevel::static_analysis,
     "plain ret; walks every node, destructs/frees it, then restores the 0x68a8f0 sentinel; sole caller is the global reset at 0x403950"},
    {"pre_actor_registry_reset", 0x00003950U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl resetAiWorldState()",
     EvidenceLevel::static_analysis,
     "no arguments and plain ret; two direct callers 0x42dc71 and 0x43487c; reaches clearActorRegistry at 0x404031 after resetting other world state"},
    {"save_orchestrator", 0x00083470U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl saveGame(void* archive_or_storage)",
     EvidenceLevel::static_analysis,
     "semantic argument type is provisional; reaches the actor-list save path"},
    {"save_actor_list", 0x00081160U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl saveActorList(void* archive)",
     EvidenceLevel::static_analysis,
     "semantic name/type is provisional; iterates every node in the global PlayerId registry with no type or flags filter"},
    {"save_actor_wrapper", 0x00080f80U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl saveActor(void* archive, AI_Player* player)",
     EvidenceLevel::static_analysis,
     "two stack arguments and plain ret; forwards the actor to its member serializer"},
    {"serialize_ai_player", 0x00075bc0U,
     X86CallingConvention::member_thiscall,
     "void __thiscall AI_Player::serialize(void* archive)",
     EvidenceLevel::static_analysis,
     "ECX is AI_Player, one archive argument, ret 4; serializes actor type at +0x54 and flags at +0x58"},
    {"load_actor_wrapper", 0x00080ff0U,
     X86CallingConvention::unknown,
     "AI_Player* loadActorFromArchive(/* provisional */)",
     EvidenceLevel::static_analysis,
     "ABI is intentionally not asserted; creates/registers an actor via 0x473470, applies serialized data via 0x4747c0, and acquires its PlayerId"},
    {"main_menu_activate", 0x00128000U,
     X86CallingConvention::member_thiscall,
     "void __thiscall MainMenu::activate()", EvidenceLevel::static_analysis,
     "no explicit stack arguments; the optional detour always calls the original first"},
    {"main_menu_input", 0x00127070U,
     X86CallingConvention::member_thiscall,
     "int __thiscall MainMenu::handleMessage(const void* message)",
     EvidenceLevel::static_analysis,
     "one stack argument and ret 4; returns one on the observed paths"},
    {"main_menu_event", 0x00127370U,
     X86CallingConvention::member_thiscall,
     "void __thiscall MainMenuEvent::dispatch(uint32_t widget_id)",
     EvidenceLevel::static_analysis,
     "one stack argument and ret 4; widget 0x66 selects the stock single-player child"},
    {"single_player_event", 0x0011cb60U,
     X86CallingConvention::member_thiscall,
     "void __thiscall SinglePlayerEvent::dispatch(uint32_t widget_id)",
     EvidenceLevel::static_analysis,
     "one stack argument and ret 4; widget 0x6e executes the stock load-selected-driver path"},
}};

constexpr std::array<LegacyFunctionResearch, 28> kSteamLegacyFunctions{{
    {"create_new_player", 0x00015940U,
     X86CallingConvention::caller_cleanup,
     "AI_Player* __cdecl createNewPlayer(int type, int order, VehicleId veh)",
     EvidenceLevel::static_analysis,
     "exact-Steam unique body; three caller-cleaned arguments confirmed by instruction-level decompilation"},
    {"acquire_player_id", 0x00022390U,
     X86CallingConvention::caller_cleanup,
     "PlayerId* __cdecl acquirePlayerId(PlayerId* out, AI_Player* player)",
     EvidenceLevel::static_analysis,
     "exact-Steam plain-ret body copies AI_Player+0x60 and performs native accounting"},
    {"announce_player", 0x00020000U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl announcePlayer(PlayerId player)",
     EvidenceLevel::static_analysis,
     "exact-Steam plain-ret appearance notification body"},
    {"process_move", 0x0001ae60U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl processMove(PlayerId player)",
     EvidenceLevel::static_analysis,
     "exact-Steam one-argument plain-ret AI movement entry"},
    {"set_position_id", 0x00074290U,
     X86CallingConvention::member_thiscall,
     "void __thiscall AI_Player::setPositionId(const PositionId* value)",
     EvidenceLevel::static_analysis,
     "exact-Steam ECX receiver, one explicit argument and ret 4"},
    {"kill_player", 0x000466a0U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl killPlayer(PlayerId player)",
     EvidenceLevel::static_analysis,
     "exact-Steam one-argument plain-ret destruction path"},
    {"vehicle_instance_constructor", 0x00143e00U,
     X86CallingConvention::member_thiscall,
     "VehicleInstance* __thiscall constructVehicle(/* four stack arguments */)",
     EvidenceLevel::static_analysis,
     "exact-Steam ECX receiver and ret 0x10; live local chain confirms owner and physics fields"},
    {"moving_item_handle_resolver", 0x0017c9e0U,
     X86CallingConvention::caller_cleanup,
     "MovingItem* __cdecl resolveMovingItem(uint32_t handle)",
     EvidenceLevel::static_analysis,
     "exact-Steam one-argument plain-ret handle resolver"},
    {"moving_item_update", 0x00182c30U,
     X86CallingConvention::member_thiscall,
     "void __thiscall MovingItem::update(uint32_t frame, float sample_time)",
     EvidenceLevel::static_analysis,
     "exact-Steam vehicle vtable slot +0x78; ECX receiver, two stack arguments and ret 8"},
    {"vehicle_render_history_update", 0x00154710U,
     X86CallingConvention::member_thiscall,
     "void __thiscall VehicleInstance::updateRenderHistory()",
     EvidenceLevel::static_analysis,
     "exact-Steam ECX receiver is VehicleInstance; no stack arguments and plain ret; sole direct runtime caller is the Vehicle MovingItem update at 0x1556a70"},
    {"vehicle_ground_effects", 0x00152940U,
     X86CallingConvention::member_thiscall,
     "void __thiscall VehicleInstance::showNic()",
     EvidenceLevel::static_analysis,
     "exact-Steam ECX receiver, no stack arguments, plain ret; submits ground-shadow corners to 0x4bb270/0x4bb350 and projected lights; simulation matrix is read, not written"},
    {"scene_transform_get_world", 0x001f25c0U,
     X86CallingConvention::member_thiscall,
     "int __thiscall SceneTransform::getWorld(float* output_matrix)",
     EvidenceLevel::static_analysis,
     "exact-Steam renderer 0x547030 reads Vehicle+0x154 then calls this getter and passes its matrix to 0x5ae580; root node matrix at +0x40, parent at +0x2c, kind at +0x38; one stack argument, ret 4"},
    {"vehicle_set_mode", 0x0014ca60U,
     X86CallingConvention::member_thiscall,
     "void __thiscall VehicleInstance::setMode(int mode)",
     EvidenceLevel::static_analysis,
     "exact-Steam ECX receiver is VehicleInstance; one stack argument and ret 4; six static callers (0x4cef60, 0x4cf080, 0x55b590)"},
    {"moving_pair_collision_dispatch", 0x00181990U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl dispatchContact(MovingItem*, ContactRecord*, uint32_t, float)",
     EvidenceLevel::static_analysis,
     "exact-Steam four-argument plain-ret contact dispatcher; contact handle remains at +0x74"},
    {"hit_player", 0x00046d20U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl hitPlayer(PlayerId player, double impact, int source)",
     EvidenceLevel::static_analysis,
     "exact-Steam prototype-string xrefs resolve this unique plain-ret body"},
    {"pre_actor_registry_reset", 0x00003940U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl resetAiWorldState()",
     EvidenceLevel::static_analysis,
     "exact-Steam no-argument plain-ret world reset entry"},
    {"save_orchestrator", 0x000833a0U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl saveGame(void* archive_or_storage)",
     EvidenceLevel::static_analysis,
     "exact-Steam unique save-orchestrator body used as the pre-save purge boundary"},
    {"main_menu_activate", 0x00128820U,
     X86CallingConvention::member_thiscall,
     "void __thiscall MainMenu::activate()", EvidenceLevel::static_analysis,
     "exact-Steam primary vtable slot; no explicit stack arguments"},
    {"main_menu_input", 0x00127890U,
     X86CallingConvention::member_thiscall,
     "int __thiscall MainMenu::handleMessage(const void* message)",
     EvidenceLevel::static_analysis,
     "exact-Steam primary vtable slot; one stack argument and ret 4"},
    {"main_menu_event", 0x00127b90U,
     X86CallingConvention::member_thiscall,
     "void __thiscall MainMenuEvent::dispatch(uint32_t widget_id)",
     EvidenceLevel::static_analysis,
     "exact-Steam event vtable slot; widget 0x66 selects Single Player"},
    {"single_player_event", 0x0011d380U,
     X86CallingConvention::member_thiscall,
     "void __thiscall SinglePlayerEvent::dispatch(uint32_t widget_id)",
     EvidenceLevel::static_analysis,
     "exact-Steam event vtable slot; widget 0x6e loads the selected driver"},
    {"create_players_regular", 0x00022f20U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl createPlayersRegular()", EvidenceLevel::static_analysis,
     "exact-Steam plain-ret no-argument stock count reconciler; directly reads current and target count arrays"},
    {"update_ghosts", 0x00020ed0U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl updateGhosts()", EvidenceLevel::static_analysis,
     "exact-Steam no-argument plain-ret type-8 lifecycle callback"},
    {"update_dealers", 0x00022370U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl updateDealers()", EvidenceLevel::static_analysis,
     "exact-Steam 26-byte no-argument plain-ret dealer lifecycle callback"},
    {"get_assortment", 0x00033350U,
     X86CallingConvention::caller_cleanup,
     "int __cdecl getAssortment(PlayerId, void** assortment, double* price)",
     EvidenceLevel::static_analysis,
     "exact-Steam three caller-cleaned arguments and boolean EAX result"},
    {"order_vehicle", 0x00033b60U,
     X86CallingConvention::caller_cleanup,
     "int __cdecl orderVehicle(PlayerId, AI_Player*, const Order*)",
     EvidenceLevel::static_analysis,
     "exact-Steam three caller-cleaned arguments; status 2 is an established pre-mutation rejection"},
    {"go_hire_it", 0x00019030U,
     X86CallingConvention::caller_cleanup,
     "void __cdecl goHireIt(PlayerId)", EvidenceLevel::static_analysis,
     "exact-Steam one caller-cleaned PlayerId argument and plain ret"},
    {"focus_loss_pause_handler", 0x001dd330U,
     X86CallingConvention::member_thiscall,
     "void __thiscall OnActivateApp(BOOL active, DWORD other_thread)",
     EvidenceLevel::static_analysis,
     "exact-Steam WM_ACTIVATEAPP message-map entry at VA 0x00651f20 points here; ret 8 confirms both arguments"},
}};

constexpr std::array<GameProfile, 2> kProfiles{{
    {"gog-05588140",
     GameEdition::gog,
     "king.exe",
     "4412a5f695dd016c9f92185b7d2d0be8ad3be787bfe2d77908f5b4d171eedd86",
     kGogPe,
     kGogSymbols,
     kGogObserver,
     kGogAiPool,
     {0x3dfU, 0x3e0U, 0x3e2U, 8, 0x58U, 0x100U, 0x20000U,
      0x40000U, 4U, 32U,
      0x60U, 0x2c8U, kGogLegacyFunctions,
      EvidenceLevel::static_analysis, false},
     {true, "main_menu_activate", "main_menu_input", "main_menu_event",
      "single_player_event", "single_player_panel_cell", 0x40U, 0x66U,
      0x6eU, 0x0024db00U, 0x0024dac0U, 0x0024d46cU, 0x0024d42cU,
      "exact-GOG native menu path: MainMenu 0x66 -> SinglePlayerPanel 0x6e; runtime still requires one unambiguous staged .pl1"}},
    {"steam-8138acee",
     GameEdition::steam,
     "king.exe",
     "8138aceebfd67b9ed3d8e1d209a34ded92075c5627a19dc6ccbb32fb0e4c3d36",
     kSteamPe,
     kSteamSymbols,
     kSteamObserver,
     kSteamAiPool,
     {0x3dfU, 0x3e0U, 0x3e2U, 8, 0x58U, 0x100U, 0x20000U,
      0x40000U, 4U, 32U,
      0x60U, 0x2c8U, kSteamLegacyFunctions,
      EvidenceLevel::static_analysis, false},
     {true, "main_menu_activate", "main_menu_input", "main_menu_event",
      "single_player_event", "single_player_panel_cell", 0x40U, 0x66U,
      0x6eU, 0x0024eb10U, 0x0024ead0U, 0x0024e47cU, 0x0024e43cU,
      "exact-Steam native menu path independently recovered from constructor-written vtables and exact event bodies; runtime still requires one unambiguous staged .pl1"}},
}};

bool EqualsCaseInsensitive(std::string_view left, std::string_view right) noexcept {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](char a, char b) {
                      return std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b));
                    });
}

std::string Hex(std::uint64_t value) {
  std::ostringstream output;
  output << "0x" << std::hex << value;
  return output.str();
}

template <typename T>
void CompareInvariant(std::string_view name, T actual, T expected,
                      std::vector<std::string>& errors) {
  if (actual != expected) {
    errors.emplace_back(std::string(name) + " mismatch: got " +
                        Hex(static_cast<std::uint64_t>(actual)) + ", expected " +
                        Hex(static_cast<std::uint64_t>(expected)));
  }
}

bool VerifyPeInvariants(const PeImage& image, const GameProfile& profile,
                        std::vector<std::string>& errors) {
  const auto before = errors.size();
  const auto& actual = image.metadata();
  const auto& expected = profile.pe;
  CompareInvariant("file size", image.file_size(), expected.file_size, errors);
  CompareInvariant("machine", actual.machine, expected.machine, errors);
  CompareInvariant("number of sections", actual.number_of_sections,
                   expected.number_of_sections, errors);
  CompareInvariant("timestamp", actual.timestamp, expected.timestamp, errors);
  CompareInvariant("COFF characteristics", actual.characteristics,
                   expected.characteristics, errors);
  CompareInvariant("optional-header magic", actual.optional_header_magic,
                   expected.optional_header_magic, errors);
  CompareInvariant("entry RVA", actual.entry_point_rva, expected.entry_point_rva,
                   errors);
  CompareInvariant("image base", actual.image_base, expected.image_base, errors);
  CompareInvariant("section alignment", actual.section_alignment,
                   expected.section_alignment, errors);
  CompareInvariant("file alignment", actual.file_alignment, expected.file_alignment,
                   errors);
  CompareInvariant("SizeOfImage", actual.size_of_image, expected.size_of_image,
                   errors);
  CompareInvariant("SizeOfHeaders", actual.size_of_headers,
                   expected.size_of_headers, errors);
  CompareInvariant("checksum", actual.checksum, expected.checksum, errors);
  CompareInvariant("subsystem", actual.subsystem, expected.subsystem, errors);
  CompareInvariant("DLL characteristics", actual.dll_characteristics,
                   expected.dll_characteristics, errors);

  for (const auto& wanted : expected.sections) {
    const auto* section = image.FindSection(wanted.name);
    if (section == nullptr) {
      errors.emplace_back("missing section " + std::string(wanted.name));
      continue;
    }
    const auto prefix = std::string("section ") + std::string(wanted.name) + " ";
    CompareInvariant(prefix + "virtual address", section->virtual_address,
                     wanted.virtual_address, errors);
    CompareInvariant(prefix + "virtual size", section->virtual_size,
                     wanted.virtual_size, errors);
    CompareInvariant(prefix + "raw offset", section->raw_offset, wanted.raw_offset,
                     errors);
    CompareInvariant(prefix + "raw size", section->raw_size, wanted.raw_size,
                     errors);
    CompareInvariant(prefix + "characteristics", section->characteristics,
                     wanted.characteristics, errors);
  }

  const auto text = image.RawSection(".text");
  if (!text.has_value()) {
    errors.emplace_back("cannot hash missing .text section");
  } else {
    const auto hash = Sha256Hex(*text);
    if (!EqualsCaseInsensitive(hash, expected.text_raw_sha256)) {
      errors.emplace_back(".text SHA-256 mismatch: got " + hash);
    }
  }
  return errors.size() == before;
}

std::uint32_t ReadLittleU32(std::span<const std::uint8_t> bytes,
                            std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

ResolvedSymbol ResolveSymbol(const PeImage& image,
                             const SymbolDescriptor& descriptor) {
  ResolvedSymbol result;
  result.name = descriptor.name;
  result.kind = descriptor.kind;
  result.evidence = descriptor.evidence;

  const auto* section = image.FindSection(descriptor.scan_section);
  const auto section_bytes = image.RawSection(descriptor.scan_section);
  if (section == nullptr || !section_bytes.has_value()) {
    result.detail = "scan section is unavailable";
    return result;
  }
  MaskedPattern pattern;
  std::string parse_error;
  if (!ParseMaskedPattern(descriptor.ida_pattern, pattern, parse_error)) {
    result.detail = "invalid built-in pattern: " + parse_error;
    return result;
  }
  const auto matches = FindAllMasked(*section_bytes, pattern, 2U);
  result.match_count = matches.size();
  if (matches.size() != 1U) {
    result.detail = matches.empty() ? "pattern not found"
                                    : "pattern is ambiguous (at least two matches)";
    return result;
  }

  const auto match_offset = matches.front();
  const auto match_rva64 = static_cast<std::uint64_t>(section->virtual_address) +
                           static_cast<std::uint64_t>(match_offset);
  if (match_rva64 > std::numeric_limits<std::uint32_t>::max()) {
    result.detail = "pattern RVA overflows PE32";
    return result;
  }
  result.match_rva = static_cast<std::uint32_t>(match_rva64);
  if (result.match_rva != descriptor.expected_match_rva) {
    result.detail = "unique pattern shifted to " + Hex(result.match_rva) +
                    "; expected " + Hex(descriptor.expected_match_rva);
    return result;
  }

  std::int64_t resolved = result.match_rva;
  if (descriptor.resolver == SymbolResolver::absolute_va32_operand) {
    const auto operand = static_cast<std::size_t>(descriptor.operand_offset);
    if (operand > pattern.size() || pattern.size() - operand < 4U ||
        match_offset > section_bytes->size() ||
        operand + 4U > section_bytes->size() - match_offset) {
      result.detail = "absolute operand is outside the matched bytes";
      return result;
    }
    const auto absolute = ReadLittleU32(*section_bytes, match_offset + operand);
    if (absolute < image.metadata().image_base) {
      result.detail = "absolute operand lies below ImageBase";
      return result;
    }
    resolved = static_cast<std::int64_t>(absolute - image.metadata().image_base);
  }
  resolved += descriptor.result_addend;
  if (resolved < 0 || resolved > std::numeric_limits<std::uint32_t>::max()) {
    result.detail = "resolved RVA overflows PE32";
    return result;
  }
  result.result_rva = static_cast<std::uint32_t>(resolved);
  if (result.result_rva != descriptor.expected_result_rva) {
    result.detail = "resolved RVA is " + Hex(result.result_rva) + ", expected " +
                    Hex(descriptor.expected_result_rva);
    return result;
  }
  if (image.FindSectionForRva(result.result_rva) == nullptr) {
    result.detail = "resolved RVA is outside mapped sections";
    return result;
  }
  result.accepted = true;
  result.detail = std::string(descriptor.evidence_note);
  return result;
}

}  // namespace

bool ProfileVerification::observer_ready() const noexcept {
  if (!accepted() || !profile->observer.enabled) {
    return false;
  }
  const auto* symbol = FindSymbol(profile->observer.transform_symbol);
  return symbol != nullptr && symbol->accepted;
}

const ResolvedSymbol* ProfileVerification::FindSymbol(
    std::string_view name) const noexcept {
  const auto found = std::find_if(
      symbols.begin(), symbols.end(),
      [name](const ResolvedSymbol& symbol) { return symbol.name == name; });
  return found == symbols.end() ? nullptr : &*found;
}

std::span<const GameProfile> KnownGameProfiles() noexcept { return kProfiles; }

const GameProfile* FindGameProfile(std::string_view id) noexcept {
  const auto found = std::find_if(kProfiles.begin(), kProfiles.end(),
                                  [id](const GameProfile& profile) {
                                    return profile.id == id;
                                  });
  return found == kProfiles.end() ? nullptr : &*found;
}

const GameProfile* FindGameProfileBySha256(std::string_view sha256) noexcept {
  const auto found = std::find_if(
      kProfiles.begin(), kProfiles.end(), [sha256](const GameProfile& profile) {
        return EqualsCaseInsensitive(profile.sha256, sha256);
      });
  return found == kProfiles.end() ? nullptr : &*found;
}

ProfileVerification VerifyGameExecutable(
    const std::filesystem::path& executable,
    std::string_view requested_profile_id) {
  ProfileVerification report;
  report.executable_path = executable;

  PeImage image;
  std::string load_error;
  if (!PeImage::Load(executable, image, load_error)) {
    report.errors.emplace_back("PE load failed: " + load_error);
    return report;
  }
  report.pe = image.metadata();
  report.calculated_sha256 = Sha256Hex(image.bytes());

  if (!requested_profile_id.empty()) {
    report.profile = FindGameProfile(requested_profile_id);
    if (report.profile == nullptr) {
      report.errors.emplace_back("unknown requested profile: " +
                                 std::string(requested_profile_id));
      return report;
    }
  } else {
    report.profile = FindGameProfileBySha256(report.calculated_sha256);
    if (report.profile == nullptr) {
      report.errors.emplace_back("unknown king.exe SHA-256: " +
                                 report.calculated_sha256);
      return report;
    }
  }

  report.hash_matched = EqualsCaseInsensitive(report.calculated_sha256,
                                              report.profile->sha256);
  if (!report.hash_matched) {
    report.errors.emplace_back("full-file SHA-256 does not match profile " +
                               std::string(report.profile->id));
  }
  if (!EqualsCaseInsensitive(executable.filename().string(),
                             report.profile->executable_name)) {
    report.errors.emplace_back("executable must be named " +
                               std::string(report.profile->executable_name));
  }

  report.pe_matched = VerifyPeInvariants(image, *report.profile, report.errors);
  if (!report.hash_matched || !report.pe_matched) {
    report.errors.emplace_back(
        "symbol resolution skipped because fingerprint verification failed");
    return report;
  }

  bool required_symbols_ok = true;
  report.symbols.reserve(report.profile->symbols.size());
  for (const auto& descriptor : report.profile->symbols) {
    auto resolved = ResolveSymbol(image, descriptor);
    if (descriptor.required && !resolved.accepted) {
      required_symbols_ok = false;
      report.errors.emplace_back("symbol " + std::string(descriptor.name) + ": " +
                                 resolved.detail);
    } else if (!resolved.accepted) {
      report.warnings.emplace_back("optional symbol " +
                                   std::string(descriptor.name) + ": " +
                                   resolved.detail);
    }
    report.symbols.push_back(std::move(resolved));
  }
  report.symbols_matched = required_symbols_ok;
  return report;
}

std::string_view ToString(GameEdition value) noexcept {
  switch (value) {
    case GameEdition::gog:
      return "gog";
    case GameEdition::steam:
      return "steam";
  }
  return "unknown";
}

std::string_view ToString(SymbolKind value) noexcept {
  switch (value) {
    case SymbolKind::read_only_data:
      return "read-only-data";
    case SymbolKind::writable_data:
      return "writable-data";
    case SymbolKind::read_only_function:
      return "read-only-function";
    case SymbolKind::diagnostic_function:
      return "diagnostic-function";
  }
  return "unknown";
}

std::string_view ToString(EvidenceLevel value) noexcept {
  switch (value) {
    case EvidenceLevel::exact_binary:
      return "exact-binary";
    case EvidenceLevel::static_analysis:
      return "static-analysis";
    case EvidenceLevel::live_observed:
      return "live-observed";
  }
  return "unknown";
}

}  // namespace ht2mp::game
