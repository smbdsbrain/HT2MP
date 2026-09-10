#include "edition.hpp"
#include "run.hpp"
#include "staging.hpp"

#include "ht2mp/windows/runtime.hpp"
#include "ht2mp/game/profile.hpp"
#include "ht2mp/protocol/validation.hpp"
#include "ht2mp/protocol/vehicle_catalog.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace {

void usage() {
  std::cout
      << "HT2MP Windows sidecar\n\n"
      << "Usage:\n"
      << "  ht2mp-client stage --edition gog|steam --source <game-directory>\n"
      << "                     [--instance <id>]\n"
      << "  ht2mp-client seed-driver --edition gog|steam --save <driver.pl1>\n"
      << "                           [--instance <id>]\n"
      << "  ht2mp-client run --edition gog|steam [--server <host:port>]\n"
      << "                   [--token <128-bit-hex>] [--name <utf8-name>]\n"
      << "                   [--vehicle <catalog-key>] [--paint <0..3>]\n"
      << "                   [--status-events] [--shutdown-event <name>]\n"
      << "                   [--instance <id>] [--online-mode]\n"
      << "                   [--injector <path>] [--bridge <path>]\n"
      << "                   [--experimental-gog-telemetry]\n"
      << "                   [--experimental-steam-telemetry]\n"
      << "                   [--experimental-gog-actor-diagnostics]\n"
      << "                   [--experimental-gog-remote-actors]\n"
      << "                   [--experimental-steam-remote-actors]\n"
      << "                   [--auto-enter-world] [--trace-dir <dir>]\n"
      << "                   [-- <game args>]\n";
}

std::optional<std::wstring> take_value(
    const int argc, wchar_t** argv, int& index, std::string& error) {
  if (index + 1 >= argc) {
    std::string conversion_error;
    const auto option = ht2mp::windows::narrow_utf8(argv[index], conversion_error);
    error = "Missing value after " + option;
    return std::nullopt;
  }
  return std::wstring(argv[++index]);
}

std::optional<ht2mp::client::EditionDescriptor> parse_edition(
    const std::wstring& value, std::string& error) {
  const auto utf8 = ht2mp::windows::narrow_utf8(value, error);
  if (!error.empty()) {
    return std::nullopt;
  }
  const auto edition = ht2mp::client::find_edition(utf8);
  if (!edition) {
    error = "Unknown edition/profile: " + utf8;
  }
  return edition;
}

bool valid_token(const std::string_view token) {
  if (token.empty()) {
    return true;
  }
  ht2mp::windows::Sha256 scratch{};
  // The session token is 128 bits rather than a SHA-256, so pad it solely to
  // reuse strict hexadecimal validation.
  if (token.size() != 32U) {
    return false;
  }
  std::string padded(token);
  padded.append(32U, '0');
  return ht2mp::windows::parse_sha256(padded, scratch);
}

int stage_command(const int argc, wchar_t** argv) {
  std::optional<ht2mp::client::EditionDescriptor> edition;
  fs::path source;
  std::string instance;
  std::string error;
  for (int i = 2; i < argc; ++i) {
    const std::wstring_view argument(argv[i]);
    if (argument == L"--edition") {
      const auto value = take_value(argc, argv, i, error);
      if (!value || !(edition = parse_edition(*value, error))) {
        std::cerr << error << '\n';
        return 2;
      }
    } else if (argument == L"--source") {
      const auto value = take_value(argc, argv, i, error);
      if (!value) {
        std::cerr << error << '\n';
        return 2;
      }
      source = *value;
    } else if (argument == L"--instance") {
      const auto value = take_value(argc, argv, i, error);
      if (!value) {
        std::cerr << error << '\n';
        return 2;
      }
      instance = ht2mp::windows::narrow_utf8(*value, error);
      if (!error.empty() || !ht2mp::client::valid_instance_id(instance)) {
        std::cerr << (error.empty() ? "Invalid --instance; expected lowercase [a-z0-9][a-z0-9_-]{0,31}" : error) << '\n';
        return 2;
      }
    } else {
      std::wcerr << L"Unknown stage option: " << argument << L'\n';
      return 2;
    }
  }
  if (!edition || source.empty()) {
    usage();
    return 2;
  }
  ht2mp::client::StageResult result;
  if (!ht2mp::client::stage_game(*edition, source, result, error, instance)) {
    std::cerr << error << '\n';
    return 3;
  }
  std::cout << "Staged " << result.files_copied << " files ("
             << result.bytes_copied << " bytes), skipped " << result.files_skipped
             << " user/runtime files.\nDestination: " << result.destination.string()
             << "\nSeed one isolated driver with `ht2mp-client seed-driver`, or "
                "create HT2MP_POC in the staged game.\n";
  return 0;
}

int seed_driver_command(const int argc, wchar_t** argv) {
  std::optional<ht2mp::client::EditionDescriptor> edition;
  fs::path save;
  std::string instance;
  std::string error;
  for (int i = 2; i < argc; ++i) {
    const std::wstring_view argument(argv[i]);
    if (argument == L"--edition") {
      const auto value = take_value(argc, argv, i, error);
      if (!value || !(edition = parse_edition(*value, error))) {
        std::cerr << error << '\n';
        return 2;
      }
    } else if (argument == L"--save") {
      const auto value = take_value(argc, argv, i, error);
      if (!value) {
        std::cerr << error << '\n';
        return 2;
      }
      save = *value;
    } else if (argument == L"--instance") {
      const auto value = take_value(argc, argv, i, error);
      if (!value) {
        std::cerr << error << '\n';
        return 2;
      }
      instance = ht2mp::windows::narrow_utf8(*value, error);
      if (!error.empty() || !ht2mp::client::valid_instance_id(instance)) {
        std::cerr << (error.empty() ? "Invalid --instance; expected lowercase [a-z0-9][a-z0-9_-]{0,31}" : error) << '\n';
        return 2;
      }
    } else {
      std::wcerr << L"Unknown seed-driver option: " << argument << L'\n';
      return 2;
    }
  }
  if (!edition || save.empty()) {
    usage();
    return 2;
  }

  ht2mp::client::DriverSeedResult result;
  if (!ht2mp::client::seed_staged_driver(*edition, save, result, error,
                                          instance)) {
    std::cerr << error << '\n';
    return 3;
  }
  std::cout << "Seeded isolated driver (" << result.bytes_copied
            << " bytes): " << result.destination.string() << '\n';
  return 0;
}

int run_command(const int argc, wchar_t** argv) {
  std::optional<ht2mp::client::EditionDescriptor> edition;
  std::optional<fs::path> injector;
  std::optional<fs::path> bridge;
  std::string server;
  std::string token;
  std::string name{"HT2MP_POC"};
  std::string instance;
  bool online_mode{};
  bool experimental_gog_telemetry{};
  bool requested_gog_telemetry{};
  bool requested_steam_telemetry{};
  bool experimental_gog_actor_diagnostics{};
  bool experimental_gog_remote_actors{};
  bool requested_gog_remote_actors{};
  bool requested_steam_remote_actors{};
  bool auto_enter_world{};
  bool status_events{};
  std::wstring shutdown_event;
  std::string vehicle_key;
  unsigned paint_variant{};
  bool paint_supplied{};
  std::optional<fs::path> trace_dir;
  std::vector<std::wstring> game_arguments;
  std::string error;

  for (int i = 2; i < argc; ++i) {
    const std::wstring_view argument(argv[i]);
    if (argument == L"--") {
      for (++i; i < argc; ++i) {
        game_arguments.emplace_back(argv[i]);
      }
      break;
    }
    if (argument == L"--experimental-gog-telemetry") {
      experimental_gog_telemetry = true;
      requested_gog_telemetry = true;
      continue;
    }
    if (argument == L"--experimental-steam-telemetry") {
      experimental_gog_telemetry = true;
      requested_steam_telemetry = true;
      continue;
    }
    if (argument == L"--experimental-gog-actor-diagnostics") {
      experimental_gog_actor_diagnostics = true;
      continue;
    }
    if (argument == L"--experimental-gog-remote-actors") {
      experimental_gog_remote_actors = true;
      requested_gog_remote_actors = true;
      experimental_gog_telemetry = true;
      continue;
    }
    if (argument == L"--experimental-steam-remote-actors") {
      experimental_gog_remote_actors = true;
      requested_steam_remote_actors = true;
      requested_steam_telemetry = true;
      experimental_gog_telemetry = true;
      continue;
    }
    if (argument == L"--auto-enter-world") {
      auto_enter_world = true;
      // World-ready acknowledgement uses the exact-build local observer, so
      // one multiplayer UX flag deliberately opts into both pieces.
      experimental_gog_telemetry = true;
      continue;
    }
    if (argument == L"--online-mode") {
      online_mode = true;
      experimental_gog_telemetry = true;
      experimental_gog_remote_actors = true;
      requested_steam_telemetry = true;
      requested_steam_remote_actors = true;
      continue;
    }
    if (argument == L"--status-events") {
      status_events = true;
      continue;
    }
    const auto value_for = [&](std::wstring_view) {
      return take_value(argc, argv, i, error);
    };
    if (argument == L"--edition") {
      const auto value = value_for(argument);
      if (!value || !(edition = parse_edition(*value, error))) {
        std::cerr << error << '\n';
        return 2;
      }
    } else if (argument == L"--injector") {
      const auto value = value_for(argument);
      if (!value) { std::cerr << error << '\n'; return 2; }
      injector = fs::path(*value);
    } else if (argument == L"--bridge") {
      const auto value = value_for(argument);
      if (!value) { std::cerr << error << '\n'; return 2; }
      bridge = fs::path(*value);
    } else if (argument == L"--trace-dir") {
      const auto value = value_for(argument);
      if (!value) { std::cerr << error << '\n'; return 2; }
      trace_dir = fs::path(*value);
    } else if (argument == L"--shutdown-event") {
      const auto value = value_for(argument);
      if (!value) { std::cerr << error << '\n'; return 2; }
      shutdown_event = *value;
    } else if (argument == L"--paint") {
      const auto value = value_for(argument);
      if (!value) { std::cerr << error << '\n'; return 2; }
      const auto utf8 = ht2mp::windows::narrow_utf8(*value, error);
      if (!error.empty() || utf8.size() != 1U || utf8[0] < '0' || utf8[0] > '3') {
        std::cerr << "--paint must be in range 0..3\n";
        return 2;
      }
      paint_variant = static_cast<unsigned>(utf8[0] - '0');
      paint_supplied = true;
    } else if (argument == L"--server" || argument == L"--token" ||
               argument == L"--name" || argument == L"--instance" ||
               argument == L"--vehicle") {
      const auto value = value_for(argument);
      if (!value) { std::cerr << error << '\n'; return 2; }
      const auto utf8 = ht2mp::windows::narrow_utf8(*value, error);
      if (!error.empty()) { std::cerr << error << '\n'; return 2; }
      if (argument == L"--server") server = utf8;
      if (argument == L"--token") token = utf8;
      if (argument == L"--name") name = utf8;
      if (argument == L"--instance") instance = utf8;
      if (argument == L"--vehicle") vehicle_key = utf8;
    } else {
      std::wcerr << L"Unknown run option: " << argument << L'\n';
      return 2;
    }
  }
  if (!edition) {
    usage();
    return 2;
  }
  if (server.empty() && !token.empty()) {
    std::cerr << "--token requires --server\n";
    return 2;
  }
  if (!valid_token(token)) {
    std::cerr << "--token must contain exactly 32 hexadecimal characters\n";
    return 2;
  }
  if (!ht2mp::client::valid_instance_id(instance)) {
    std::cerr << "Invalid --instance; expected lowercase [a-z0-9][a-z0-9_-]{0,31}\n";
    return 2;
  }
  if (online_mode && server.empty()) {
    std::cerr << "--online-mode requires --server\n";
    return 2;
  }
  if (online_mode && edition->profile_id != "steam-8138acee") {
    std::cerr << "--online-mode is supported only for the exact steam-8138acee profile\n";
    return 2;
  }
  const ht2mp::protocol::VehicleCatalogEntry* vehicle{};
  if (!vehicle_key.empty()) {
    vehicle = ht2mp::protocol::find_steam_vehicle_by_key(vehicle_key);
    if (vehicle == nullptr) {
      std::cerr << "--vehicle is not an allowed standalone Steam vehicle\n";
      return 2;
    }
  }
  if (paint_supplied && vehicle == nullptr) {
    std::cerr << "--paint requires --vehicle\n";
    return 2;
  }
  if (requested_gog_telemetry && edition->edition != "gog") {
    std::cerr << "--experimental-gog-telemetry is valid only for the exact GOG profile\n";
    return 2;
  }
  if (requested_steam_telemetry && edition->edition != "steam") {
    std::cerr << "--experimental-steam-telemetry is valid only for the exact Steam profile\n";
    return 2;
  }
  if (requested_gog_telemetry && requested_steam_telemetry) {
    std::cerr << "select only one edition-specific telemetry opt-in\n";
    return 2;
  }
  if (experimental_gog_actor_diagnostics && !experimental_gog_telemetry) {
    std::cerr << "--experimental-gog-actor-diagnostics requires --experimental-gog-telemetry\n";
    return 2;
  }
  if (experimental_gog_actor_diagnostics && edition->edition != "gog") {
    std::cerr << "--experimental-gog-actor-diagnostics is valid only for the exact GOG profile\n";
    return 2;
  }
  if (requested_gog_remote_actors && edition->edition != "gog") {
    std::cerr << "--experimental-gog-remote-actors is valid only for the exact GOG profile\n";
    return 2;
  }
  if (requested_steam_remote_actors && edition->edition != "steam") {
    std::cerr << "--experimental-steam-remote-actors is valid only for the exact Steam profile\n";
    return 2;
  }
  if (requested_gog_remote_actors && requested_steam_remote_actors) {
    std::cerr << "select only one edition-specific remote actor opt-in\n";
    return 2;
  }
  if (experimental_gog_remote_actors && experimental_gog_actor_diagnostics) {
    std::cerr << "remote actor hooks and pass-through actor diagnostics are mutually exclusive\n";
    return 2;
  }
  if (experimental_gog_remote_actors && server.empty()) {
    std::cerr << "experimental remote actors require --server\n";
    return 2;
  }
  const auto* selected_game_profile =
      ht2mp::game::FindGameProfile(edition->profile_id);
  if (auto_enter_world &&
      (selected_game_profile == nullptr ||
       !selected_game_profile->auto_enter_world.enabled)) {
    std::cerr << "--auto-enter-world is unavailable for this exact profile\n";
    return 2;
  }
  const auto limits =
      ht2mp::protocol::limits_for_profile(edition->profile_id);
  const auto validation_vehicle = vehicle != nullptr
      ? vehicle->selector
      : static_cast<std::uint16_t>(edition->profile_id == "steam-8138acee" ? 62U : 0U);
  const ht2mp::protocol::ClientHello validation_probe{
      std::string(edition->profile_id), {}, name, 1U, validation_vehicle,
      static_cast<std::uint8_t>(paint_variant)};
  if (!limits) {
    std::cerr << "Internal error: selected edition has no protocol limits\n";
    return 2;
  }
  if (const auto validation =
          ht2mp::protocol::validate(validation_probe, *limits);
      !validation) {
    std::cerr << "Invalid multiplayer identity: " << validation.detail << '\n';
    return 2;
  }

  const auto self = ht2mp::windows::executable_path(error);
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 3;
  }
  ht2mp::client::RunOptions options;
  options.edition = *edition;
  std::error_code path_error;
  options.injector_path = fs::absolute(
      injector.value_or(self.parent_path() / L"ht2mp-injector32.exe"), path_error);
  if (path_error) {
    std::cerr << "Cannot normalize injector path: " << path_error.message() << '\n';
    return 3;
  }
  options.bridge_path = fs::absolute(
      bridge.value_or(self.parent_path() / L"ht2mp-bridge32.dll"), path_error);
  if (path_error) {
    std::cerr << "Cannot normalize bridge path: " << path_error.message() << '\n';
    return 3;
  }
  options.server = std::move(server);
  options.token = std::move(token);
  options.player_name = std::move(name);
  options.vehicle_type = vehicle == nullptr ? 0xffffU : vehicle->selector;
  options.paint_variant = static_cast<std::uint8_t>(paint_variant);
  options.instance = std::move(instance);
  options.online_mode = online_mode;
  options.experimental_gog_telemetry = experimental_gog_telemetry;
  options.experimental_gog_actor_diagnostics =
      experimental_gog_actor_diagnostics;
  options.experimental_gog_remote_actors = experimental_gog_remote_actors;
  options.auto_enter_world = auto_enter_world;
  options.status_events = status_events;
  options.shutdown_event = std::move(shutdown_event);
  if (trace_dir) {
    options.trace_dir = fs::absolute(*trace_dir, path_error);
    if (path_error) {
      std::cerr << "Cannot normalize trace directory: " << path_error.message() << '\n';
      return 3;
    }
  }
  options.game_arguments = std::move(game_arguments);

  unsigned long exit_code{};
  if (!ht2mp::client::run_staged_game(options, exit_code, error)) {
    if (status_events) {
      std::cout << "HT2MP-EVENT/1 state=error detail=" << error << '\n'
                << std::flush;
    }
    std::cerr << error << '\n';
    return 4;
  }
  std::cout << "king.exe exit code: " << exit_code << '\n';
  return 0;
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::wstring_view command(argv[1]);
  if (command == L"stage") {
    return stage_command(argc, argv);
  }
  if (command == L"seed-driver") {
    return seed_driver_command(argc, argv);
  }
  if (command == L"run") {
    return run_command(argc, argv);
  }
  if (command == L"--help" || command == L"-h" || command == L"help") {
    usage();
    return 0;
  }
  std::wcerr << L"Unknown command: " << command << L'\n';
  usage();
  return 2;
}
