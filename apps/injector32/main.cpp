#include "injector.hpp"

#include "ht2mp/windows/runtime.hpp"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

void usage() {
  std::cerr
      << "Usage: ht2mp-injector32 --pid <pid> --exe <king.exe> --sha256 <hex>\n"
      << "       --dll <bridge.dll> --profile <id> --pipe <name>\n"
      << "       --nonce <32-hex> --run-id <uint64>\n"
      << "       [--vehicle-selector <62..87> --paint <0..3>]\n"
      << "       [--experimental-telemetry]\n"
      << "       [--experimental-gog-actor-diagnostics]\n";
  std::cerr << "       [--experimental-remote-actors]\n"
            << "       [--online-mode]\n"
            << "       [--auto-enter-world]\n";
}

std::optional<std::wstring> take_value(const int argc, wchar_t** argv,
                                       int& index, std::string& error) {
  if (index + 1 >= argc) {
    error = "Missing command-line value";
    return std::nullopt;
  }
  return std::wstring(argv[++index]);
}

bool parse_u64(const std::wstring& value, std::uint64_t& result) {
  if (value.empty() || value.front() == L'-') {
    return false;
  }
  wchar_t* end{};
  errno = 0;
  const auto parsed = _wcstoui64(value.c_str(), &end, 10);
  if (errno == ERANGE || end == value.c_str() || *end != L'\0') {
    return false;
  }
  result = parsed;
  return true;
}

bool parse_nonce(const std::string_view value, std::array<std::uint8_t, 16>& nonce) {
  if (value.size() != nonce.size() * 2U) {
    return false;
  }
  const auto nibble = [](const char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < nonce.size(); ++i) {
    const auto high = nibble(value[i * 2U]);
    const auto low = nibble(value[i * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    nonce[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
  ht2mp::injector::Options options;
  bool have_pid{};
  bool have_exe{};
  bool have_hash{};
  bool have_dll{};
  bool have_profile{};
  bool have_pipe{};
  bool have_nonce{};
  bool have_run_id{};
  bool have_vehicle_selector{};
  bool have_paint{};
  std::string error;

  for (int i = 1; i < argc; ++i) {
    const std::wstring_view argument(argv[i]);
    if (argument == L"--experimental-telemetry" ||
        argument == L"--experimental-gog-telemetry") {
      options.experimental_gog_telemetry = true;
      continue;
    }
    if (argument == L"--experimental-gog-actor-diagnostics") {
      options.experimental_gog_actor_diagnostics = true;
      continue;
    }
    if (argument == L"--experimental-remote-actors" ||
        argument == L"--experimental-gog-remote-actors") {
      options.experimental_gog_remote_actors = true;
      continue;
    }
    if (argument == L"--auto-enter-world") {
      options.auto_enter_world = true;
      continue;
    }
    if (argument == L"--online-mode") {
      options.online_mode = true;
      options.experimental_gog_telemetry = true;
      options.experimental_gog_remote_actors = true;
      continue;
    }
    const auto value = take_value(argc, argv, i, error);
    if (!value) {
      usage();
      return 2;
    }
    if (argument == L"--pid") {
      std::uint64_t parsed{};
      have_pid = parse_u64(*value, parsed) && parsed > 0U && parsed <= UINT32_MAX;
      options.process_id = static_cast<std::uint32_t>(parsed);
    } else if (argument == L"--exe") {
      options.expected_executable = *value;
      have_exe = true;
    } else if (argument == L"--sha256") {
      options.expected_sha256 = ht2mp::windows::narrow_utf8(*value, error);
      have_hash = error.empty();
    } else if (argument == L"--dll") {
      options.bridge_dll = *value;
      have_dll = true;
    } else if (argument == L"--profile") {
      options.profile_id = ht2mp::windows::narrow_utf8(*value, error);
      have_profile = error.empty();
    } else if (argument == L"--pipe") {
      options.pipe_name = *value;
      have_pipe = true;
    } else if (argument == L"--nonce") {
      const auto text = ht2mp::windows::narrow_utf8(*value, error);
      have_nonce = error.empty() && parse_nonce(text, options.nonce);
    } else if (argument == L"--run-id") {
      have_run_id = parse_u64(*value, options.run_id) && options.run_id != 0U;
    } else if (argument == L"--vehicle-selector") {
      std::uint64_t parsed{};
      have_vehicle_selector = parse_u64(*value, parsed) && parsed >= 62U && parsed <= 87U;
      options.vehicle_selector = static_cast<std::uint32_t>(parsed);
    } else if (argument == L"--paint") {
      std::uint64_t parsed{};
      have_paint = parse_u64(*value, parsed) && parsed <= 3U;
      options.paint_variant = static_cast<std::uint32_t>(parsed);
    } else {
      std::wcerr << L"Unknown option: " << argument << L'\n';
      usage();
      return 2;
    }
  }

  if (!have_pid || !have_exe || !have_hash || !have_dll || !have_profile ||
      !have_pipe || !have_nonce || !have_run_id) {
    std::cerr << "Injector arguments are incomplete or malformed\n";
    usage();
    return 2;
  }
  if (have_vehicle_selector != have_paint) {
    std::cerr << "--vehicle-selector and --paint must be supplied together\n";
    return 2;
  }
  options.appearance_override = have_vehicle_selector && have_paint;
  if (options.experimental_gog_actor_diagnostics &&
      !options.experimental_gog_telemetry) {
    std::cerr << "--experimental-gog-actor-diagnostics requires --experimental-telemetry\n";
    return 2;
  }
  if (options.experimental_gog_remote_actors &&
      !options.experimental_gog_telemetry) {
    std::cerr << "--experimental-remote-actors requires --experimental-telemetry\n";
    return 2;
  }
  if (options.experimental_gog_remote_actors &&
      options.experimental_gog_actor_diagnostics) {
    std::cerr << "remote actor hooks and pass-through actor diagnostics are mutually exclusive\n";
    return 2;
  }
  if (options.auto_enter_world && !options.experimental_gog_telemetry) {
    std::cerr << "--auto-enter-world requires --experimental-telemetry\n";
    return 2;
  }
  if (options.online_mode && options.profile_id != "steam-8138acee") {
    std::cerr << "--online-mode requires exact profile steam-8138acee\n";
    return 2;
  }
  if (!ht2mp::injector::inject_and_bootstrap(options, error)) {
    std::cerr << error << '\n';
    return 3;
  }
  return 0;
}
