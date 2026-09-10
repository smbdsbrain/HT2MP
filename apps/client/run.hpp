#pragma once

#include "edition.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ht2mp::client {

struct RunOptions final {
  EditionDescriptor edition{};
  std::filesystem::path injector_path;
  std::filesystem::path bridge_path;
  std::string server;
  std::string token;
  std::string player_name{"HT2MP_POC"};
  std::uint16_t vehicle_type{0xffffU};
  std::uint8_t paint_variant{};
  std::string instance;
  bool online_mode{};
  bool experimental_gog_telemetry{};
  bool experimental_gog_actor_diagnostics{};
  bool experimental_gog_remote_actors{};
  bool auto_enter_world{};
  bool status_events{};
  std::wstring shutdown_event;
  // Optional directory for network trace CSVs (empty disables tracing).
  std::filesystem::path trace_dir;
  std::vector<std::wstring> game_arguments;
};

[[nodiscard]] bool run_staged_game(
    const RunOptions& options,
    unsigned long& game_exit_code,
    std::string& error);

} // namespace ht2mp::client
