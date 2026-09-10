#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ht2mp::injector {

struct Options final {
  std::uint32_t process_id{};
  std::filesystem::path expected_executable;
  std::filesystem::path bridge_dll;
  std::string expected_sha256;
  std::string profile_id;
  std::wstring pipe_name;
  std::array<std::uint8_t, 16> nonce{};
  std::uint64_t run_id{};
  std::uint32_t vehicle_selector{0xffffffffU};
  std::uint32_t paint_variant{};
  bool appearance_override{};
  bool experimental_gog_telemetry{};
  bool experimental_gog_actor_diagnostics{};
  bool experimental_gog_remote_actors{};
  bool online_mode{};
  bool auto_enter_world{};
};

[[nodiscard]] bool inject_and_bootstrap(const Options& options, std::string& error);

} // namespace ht2mp::injector
