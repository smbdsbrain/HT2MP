#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ht2mp::launcher {

struct SavedServer final {
  std::string endpoint;
  std::string token;

  friend bool operator==(const SavedServer&, const SavedServer&) = default;
};

struct LauncherConfig final {
  std::filesystem::path steam_directory;
  std::string player_name{"HT2MP"};
  std::string vehicle_key{"Gazelle"};
  std::uint8_t paint_variant{};
  std::vector<SavedServer> servers;
};

[[nodiscard]] std::filesystem::path launcher_config_path(std::string& error);
[[nodiscard]] std::filesystem::path launcher_log_path(std::string& error);
[[nodiscard]] bool normalize_endpoint(std::string value, std::string& normalized,
                                      std::string& error);
[[nodiscard]] bool validate_token(std::string_view value) noexcept;
[[nodiscard]] bool load_config(const std::filesystem::path& path,
                               LauncherConfig& config, std::string& error);
[[nodiscard]] bool save_config(const std::filesystem::path& path,
                               const LauncherConfig& config, std::string& error);
void upsert_server(LauncherConfig& config, SavedServer server);
[[nodiscard]] bool erase_server(LauncherConfig& config, std::string_view endpoint);

}  // namespace ht2mp::launcher
