#include "ht2mp/game/ai_player_pool.hpp"
#include "ht2mp/game/observer.hpp"
#include "ht2mp/game/pattern.hpp"
#include "ht2mp/game/pe_image.hpp"
#include "ht2mp/game/profile.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#include <TlHelp32.h>
#endif

namespace {

struct Arguments {
  std::filesystem::path executable;
  std::string requested_profile;
  std::string research_patterns_from;
  std::optional<std::uint32_t> pid;
  std::uint32_t samples{};
  std::uint32_t interval_ms{50U};
  bool list_ai{};
  bool help{};
};

void Usage() {
  std::cout
      << "usage:\n"
      << "  ht2mp-profile-check [--profile ID] <path-to-king.exe>\n"
      << "  ht2mp-profile-check [--profile ID] --pid PID [--samples N] "
         "[--interval-ms N] [--list-ai]\n"
      << "  ht2mp-profile-check --research-patterns-from ID <path-to-exe>\n";
}

bool ParseUnsigned(std::string_view text, std::uint32_t& value) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t parsed = 0U;
  for (const auto character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    parsed = parsed * 10U + static_cast<unsigned int>(character - '0');
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
  }
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseArguments(int argc, char** argv, Arguments& arguments,
                    std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view token(argv[index]);
    if (token == "--help" || token == "-h") {
      arguments.help = true;
      continue;
    }
    const auto take_value = [&](std::string_view option) -> const char* {
      if (index + 1 >= argc) {
        error = std::string(option) + " requires a value";
        return nullptr;
      }
      return argv[++index];
    };
    if (token == "--profile") {
      const auto* value = take_value(token);
      if (value == nullptr) {
        return false;
      }
      arguments.requested_profile = value;
    } else if (token == "--research-patterns-from") {
      const auto* value = take_value(token);
      if (value == nullptr) {
        return false;
      }
      arguments.research_patterns_from = value;
    } else if (token == "--pid") {
      const auto* value = take_value(token);
      std::uint32_t parsed = 0U;
      if (value == nullptr || !ParseUnsigned(value, parsed) || parsed == 0U) {
        error = "--pid must be a positive decimal process ID";
        return false;
      }
      arguments.pid = parsed;
    } else if (token == "--samples") {
      const auto* value = take_value(token);
      if (value == nullptr || !ParseUnsigned(value, arguments.samples) ||
          arguments.samples > 100'000U) {
        error = "--samples must be in range 0..100000";
        return false;
      }
    } else if (token == "--interval-ms") {
      const auto* value = take_value(token);
      if (value == nullptr || !ParseUnsigned(value, arguments.interval_ms) ||
          arguments.interval_ms < 10U || arguments.interval_ms > 60'000U) {
        error = "--interval-ms must be in range 10..60000";
        return false;
      }
    } else if (token == "--list-ai") {
      arguments.list_ai = true;
    } else if (!token.empty() && token.front() == '-') {
      error = "unknown option: " + std::string(token);
      return false;
    } else if (arguments.executable.empty()) {
      arguments.executable = std::filesystem::path(std::string(token));
    } else {
      error = "more than one executable path was supplied";
      return false;
    }
  }
  if (arguments.help) {
    return true;
  }
  if (arguments.pid.has_value() == !arguments.executable.empty()) {
    error = "supply exactly one of an executable path or --pid";
    return false;
  }
  if ((arguments.samples != 0U || arguments.list_ai) && !arguments.pid.has_value()) {
    error = "live observation requires --pid";
    return false;
  }
  if (!arguments.research_patterns_from.empty() &&
      (arguments.pid.has_value() || arguments.samples != 0U ||
       arguments.list_ai || !arguments.requested_profile.empty())) {
    error = "research pattern scan accepts only its source profile and an offline executable";
    return false;
  }
  return true;
}

std::string Hex(std::uint64_t value, int width = 0) {
  std::ostringstream output;
  output << "0x" << std::hex << std::uppercase << std::setfill('0');
  if (width > 0) {
    output << std::setw(width);
  }
  output << value;
  return output.str();
}

#if defined(_WIN32)
struct ProcessModule {
  std::filesystem::path path;
  std::uintptr_t base{};
};

bool FindKingModule(std::uint32_t pid, ProcessModule& module, std::string& error) {
  const auto snapshot = CreateToolhelp32Snapshot(
      TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, static_cast<DWORD>(pid));
  if (snapshot == INVALID_HANDLE_VALUE) {
    error = "CreateToolhelp32Snapshot failed: " +
            std::to_string(static_cast<unsigned long>(GetLastError()));
    return false;
  }
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Module32FirstW(snapshot, &entry) != FALSE) {
    do {
      std::wstring name(entry.szModule);
      for (auto& character : name) {
        if (character >= L'A' && character <= L'Z') {
          character = static_cast<wchar_t>(character - L'A' + L'a');
        }
      }
      if (name == L"king.exe") {
        module.path = entry.szExePath;
        module.base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        found = true;
        break;
      }
    } while (Module32NextW(snapshot, &entry) != FALSE);
  }
  CloseHandle(snapshot);
  if (!found) {
    error = "king.exe module was not found in the target process";
  }
  return found;
}

class ProcessMemory final : public ht2mp::game::ReadOnlyMemory {
 public:
  explicit ProcessMemory(std::uint32_t pid) noexcept
      : handle_(OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
                            FALSE, static_cast<DWORD>(pid))) {}
  ~ProcessMemory() override {
    if (handle_ != nullptr) {
      CloseHandle(handle_);
    }
  }
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

  bool Read(std::uintptr_t address, std::span<std::byte> destination,
            std::string& error) noexcept override {
    if (ReadFast(address, destination)) return true;
    error = "ReadProcessMemory failed: " +
            std::to_string(static_cast<unsigned long>(GetLastError()));
    return false;
  }

  bool ReadFast(std::uintptr_t address,
                std::span<std::byte> destination) noexcept override {
    SIZE_T read = 0U;
    if (ReadProcessMemory(handle_, reinterpret_cast<const void*>(address),
                          destination.data(), destination.size(), &read) == FALSE ||
        read != destination.size()) {
      return false;
    }
    return true;
  }

 private:
  HANDLE handle_{};
};
#endif

void PrintReport(const ht2mp::game::ProfileVerification& report) {
  std::cout << "executable: " << report.executable_path.string() << '\n';
  std::cout << "sha256: " << report.calculated_sha256 << '\n';
  std::cout << "profile: "
            << (report.profile == nullptr ? "<none>"
                                          : std::string(report.profile->id))
            << '\n';
  if (report.profile != nullptr) {
    std::cout << "edition: " << ht2mp::game::ToString(report.profile->edition)
              << '\n';
  }
  std::cout << "fingerprint: " << (report.accepted() ? "ACCEPTED" : "REJECTED")
            << '\n';
  std::cout << "observer_ready: " << (report.observer_ready() ? "yes" : "no")
            << '\n';
  std::cout << "pe: machine=" << Hex(report.pe.machine, 4)
            << " image_base=" << Hex(report.pe.image_base, 8)
            << " entry_rva=" << Hex(report.pe.entry_point_rva, 8)
            << " image_size=" << Hex(report.pe.size_of_image, 8) << '\n';
  if (!report.symbols.empty()) {
    std::cout << "symbols:\n";
    for (const auto& symbol : report.symbols) {
      std::cout << "  " << symbol.name << ": "
                << (symbol.accepted ? "ok" : "rejected")
                << " match=" << Hex(symbol.match_rva, 8)
                << " result=" << Hex(symbol.result_rva, 8)
                << " count=" << symbol.match_count << " evidence="
                << ht2mp::game::ToString(symbol.evidence) << '\n';
    }
  }
  for (const auto& warning : report.warnings) {
    std::cout << "warning: " << warning << '\n';
  }
  for (const auto& error : report.errors) {
    std::cout << "error: " << error << '\n';
  }
}

int PrintResearchPatternScan(const std::filesystem::path& executable,
                             const std::string_view source_profile_id) {
  const auto* source = ht2mp::game::FindGameProfile(source_profile_id);
  if (source == nullptr) {
    std::cerr << "error: unknown source profile: " << source_profile_id << '\n';
    return 2;
  }
  ht2mp::game::PeImage image;
  std::string error;
  if (!ht2mp::game::PeImage::Load(executable, image, error)) {
    std::cerr << "error: " << error << '\n';
    return 2;
  }
  std::cout << "RESEARCH ONLY: matches do not authorize runtime calls or a profile\n"
            << "target: " << executable.string() << '\n'
            << "target_sha256: " << ht2mp::game::Sha256Hex(image.bytes()) << '\n'
            << "patterns_from: " << source->id << '\n';
  for (const auto& descriptor : source->symbols) {
    const auto* section = image.FindSection(descriptor.scan_section);
    const auto raw = image.RawSection(descriptor.scan_section);
    ht2mp::game::MaskedPattern pattern;
    if (section == nullptr || !raw ||
        !ht2mp::game::ParseMaskedPattern(descriptor.ida_pattern, pattern,
                                         error)) {
      std::cout << "  " << descriptor.name << ": unavailable\n";
      continue;
    }
    const auto matches = ht2mp::game::FindAllMasked(*raw, pattern, 9U);
    std::cout << "  " << descriptor.name << ": count=" << matches.size();
    for (const auto offset : matches) {
      const auto match_rva = section->virtual_address +
                             static_cast<std::uint32_t>(offset);
      std::cout << " match=" << Hex(match_rva, 8);
      if (descriptor.resolver ==
              ht2mp::game::SymbolResolver::absolute_va32_operand &&
          offset + descriptor.operand_offset + sizeof(std::uint32_t) <=
              raw->size()) {
        std::uint32_t absolute{};
        std::memcpy(&absolute,
                    raw->data() + offset + descriptor.operand_offset,
                    sizeof(absolute));
        const auto result = static_cast<std::int64_t>(absolute) -
                            image.metadata().image_base +
                            descriptor.result_addend;
        if (result >= 0 && result <= UINT32_MAX) {
          std::cout << " result="
                    << Hex(static_cast<std::uint32_t>(result), 8);
        } else {
          std::cout << " result=<out-of-image>";
        }
      }
    }
    if (matches.size() == 9U) std::cout << " (capped)";
    std::cout << '\n';
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  std::string argument_error;
  if (!ParseArguments(argc, argv, arguments, argument_error)) {
    std::cerr << "error: " << argument_error << '\n';
    Usage();
    return 64;
  }
  if (arguments.help) {
    Usage();
    return 0;
  }
  if (!arguments.research_patterns_from.empty()) {
    return PrintResearchPatternScan(arguments.executable,
                                    arguments.research_patterns_from);
  }

  std::uintptr_t module_base = 0U;
#if defined(_WIN32)
  ProcessModule process_module;
  if (arguments.pid.has_value()) {
    std::string process_error;
    if (!FindKingModule(*arguments.pid, process_module, process_error)) {
      std::cerr << "error: " << process_error << '\n';
      return 2;
    }
    arguments.executable = process_module.path;
    module_base = process_module.base;
  }
#else
  if (arguments.pid.has_value()) {
    std::cerr << "error: live process observation is only available on Windows\n";
    return 2;
  }
#endif

  const auto report = ht2mp::game::VerifyGameExecutable(
      arguments.executable, arguments.requested_profile);
  PrintReport(report);
  if (!report.accepted()) {
    return 2;
  }

#if defined(_WIN32)
  if (arguments.pid.has_value()) {
    ProcessMemory memory(*arguments.pid);
    if (!memory.valid()) {
      std::cerr << "error: OpenProcess(PROCESS_VM_READ) failed\n";
      return 3;
    }
    if (arguments.samples != 0U) {
      std::string observer_error;
      auto observer = ht2mp::game::LocalTransformObserver::Create(
          report, module_base, memory, observer_error);
      if (!observer) {
        std::cerr << "error: " << observer_error << '\n';
        return 3;
      }
      for (std::uint32_t index = 0; index < arguments.samples; ++index) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto time_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        const auto sample = observer->Poll(time_us);
        std::cout << "telemetry status=" << ht2mp::game::ToString(sample.status);
        if (sample.has_sample()) {
          std::cout << " seq=" << sample.sample.sequence << " pos="
                    << sample.sample.position.x << ',' << sample.sample.position.y
                    << ',' << sample.sample.position.z << " quat="
                    << sample.sample.orientation.x << ','
                    << sample.sample.orientation.y << ','
                    << sample.sample.orientation.z << ','
                    << sample.sample.orientation.w << " velocity_valid="
                    << (sample.sample.velocity_valid ? "yes" : "no");
        }
        std::cout << " detail=" << sample.detail << '\n';
        if (index + 1U < arguments.samples) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(arguments.interval_ms));
        }
      }
    }
    if (arguments.list_ai) {
      std::string pool_error;
      auto pool = ht2mp::game::AiPlayerPoolObserver::Create(
          report, module_base, memory, pool_error);
      if (!pool) {
        std::cerr << "error: " << pool_error << '\n';
        return 3;
      }
      const auto snapshot = pool->Snapshot();
      std::cout << "ai_pool status=" << ht2mp::game::ToString(snapshot.status)
                << " sentinel=" << Hex(snapshot.sentinel_address, 8)
                << " local_player_id=" << Hex(snapshot.local_player_id, 8)
                << " local_valid="
                << (snapshot.local_player_valid ? "yes" : "no")
                << " world_ready="
                << (snapshot.local_world_ready ? "yes" : "no")
                << " ai_initialized="
                << (snapshot.ai_subsystem_initialized ? "yes" : "no")
                << " room_registry_count=" << snapshot.room_registry_count
                << " count=" << snapshot.players.size()
                << " detail=" << snapshot.detail << '\n';
      if (snapshot.has_snapshot() && snapshot.local_player_valid) {
        const auto& vehicle = snapshot.local_vehicle;
        std::cout << "  local_vehicle=" << Hex(vehicle.vehicle_address, 8)
                  << " viewer=" << Hex(vehicle.viewer_address, 8)
                  << " owner=" << Hex(vehicle.owner_player_id, 8)
                  << " physics=" << Hex(vehicle.physics_address, 8)
                  << " chain_consistent="
                  << (vehicle.chain_consistent ? "yes" : "no")
                  << " owner_matches="
                  << (vehicle.owner_matches_local ? "yes" : "no")
                  << " position_finite="
                  << (vehicle.position_finite ? "yes" : "no")
                  << " position=[" << vehicle.position[0] << ','
                  << vehicle.position[1] << ',' << vehicle.position[2] << ']'
                  << " current_room=\"" << vehicle.current_room_name << "\"/"
                  << vehicle.current_room_id << " room_resolved="
                  << (vehicle.current_room_resolved ? "yes" : "no")
                  << " detail=" << vehicle.detail << '\n';
      }
      for (const auto& player : snapshot.players) {
        std::cout << "  player=" << Hex(player.player_address, 8)
                  << " node=" << Hex(player.node_address, 8)
                  << " name=" << player.name
                  << " vehicle_id=" << player.vehicle_id
                  << " actor_type=" << player.actor_type
                  << " liveness_marker=" << Hex(player.liveness_marker, 8)
                  << " constructor_marker="
                  << (player.constructor_marker_matches ? "yes" : "no")
                  << " selected_local="
                  << (player.selected_local ? "yes" : "no")
                   << " flags=" << Hex(player.flags, 8)
                   << " x_controlled=" << (player.x_controlled ? "yes" : "no")
                   << " endpoint_rooms=[" << player.position_room_a << ','
                   << player.position_room_b << ']'
                   << " room_bounds="
                   << (player.position_rooms_in_bounds ? "yes" : "no")
                   << '\n';
      }
    }
  }
#endif
  return 0;
}
