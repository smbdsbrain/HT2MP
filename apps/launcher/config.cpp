#include "config.hpp"

#include "ht2mp/protocol/validation.hpp"
#include "ht2mp/protocol/vehicle_catalog.hpp"
#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ht2mp::launcher {
namespace fs = std::filesystem;
namespace {

bool safe_byte(const unsigned char value) noexcept {
  return std::isalnum(value) != 0 || value == '-' || value == '_' ||
         value == '.' || value == ' ' || value == ':' || value == '\\' ||
         value == '/';
}

std::string encode(const std::string_view value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (const unsigned char byte : value) {
    if (safe_byte(byte) && byte != '%') {
      result.push_back(static_cast<char>(byte));
    } else {
      result.push_back('%');
      result.push_back(hex[byte >> 4U]);
      result.push_back(hex[byte & 0x0fU]);
    }
  }
  return result;
}

int nibble(const char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool decode(const std::string_view value, std::string& result) {
  result.clear();
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') {
      result.push_back(value[index]);
      continue;
    }
    if (index + 2U >= value.size()) return false;
    const auto high = nibble(value[index + 1U]);
    const auto low = nibble(value[index + 2U]);
    if (high < 0 || low < 0) return false;
    result.push_back(static_cast<char>((high << 4) | low));
    index += 2U;
  }
  return true;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1U);
}

bool valid_ipv4(const std::string_view host) {
  std::size_t begin{};
  unsigned parts{};
  while (begin <= host.size()) {
    const auto end = host.find('.', begin);
    const auto text = host.substr(
        begin, (end == std::string_view::npos ? host.size() : end) - begin);
    unsigned value{};
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || value > 255U) {
      return false;
    }
    ++parts;
    if (end == std::string_view::npos) break;
    begin = end + 1U;
  }
  return parts == 4U;
}

bool valid_dns_name(const std::string_view host) {
  std::size_t begin{};
  while (begin <= host.size()) {
    const auto end = host.find('.', begin);
    const auto label = host.substr(
        begin, (end == std::string_view::npos ? host.size() : end) - begin);
    if (label.empty() || label.size() > 63U || label.front() == '-' ||
        label.back() == '-' ||
        !std::all_of(label.begin(), label.end(), [](const unsigned char ch) {
          return std::isalnum(ch) != 0 || ch == '-';
        })) {
      return false;
    }
    if (end == std::string_view::npos) break;
    begin = end + 1U;
  }
  return true;
}

bool ensure_parent(const fs::path& path, std::string& error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    error = "Cannot create launcher settings directory: " + ec.message();
    return false;
  }
  return true;
}

bool valid_identity(const LauncherConfig& config) {
  const auto* vehicle =
      ht2mp::protocol::find_steam_vehicle_by_key(config.vehicle_key);
  const auto limits =
      ht2mp::protocol::limits_for_profile("steam-8138acee");
  if (vehicle == nullptr || !limits) return false;
  const ht2mp::protocol::ClientHello probe{
      "steam-8138acee", {}, config.player_name, 1U, vehicle->selector,
      config.paint_variant};
  return static_cast<bool>(ht2mp::protocol::validate(probe, *limits));
}

}  // namespace

fs::path launcher_config_path(std::string& error) {
  const auto local = ht2mp::windows::local_app_data(error);
  return error.empty() ? local / L"HT2MP" / L"launcher.ini" : fs::path{};
}

fs::path launcher_log_path(std::string& error) {
  const auto local = ht2mp::windows::local_app_data(error);
  return error.empty() ? local / L"HT2MP" / L"logs" / L"launcher.log"
                       : fs::path{};
}

bool validate_token(const std::string_view value) noexcept {
  return value.empty() || (value.size() == 32U &&
         std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
           return std::isxdigit(ch) != 0;
         }));
}

bool normalize_endpoint(std::string value, std::string& normalized,
                        std::string& error) {
  normalized.clear();
  error.clear();
  value = trim(std::move(value));
  if (value.empty() || value.find('[') != std::string::npos ||
      value.find(']') != std::string::npos ||
      std::count(value.begin(), value.end(), ':') > 1) {
    error = "Use an IPv4 address or DNS name, optionally followed by :port";
    return false;
  }
  auto host = value;
  std::uint32_t port = 28020U;
  if (const auto colon = value.find(':'); colon != std::string::npos) {
    host = value.substr(0U, colon);
    const auto port_text = std::string_view(value).substr(colon + 1U);
    const auto parsed = std::from_chars(port_text.data(),
                                        port_text.data() + port_text.size(), port);
    if (port_text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != port_text.data() + port_text.size() || port == 0U ||
        port > 65535U) {
      error = "Server port must be in range 1..65535";
      return false;
    }
  }
  const auto numeric_address =
      std::all_of(host.begin(), host.end(), [](const unsigned char ch) {
        return std::isdigit(ch) != 0 || ch == '.';
      });
  if (host.empty() || host.size() > 253U ||
      (numeric_address ? !valid_ipv4(host) : !valid_dns_name(host))) {
    error = "Server host is not a valid IPv4 address or DNS name";
    return false;
  }
  normalized = host + ':' + std::to_string(port);
  return true;
}

bool load_config(const fs::path& path, LauncherConfig& config,
                 std::string& error) {
  config = {};
  error.clear();
  std::error_code ec;
  if (!fs::exists(path, ec)) return !ec;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "Cannot open launcher.ini";
    return false;
  }
  std::string section;
  SavedServer current;
  bool in_server{};
  auto finish_server = [&] {
    if (!in_server) return true;
    std::string endpoint_error;
    std::string normalized;
    if (!normalize_endpoint(current.endpoint, normalized, endpoint_error) ||
        !validate_token(current.token)) {
      error = "launcher.ini contains an invalid saved server";
      return false;
    }
    current.endpoint = std::move(normalized);
    upsert_server(config, std::move(current));
    current = {};
    in_server = false;
    return true;
  };
  std::string line;
  std::size_t line_number{};
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto clean = trim(line);
    if (clean.empty() || clean.front() == ';' || clean.front() == '#') continue;
    if (clean.front() == '[' && clean.back() == ']') {
      if (!finish_server()) return false;
      section = clean.substr(1U, clean.size() - 2U);
      in_server = section.starts_with("server.");
      continue;
    }
    const auto equals = clean.find('=');
    if (equals == std::string::npos || section.empty()) {
      error = "Malformed launcher.ini line " + std::to_string(line_number);
      return false;
    }
    const auto key = trim(clean.substr(0U, equals));
    std::string value;
    if (!decode(clean.substr(equals + 1U), value)) {
      error = "Invalid escaped value in launcher.ini line " +
              std::to_string(line_number);
      return false;
    }
    if (section == "launcher") {
      if (key == "steam_path") {
        std::string conversion_error;
        config.steam_directory =
            ht2mp::windows::widen_utf8(value, conversion_error);
        if (!conversion_error.empty()) {
          error = "steam_path is not valid UTF-8";
          return false;
        }
      } else if (key == "player_name") {
        config.player_name = value;
      } else if (key == "vehicle") {
        config.vehicle_key = value;
      } else if (key == "paint") {
        unsigned parsed{};
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
            parsed >= ht2mp::protocol::kPaintVariantCount) {
          error = "Invalid paint value in launcher.ini";
          return false;
        }
        config.paint_variant = static_cast<std::uint8_t>(parsed);
      }
    } else if (in_server) {
      if (key == "endpoint") current.endpoint = value;
      if (key == "token") current.token = value;
    }
  }
  if (!finish_server()) return false;
  if (!valid_identity(config)) {
    error = "launcher.ini contains invalid player or vehicle settings";
    return false;
  }
  return true;
}

bool save_config(const fs::path& path, const LauncherConfig& config,
                 std::string& error) {
  error.clear();
  if (!valid_identity(config)) {
    error = "Launcher identity or appearance is invalid";
    return false;
  }
  if (!ensure_parent(path, error)) return false;
  std::vector<SavedServer> normalized_servers;
  normalized_servers.reserve(config.servers.size());
  for (const auto& server : config.servers) {
    SavedServer normalized{server};
    if (!normalize_endpoint(server.endpoint, normalized.endpoint, error) ||
        !validate_token(server.token)) {
      error = "Cannot save invalid server entry";
      return false;
    }
    normalized_servers.push_back(std::move(normalized));
  }
  std::string path_error;
  const auto path_utf8 = ht2mp::windows::narrow_utf8(
      config.steam_directory.wstring(), path_error);
  if (!path_error.empty()) {
    error = path_error;
    return false;
  }
  const auto temporary = path.wstring() + L".tmp";
  std::ofstream output(fs::path(temporary), std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "Cannot create launcher.ini temporary file";
    return false;
  }
  output << "[launcher]\nsteam_path=" << encode(path_utf8)
         << "\nplayer_name=" << encode(config.player_name)
         << "\nvehicle=" << encode(config.vehicle_key)
         << "\npaint=" << static_cast<unsigned>(config.paint_variant) << "\n";
  for (std::size_t index = 0U; index < normalized_servers.size(); ++index) {
    output << "\n[server." << index << "]\nendpoint="
           << encode(normalized_servers[index].endpoint)
           << "\ntoken=" << encode(normalized_servers[index].token) << "\n";
  }
  output.flush();
  if (!output) {
    error = "Cannot flush launcher.ini";
    return false;
  }
  output.close();
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "Cannot replace launcher.ini: " +
            ht2mp::windows::win32_error(GetLastError());
    DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

void upsert_server(LauncherConfig& config, SavedServer server) {
  const auto found = std::find_if(config.servers.begin(), config.servers.end(),
                                  [&](const auto& item) {
                                    return item.endpoint == server.endpoint;
                                  });
  if (found == config.servers.end()) {
    config.servers.push_back(std::move(server));
  } else {
    *found = std::move(server);
  }
}

bool erase_server(LauncherConfig& config, const std::string_view endpoint) {
  const auto before = config.servers.size();
  std::erase_if(config.servers, [&](const auto& server) {
    return server.endpoint == endpoint;
  });
  return config.servers.size() != before;
}

}  // namespace ht2mp::launcher
