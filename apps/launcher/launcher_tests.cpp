#include "config.hpp"
#include "profile_editor.hpp"
#include "status.hpp"

#include "ht2mp/protocol/vehicle_catalog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
int failures{};

void check(const bool condition, const char* expression, const int line) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
  }
}
#define CHECK(value) check((value), #value, __LINE__)

template <typename T>
void release(T*& value) {
  if (value != nullptr) value->Release();
  value = nullptr;
}

bool make_stream(IStorage* storage, const wchar_t* name,
                 const std::uint32_t value = 0U) {
  IStream* stream{};
  const auto result = storage->CreateStream(
      name, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0U, 0U, &stream);
  if (FAILED(result)) return false;
  ULONG written{};
  const bool ok = SUCCEEDED(stream->Write(&value, sizeof(value), &written)) &&
                  written == sizeof(value);
  release(stream);
  return ok;
}

bool make_byte_stream(IStorage* storage, const wchar_t* name,
                      const std::vector<std::uint8_t>& value) {
  IStream* stream{};
  const auto result = storage->CreateStream(
      name, STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, 0U, 0U, &stream);
  if (FAILED(result)) return false;
  ULONG written{};
  const bool ok = SUCCEEDED(stream->Write(value.data(),
                                          static_cast<ULONG>(value.size()),
                                          &written)) &&
                  written == value.size();
  release(stream);
  return ok;
}

void append_u32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

std::vector<std::uint8_t> chunk(const std::array<char, 4>& tag,
                                const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> result(tag.begin(), tag.end());
  append_u32(result, static_cast<std::uint32_t>(payload.size()));
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

std::vector<std::uint8_t> make_globals(const bool corrupt_campaign) {
  // Exact-Steam GLBL schema through the campaign-result threshold: fixed
  // prefix, DWORD vector, fixed fields, DWORD vector, then a 32-byte tail.
  std::vector<std::uint8_t> globals(296U, 0U);
  append_u32(globals, 0U);
  globals.resize(globals.size() + 21U, 0U);
  append_u32(globals, 0U);
  globals.resize(globals.size() + 8U, 0U);
  const std::uint64_t threshold = corrupt_campaign
      ? 0x0123456789abcdefULL
      : 0x3fe051eb851eb852ULL;
  append_u32(globals, static_cast<std::uint32_t>(threshold));
  append_u32(globals, static_cast<std::uint32_t>(threshold >> 32U));
  globals.resize(globals.size() + 16U, 0U);
  return globals;
}

std::vector<std::uint8_t> make_xai(const bool corrupt_model = false,
                                   const bool corrupt_campaign = false) {
  std::vector<std::uint8_t> vehicle(0xfcU, 0U);
  vehicle[0] = 40U;  // Jeep in the source save.
  vehicle[4] = corrupt_model ? 41U : 40U;
  vehicle[8] = 0xa0U;
  vehicle[9] = 0x86U;
  vehicle[10] = 0x01U;  // Local native vehicle id 100000.
  const auto pveh = chunk({'P', 'V', 'E', 'H'}, vehicle);
  const auto plyd = chunk({'P', 'L', 'Y', 'D'}, pveh);
  constexpr std::array local_name{'$', '$', '$', '_', 'L', 'I', 'V', 'E',
                                  '_', '0', '_', '0', '\0'};
  std::vector<std::uint8_t> player(local_name.begin(), local_name.end());
  player.insert(player.end(), plyd.begin(), plyd.end());
  auto player_chunk = chunk({'P', 'L', 'Y', 'R'}, player);
  while ((player_chunk.size() & 3U) != 0U) player_chunk.push_back(0U);
  const auto players = chunk({'P', 'L', 'R', 'S'}, player_chunk);
  auto globals = chunk({'G', 'L', 'B', 'L'}, make_globals(corrupt_campaign));
  while ((globals.size() & 3U) != 0U) globals.push_back(0U);
  globals.insert(globals.end(), players.begin(), players.end());
  return chunk({'T', 'O', 'T', 'L'}, globals);
}

std::vector<std::uint8_t> make_anm(const bool corrupt = false) {
  std::vector<std::uint8_t> bytes;
  for (std::uint32_t page = 0U; page < 400U; ++page) {
    const bool populated = page == 100U;
    append_u32(bytes, populated ? 1U : 0U);
    if (populated) {
      for (std::uint32_t slot = 0U; slot < 500U; ++slot) {
        if (slot != 0U) {
          append_u32(bytes, 0xffffffffU);
          continue;
        }
        append_u32(bytes, corrupt ? 100001U : 100000U);
        append_u32(bytes, 3U);
        bytes.insert(bytes.end(), {'a', 'p', '\0'});
        std::vector<std::uint8_t> payload(300U, 0U);
        payload[0] = 2U;  // Offroad vehicle.tech index in the source save.
        constexpr std::array room{'r', 'o', 'o', 'm', '_', 'a', 'p', '_',
                                  '0', '4', '8', '\0'};
        std::copy(room.begin(), room.end(), payload.begin() + 16U);
        append_u32(bytes, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
      }
    }
    append_u32(bytes, 0xfffffffeU);
  }
  return bytes;
}

bool make_slot(IStorage* games, const wchar_t* name, const bool complete,
               const bool corrupt_xai = false,
               const bool corrupt_campaign = false,
               const bool corrupt_anm = false) {
  IStorage* slot{};
  if (FAILED(games->CreateStorage(
          name, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0U, 0U,
          &slot))) return false;
  constexpr std::array names{L"XAI", L"env", L"anm", L"stat", L"cont"};
  bool ok = true;
  for (std::size_t index = 0; index < names.size() - (complete ? 0U : 1U); ++index) {
    if (index == 0U) {
      ok = ok && make_byte_stream(slot, names[index],
                                  make_xai(corrupt_xai, corrupt_campaign));
    } else if (index == 2U) {
      ok = ok && make_byte_stream(slot, names[index], make_anm(corrupt_anm));
    } else {
      ok = ok && make_stream(slot, names[index]);
    }
  }
  ok = ok && SUCCEEDED(slot->Commit(STGC_DEFAULT));
  release(slot);
  return ok;
}

bool make_template(const fs::path& path,
                   const std::uint32_t version = 4U,
                   const bool corrupt_xai = false,
                   const bool corrupt_campaign = false,
                   const bool corrupt_anm = false) {
  IStorage* root{};
  if (FAILED(StgCreateDocfile(path.c_str(),
                              STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
                              0U, &root))) return false;
  bool ok = make_stream(root, L"settings", 5U) &&
            make_stream(root, L"ver", version);
  IStorage* games{};
  ok = ok && SUCCEEDED(root->CreateStorage(
                 L"listofgames",
                 STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0U, 0U,
                 &games));
  if (games != nullptr) {
    ok = ok && make_slot(games, L"complete-a", true, corrupt_xai,
                         corrupt_campaign, corrupt_anm) &&
         make_slot(games, L"complete-b", true) &&
         make_slot(games, L"unfinished", false) &&
         SUCCEEDED(games->Commit(STGC_DEFAULT));
  }
  release(games);
  ok = ok && SUCCEEDED(root->Commit(STGC_DEFAULT));
  release(root);
  return ok;
}

std::vector<char> bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void catalog_test() {
  const auto catalog = ht2mp::protocol::steam_vehicle_catalog();
  CHECK(catalog.size() == 26U);
  constexpr std::array expected_keys{
      "Gazelle", "Gazelle1C", "Offroad", "Pickup", "Patrol", "Cayman",
      "BmwM5", "BmwM5police", "Bus", "Marera", "Megan", "Mini", "Oka",
      "Van", "Avensis", "Volga", "Fiat", "Sobol", "ScaniaR", "KamazR",
      "RenaultR", "ZilR", "MercedesR", "VolvoR", "DafR", "StormR"};
  std::set<std::string_view> keys;
  std::set<std::uint16_t> selectors;
  std::set<std::uint16_t> native_selectors;
  std::size_t native_appearance_count{};
  for (std::size_t index = 0U; index < catalog.size(); ++index) {
    const auto& vehicle = catalog[index];
    CHECK(vehicle.key == expected_keys[index]);
    keys.insert(vehicle.key);
    selectors.insert(vehicle.selector);
    CHECK(vehicle.selector == static_cast<std::uint16_t>(62U + index));
    CHECK(vehicle.steam_vehicle_tech_index == index);
    CHECK(vehicle.steam_native_paint_count >= 1U &&
          vehicle.steam_native_paint_count <= 4U);
    for (std::uint8_t paint = 0U;
         paint < vehicle.steam_native_paint_count; ++paint) {
      ht2mp::protocol::SteamNativeVehicleAppearance native;
      CHECK(ht2mp::protocol::to_steam_native_vehicle(
          vehicle.selector, paint, native));
      CHECK(native.selector == vehicle.steam_native_selector + paint);
      CHECK(native.paint_variant == paint);
      CHECK(native.vehicle_tech_index == index);
      native_selectors.insert(native.selector);
      ++native_appearance_count;
      std::uint8_t decoded_paint{};
      const auto* decoded =
          ht2mp::protocol::find_steam_vehicle_by_native_selector(
              native.selector, decoded_paint);
      CHECK(decoded == &vehicle && decoded_paint == paint);
    }
  }
  CHECK(keys.size() == 26U && selectors.size() == 26U);
  CHECK(native_appearance_count == 94U);
  CHECK(native_selectors.size() == native_appearance_count);
  ht2mp::protocol::SteamNativeVehicleAppearance cayman;
  CHECK(ht2mp::protocol::to_steam_native_vehicle(67U, 3U, cayman));
  CHECK(cayman.selector == 52U && cayman.vehicle_tech_index == 5U);
  std::uint8_t scania_paint{};
  const auto* native_scania =
      ht2mp::protocol::find_steam_vehicle_by_native_selector(
          67U, scania_paint);
  CHECK(native_scania != nullptr && native_scania->key == "ScaniaR" &&
        scania_paint == 0U);
  ht2mp::protocol::SteamNativeVehicleAppearance unsupported;
  CHECK(!ht2mp::protocol::to_steam_native_vehicle(63U, 1U, unsupported));
  CHECK(!ht2mp::protocol::to_steam_native_vehicle(64U, 2U, unsupported));
  for (std::uint16_t tractor = 88U; tractor <= 100U; ++tractor) {
    CHECK(!ht2mp::protocol::is_allowed_steam_vehicle(tractor));
  }
  constexpr std::array tractor_keys{
      "Renault", "Freightliner", "Scania", "Mack", "Kenworth", "Kamaz",
      "Zil", "Peterbilt", "Daf", "Mercedes", "Volvo", "Storm",
      "International"};
  for (const auto* tractor : tractor_keys) {
    CHECK(ht2mp::protocol::find_steam_vehicle_by_key(tractor) == nullptr);
  }
  CHECK(ht2mp::protocol::kPaintVariantCount == 4U);
}

void config_test(const fs::path& root) {
  ht2mp::launcher::LauncherConfig source;
  source.steam_directory = root / L"Игра Steam";
  source.player_name = "Водитель";
  source.vehicle_key = "StormR";
  source.paint_variant = 3U;
  source.servers = {{"example.org", "00112233445566778899aabbccddeeff"},
                    {"127.0.0.1:30000", "ffeeddccbbaa99887766554433221100"},
                    {"public.example.org:28020", ""}};
  std::string error;
  const auto path = root / L"launcher.ini";
  CHECK(ht2mp::launcher::save_config(path, source, error));
  ht2mp::launcher::LauncherConfig loaded;
  CHECK(ht2mp::launcher::load_config(path, loaded, error));
  CHECK(loaded.player_name == source.player_name);
  CHECK(loaded.steam_directory == source.steam_directory);
  CHECK(loaded.vehicle_key == source.vehicle_key && loaded.paint_variant == 3U);
  CHECK(loaded.servers.size() == 3U);
  CHECK(loaded.servers[0].endpoint == "example.org:28020");
  CHECK(loaded.servers[2].endpoint == "public.example.org:28020");
  CHECK(loaded.servers[2].token.empty());
  CHECK(ht2mp::launcher::validate_token(""));
  std::string normalized;
  CHECK(!ht2mp::launcher::normalize_endpoint("[::1]:28020", normalized,
                                               error));
  CHECK(!ht2mp::launcher::normalize_endpoint("999.1.1.1", normalized,
                                               error));
  CHECK(!ht2mp::launcher::normalize_endpoint("-bad.example", normalized,
                                               error));

  ht2mp::launcher::upsert_server(
      loaded, {"example.org:28020", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"});
  CHECK(loaded.servers.size() == 3U);
  CHECK(loaded.servers[0].token == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  CHECK(ht2mp::launcher::erase_server(loaded, "127.0.0.1:30000"));
  CHECK(loaded.servers.size() == 2U);

  std::ofstream corrupt(root / L"broken.ini", std::ios::binary);
  corrupt << "[launcher]\nthis line is broken\n";
  corrupt.close();
  CHECK(!ht2mp::launcher::load_config(root / L"broken.ini", loaded, error));
}

void profile_test(const fs::path& root) {
  const auto source = root / L"template.pl1";
  CHECK(make_template(source));
  const auto original = bytes(source);
  const auto runtime = root / L"runtime";
  std::error_code ec;
  fs::create_directories(runtime, ec);
  CHECK(!ec);
  std::string error;
  for (const auto& vehicle : ht2mp::protocol::steam_vehicle_catalog()) {
    for (std::uint8_t paint = 0U;
         paint < vehicle.steam_native_paint_count; ++paint) {
      ht2mp::launcher::ProfileEditResult result;
      CHECK(ht2mp::launcher::prepare_online_profile(
          source, runtime, vehicle.selector, paint, result, error));
      CHECK(result.completed_slots == 2U);
    }
  }
  ht2mp::launcher::ProfileEditResult unsupported;
  CHECK(!ht2mp::launcher::prepare_online_profile(
      source, runtime, 63U, 1U, unsupported, error));
  CHECK(bytes(source) == original);
  CHECK(fs::is_regular_file(
      runtime / L"\u0412\u041e\u0414\u0418\u0422\u0415\u041b\u042c.pl1.bak"));

  std::ofstream damage(
      runtime / L"\u0412\u041e\u0414\u0418\u0422\u0415\u041b\u042c.pl1",
      std::ios::binary | std::ios::trunc);
  damage << "broken";
  damage.close();
  ht2mp::launcher::ProfileEditResult restored;
  CHECK(ht2mp::launcher::prepare_online_profile(source, runtime, 62U, 0U,
                                                 restored, error));
  CHECK(restored.destination.filename() ==
        L"\u0412\u041e\u0414\u0418\u0422\u0415\u041b\u042c.pl1");

  // A stale profile created by the old launcher would make native auto-enter
  // reject the runtime because it contains two .pl1 files.
  std::ofstream legacy(runtime / L"HT2MP.pl1", std::ios::binary);
  legacy << "obsolete";
  legacy.close();
  CHECK(ht2mp::launcher::prepare_online_profile(source, runtime, 62U, 0U,
                                                 restored, error));
  CHECK(!fs::exists(runtime / L"HT2MP.pl1"));

  const auto truck_ini = runtime / L"TRUCK.INI";
  {
    std::ofstream ini(truck_ini, std::ios::binary);
    ini << "[truck]\r\nlastp=HT2MP\r\nfoo=bar\r\n";
  }
  CHECK(ht2mp::launcher::write_last_player(truck_ini, error));
  const auto ini_bytes = bytes(truck_ini);
  const std::string ini_text(ini_bytes.begin(), ini_bytes.end());
  CHECK(ini_text.find(
            "lastp=\xC2\xCE\xC4\xC8\xD2\xC5\xCB\xDC\r\n") !=
        std::string::npos);

  std::ofstream invalid(root / L"invalid.pl1", std::ios::binary);
  invalid << "not structured storage";
  invalid.close();
  const auto empty_runtime = root / L"empty-runtime";
  fs::create_directories(empty_runtime, ec);
  ht2mp::launcher::ProfileEditResult rejected;
  CHECK(!ht2mp::launcher::prepare_online_profile(
      root / L"invalid.pl1", empty_runtime, 62U, 0U, rejected, error));

  const auto wrong_version = root / L"wrong-version.pl1";
  CHECK(make_template(wrong_version, 3U));
  const auto version_runtime = root / L"version-runtime";
  fs::create_directories(version_runtime, ec);
  CHECK(!ht2mp::launcher::prepare_online_profile(
      wrong_version, version_runtime, 62U, 0U, rejected, error));

  const auto corrupt_native = root / L"corrupt-native.pl1";
  CHECK(make_template(corrupt_native, 4U, true));
  const auto corrupt_runtime = root / L"corrupt-runtime";
  fs::create_directories(corrupt_runtime, ec);
  CHECK(!ht2mp::launcher::prepare_online_profile(
      corrupt_native, corrupt_runtime, 62U, 0U, rejected, error));

  const auto corrupt_campaign = root / L"corrupt-campaign.pl1";
  CHECK(make_template(corrupt_campaign, 4U, false, true));
  const auto campaign_runtime = root / L"campaign-runtime";
  fs::create_directories(campaign_runtime, ec);
  CHECK(!ht2mp::launcher::prepare_online_profile(
      corrupt_campaign, campaign_runtime, 62U, 0U, rejected, error));

  const auto corrupt_anm = root / L"corrupt-anm.pl1";
  CHECK(make_template(corrupt_anm, 4U, false, false, true));
  const auto anm_runtime = root / L"anm-runtime";
  fs::create_directories(anm_runtime, ec);
  CHECK(!ht2mp::launcher::prepare_online_profile(
      corrupt_anm, anm_runtime, 62U, 0U, rejected, error));
}

void bundled_template_test(const fs::path& source) {
  CHECK(fs::is_regular_file(source));
  const auto original = bytes(source);
  CHECK(!original.empty());

  const auto root = fs::temp_directory_path() /
                    (L"ht2mp-bundled-template-test-" +
                     std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  CHECK(!ec);

  std::string error;
  ht2mp::launcher::ProfileEditResult result;
  const bool prepared = ht2mp::launcher::prepare_online_profile(
      source, root, 62U, 0U, result, error);
  if (!prepared) std::cerr << "bundled template: " << error << '\n';
  CHECK(prepared);
  if (prepared) {
    // The distribution profile must contain one driver and exactly one
    // deliberately completed game.  Extra complete slots risk carrying user
    // history into the online runtime.
    CHECK(result.completed_slots == 1U);
    CHECK(fs::is_regular_file(result.destination));
  }
  CHECK(bytes(source) == original);
  fs::remove_all(root, ec);
}

void state_test() {
  ht2mp::launcher::RunState state;
  CHECK(state.begin());
  CHECK(!state.begin());
  const auto started = ht2mp::launcher::parse_status_event(
      "HT2MP-EVENT/1 state=game-started");
  const auto connected = ht2mp::launcher::parse_status_event(
      "HT2MP-EVENT/1 state=connected session=7 player=2");
  const auto reconnecting = ht2mp::launcher::parse_status_event(
      "HT2MP-EVENT/1 state=reconnecting detail=transport reset");
  const auto rejected = ht2mp::launcher::parse_status_event(
      "HT2MP-EVENT/1 state=rejected detail=token mismatch");
  CHECK(started && connected && reconnecting && rejected);
  state.apply(*started);
  state.apply(*connected);
  CHECK(state.phase() == ht2mp::launcher::RunPhase::connected);
  state.apply(*reconnecting);
  CHECK(state.phase() == ht2mp::launcher::RunPhase::reconnecting &&
        reconnecting->detail == "transport reset");
  state.apply(*connected);
  CHECK(state.phase() == ht2mp::launcher::RunPhase::connected);
  state.apply(*rejected);
  CHECK(state.phase() == ht2mp::launcher::RunPhase::rejected);
  CHECK(state.request_shutdown());
  state.finish(0U);
  CHECK(!state.active() && state.phase() == ht2mp::launcher::RunPhase::stopped);

  CHECK(state.begin());
  state.finish(0xc0000005U);
  CHECK(!state.active() && state.phase() == ht2mp::launcher::RunPhase::failed);
  CHECK(!ht2mp::launcher::parse_status_event("ordinary sidecar output"));
}

}  // namespace

int main(const int argc, const char* const argv[]) {
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // Test-only live harness: exercises the same profile-preparation API used by
  // the GUI without automating mouse/keyboard input in the user's desktop.
  if (argc == 6 && std::string_view(argv[1]) == "--prepare-runtime") {
    std::string error;
    ht2mp::launcher::ProfileEditResult result;
    bool prepared{};
    try {
      prepared = ht2mp::launcher::prepare_online_profile(
          fs::path(argv[2]), fs::path(argv[3]),
          static_cast<std::uint16_t>(std::stoul(argv[4])),
          static_cast<std::uint8_t>(std::stoul(argv[5])), result, error);
    } catch (const std::exception& exception) {
      error = exception.what();
    }
    if (prepared) {
      prepared = ht2mp::launcher::write_last_player(
          fs::path(argv[3]) / L"TRUCK.INI", error);
    }
    if (initialized == S_OK || initialized == S_FALSE) CoUninitialize();
    if (!prepared) {
      std::cerr << error << '\n';
      return 1;
    }
    std::cout << "prepared " << result.completed_slots << " slot(s)\n";
    return 0;
  }
  if (argc == 2) {
    bundled_template_test(fs::path(argv[1]));
    if (initialized == S_OK || initialized == S_FALSE) CoUninitialize();
    if (failures == 0) std::cout << "bundled template test passed\n";
    return failures == 0 ? 0 : 1;
  }
  const auto root = fs::temp_directory_path() /
                    (L"ht2mp-launcher-tests-" +
                     std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  CHECK(!ec);
  catalog_test();
  config_test(root);
  profile_test(root);
  state_test();
  fs::remove_all(root, ec);
  if (initialized == S_OK || initialized == S_FALSE) CoUninitialize();
  if (failures == 0) std::cout << "launcher tests passed\n";
  return failures == 0 ? 0 : 1;
}
