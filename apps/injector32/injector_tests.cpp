#include "injector.hpp"

#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures{};

void expect_rejection(const ht2mp::injector::Options& options,
                      const std::string_view expected_error) {
  std::string error;
  if (ht2mp::injector::inject_and_bootstrap(options, error)) {
    std::cerr << "unexpected injector success; expected " << expected_error
              << '\n';
    ++failures;
    return;
  }
  if (!error.starts_with(expected_error)) {
    std::cerr << "unexpected injector error: " << error << "; expected prefix "
              << expected_error << '\n';
    ++failures;
  }
}

ht2mp::injector::Options base_options(
    const std::filesystem::path& executable,
    const std::filesystem::path& bridge,
    const std::string& sha256) {
  ht2mp::injector::Options options;
  options.process_id = GetCurrentProcessId();
  options.expected_executable = executable;
  options.bridge_dll = bridge;
  options.expected_sha256 = sha256;
  options.profile_id = "gog-05588140";
  options.pipe_name = L"\\\\.\\pipe\\HT2MP-injector-preflight-test";
  options.nonce.fill(0x5aU);
  options.run_id = 1U;
  return options;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
  if (argc != 2) {
    std::cerr << "expected absolute path to ht2mp-bridge32.dll\n";
    return 2;
  }

  std::string error;
  const auto self = ht2mp::windows::executable_path(error);
  if (!error.empty()) {
    std::cerr << error << '\n';
    return 2;
  }
  const auto bridge = std::filesystem::absolute(argv[1]);

  ht2mp::windows::Sha256 self_digest{};
  if (!ht2mp::windows::sha256_file(self, self_digest, error)) {
    std::cerr << error << '\n';
    return 2;
  }

  auto wrong_path = base_options(
      self.parent_path() / L"definitely-not-king.exe", bridge,
      ht2mp::windows::sha256_hex(self_digest));
  expect_rejection(wrong_path,
                   "Expected executable or bridge DLL does not exist");

  auto actor_without_telemetry = base_options(
      self, bridge, ht2mp::windows::sha256_hex(self_digest));
  actor_without_telemetry.experimental_gog_actor_diagnostics = true;
  expect_rejection(actor_without_telemetry,
                   "Actor diagnostics require experimental GOG telemetry");

  auto auto_enter_without_telemetry = base_options(
      self, bridge, ht2mp::windows::sha256_hex(self_digest));
  auto_enter_without_telemetry.auto_enter_world = true;
  expect_rejection(auto_enter_without_telemetry,
                   "Auto-enter requires experimental GOG telemetry");

  auto online_wrong_profile = base_options(
      self, bridge, ht2mp::windows::sha256_hex(self_digest));
  online_wrong_profile.online_mode = true;
  online_wrong_profile.experimental_gog_telemetry = true;
  online_wrong_profile.experimental_gog_remote_actors = true;
  expect_rejection(
      online_wrong_profile,
      "Online mode requires telemetry and remote actors on exact profile steam-8138acee");

  auto online_missing_capabilities = base_options(
      self, bridge, ht2mp::windows::sha256_hex(self_digest));
  online_missing_capabilities.profile_id = "steam-8138acee";
  online_missing_capabilities.online_mode = true;
  expect_rejection(
      online_missing_capabilities,
      "Online mode requires telemetry and remote actors on exact profile steam-8138acee");

  auto appearance_wrong_profile = base_options(
      self, bridge, ht2mp::windows::sha256_hex(self_digest));
  appearance_wrong_profile.appearance_override = true;
  appearance_wrong_profile.vehicle_selector = 62U;
  appearance_wrong_profile.paint_variant = 3U;
  expect_rejection(
      appearance_wrong_profile,
      "Appearance override requires an allowed exact-Steam vehicle and paint 0..3");

  auto appearance_tractor = appearance_wrong_profile;
  appearance_tractor.profile_id = "steam-8138acee";
  appearance_tractor.vehicle_selector = 88U;
  expect_rejection(
      appearance_tractor,
      "Appearance override requires an allowed exact-Steam vehicle and paint 0..3");

  auto wrong_hash = base_options(self, bridge, std::string(64U, '0'));
  expect_rejection(wrong_hash,
                   "Expected SHA-256 does not match requested profile");

  // A caller must not turn --profile into a label and authorize an arbitrary
  // x86 image merely by supplying that image's own hash.
  auto spoofed_profile = base_options(
      self, bridge, ht2mp::windows::sha256_hex(self_digest));
  expect_rejection(spoofed_profile,
                   "Expected SHA-256 does not match requested profile");

  // Exercise the post-profile PID/file-identity boundary when this machine has
  // a staged exact GOG image. The test remains portable: a clean CI worker may
  // legitimately have no proprietary game files and skips only this case.
  PWSTR local_app_data{};
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                     nullptr, &local_app_data))) {
    const auto staged_king = std::filesystem::path(local_app_data) /
                             L"HT2MP" / L"runtime" /
                             L"gog-05588140" / L"king.exe";
    CoTaskMemFree(local_app_data);
    if (std::filesystem::is_regular_file(staged_king)) {
      auto wrong_pid = base_options(
          staged_king, bridge,
          "4412a5f695dd016c9f92185b7d2d0be8ad3be787bfe2d77908f5b4d171eedd86");
      expect_rejection(wrong_pid,
                       "PID does not refer to the expected staged king.exe");
    } else {
      std::cout << "SKIP: staged exact GOG image unavailable for PID identity case\n";
    }
  }

  auto malformed_hash = base_options(self, bridge, "not-a-sha256");
  expect_rejection(malformed_hash, "Expected SHA-256 is malformed");

  if (failures != 0) {
    std::cerr << failures << " injector preflight test(s) failed\n";
    return 1;
  }
  std::cout << "injector fail-closed preflight checks passed\n";
  return 0;
}
