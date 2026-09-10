#include "ht2mp/game/ai_player_pool.hpp"
#include "ht2mp/game/local_world_observer.hpp"
#include "ht2mp/game/observer.hpp"
#include "ht2mp/game/profile.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

class Handle final {
 public:
  Handle() noexcept = default;
  explicit Handle(HANDLE value) noexcept : value_(value) {}
  ~Handle() {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
      value_ = other.release();
    }
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] HANDLE release() noexcept {
    const auto value = value_;
    value_ = nullptr;
    return value;
  }

 private:
  HANDLE value_{};
};

class ProcessMemory final : public ht2mp::game::ReadOnlyMemory {
 public:
  explicit ProcessMemory(HANDLE process) noexcept : process_(process) {}

  bool Read(const std::uintptr_t address, std::span<std::byte> destination,
            std::string& error) noexcept override {
    if (ReadFast(address, destination)) return true;
    error = "ReadProcessMemory failed at 0x" + hex(address) +
            " (Win32 " + std::to_string(GetLastError()) + ')';
    return false;
  }

  bool ReadFast(const std::uintptr_t address,
                const std::span<std::byte> destination) noexcept override {
    SIZE_T transferred{};
    if (!ReadProcessMemory(process_, reinterpret_cast<const void*>(address),
                           destination.data(), destination.size(), &transferred) ||
        transferred != destination.size()) {
      return false;
    }
    return true;
  }

 private:
  static std::string hex(std::uintptr_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    do {
      result.push_back(digits[value & 0xFU]);
      value >>= 4U;
    } while (value != 0U);
    std::reverse(result.begin(), result.end());
    return result;
  }

  HANDLE process_{};
};

struct Options final {
  DWORD pid{};
  std::string profile;
  std::uint32_t duration_seconds{600U};
  std::uint32_t frequency_hz{20U};
  std::filesystem::path csv;
  // Optional additional VehicleInstance (for example the HT2MP remote actor
  // printed by the bridge as vi=<hex>) whose render and simulation transforms
  // are sampled read-only next to the local vehicle.
  std::uint32_t vehicle_address{};
};

std::atomic_bool g_running{true};

BOOL WINAPI console_handler(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
      event == CTRL_CLOSE_EVENT) {
    g_running.store(false, std::memory_order_relaxed);
    return TRUE;
  }
  return FALSE;
}

void usage() {
  std::cout
      << "Usage: ht2mp-runtime-observer --pid <king-pid> --profile <id>\n"
      << "       [--duration <seconds>] [--hz 1..40] [--csv <path>]\n"
      << "       [--vehicle <hex VehicleInstance address>]\n"
      << "This diagnostic opens the process read-only; it never injects or writes.\n";
}

template <typename T>
bool parse_unsigned(std::string_view text, T& value) {
  std::uint64_t parsed{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      parsed > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    return false;
  }
  value = static_cast<T>(parsed);
  return true;
}

std::optional<Options> parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      usage();
      return std::nullopt;
    }
    if (index + 1 >= argc) {
      std::cerr << "Missing value for " << argument << '\n';
      return std::nullopt;
    }
    const std::string_view value(argv[++index]);
    if (argument == "--pid") {
      if (!parse_unsigned(value, options.pid) || options.pid == 0U) {
        std::cerr << "Invalid --pid\n";
        return std::nullopt;
      }
    } else if (argument == "--profile") {
      options.profile.assign(value);
    } else if (argument == "--duration") {
      if (!parse_unsigned(value, options.duration_seconds) ||
          options.duration_seconds == 0U) {
        std::cerr << "Invalid --duration\n";
        return std::nullopt;
      }
    } else if (argument == "--hz") {
      if (!parse_unsigned(value, options.frequency_hz) ||
          options.frequency_hz == 0U || options.frequency_hz > 40U) {
        std::cerr << "--hz must be in 1..40\n";
        return std::nullopt;
      }
    } else if (argument == "--csv") {
      options.csv = std::string(value);
    } else if (argument == "--vehicle") {
      std::string_view digits = value;
      if (digits.starts_with("0x") || digits.starts_with("0X")) {
        digits.remove_prefix(2U);
      }
      std::uint32_t parsed{};
      const auto result = std::from_chars(
          digits.data(), digits.data() + digits.size(), parsed, 16);
      if (result.ec != std::errc{} ||
          result.ptr != digits.data() + digits.size() || parsed == 0U) {
        std::cerr << "Invalid --vehicle (expected hex address)\n";
        return std::nullopt;
      }
      options.vehicle_address = parsed;
    } else {
      std::cerr << "Unknown option: " << argument << '\n';
      return std::nullopt;
    }
  }
  if (options.pid == 0U || options.profile.empty()) {
    usage();
    return std::nullopt;
  }
  return options;
}

std::optional<std::filesystem::path> process_path(HANDLE process) {
  std::vector<wchar_t> buffer(32768U);
  DWORD size = static_cast<DWORD>(buffer.size());
  if (!QueryFullProcessImageNameW(process, 0U, buffer.data(), &size)) {
    return std::nullopt;
  }
  return std::filesystem::path(std::wstring_view(buffer.data(), size));
}

std::optional<std::uintptr_t> module_base(DWORD pid) {
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           pid));
  if (snapshot.get() == INVALID_HANDLE_VALUE) return std::nullopt;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Module32FirstW(snapshot.get(), &entry)) return std::nullopt;
  do {
    if (_wcsicmp(entry.szModule, L"king.exe") == 0) {
      return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
    }
  } while (Module32NextW(snapshot.get(), &entry));
  return std::nullopt;
}

std::uint64_t monotonic_us() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct VehicleTransformPair final {
  ht2mp::game::ObservationStatus render_status{
      ht2mp::game::ObservationStatus::not_ready};
  ht2mp::game::ObservationStatus simulation_status{
      ht2mp::game::ObservationStatus::not_ready};
  ht2mp::game::LocalTransformSample render;
  ht2mp::game::LocalTransformSample simulation;
};

// Reads the VehicleInstance render copy (+0x4ef4/+0x4f18 on both exact
// profiles) and the simulation copy (+0x204/+0x228) of one vehicle. This is
// diagnostic only: the address comes from the AI pool observer or the command
// line, and every byte is validated by the same rigid-transform decoder the
// bridge uses.
VehicleTransformPair read_vehicle_transforms(
    ht2mp::game::ReadOnlyMemory& memory,
    const ht2mp::game::LocalVehicleLayout& layout,
    const std::uint32_t vehicle_address) noexcept {
  VehicleTransformPair result;
  if (vehicle_address == 0U || layout.vehicle_transform_read_size == 0U ||
      layout.vehicle_transform_read_size > 0x40U) {
    return result;
  }
  const ht2mp::game::RigidTransformLayout rigid{
      0U,
      layout.vehicle_position_offset - layout.vehicle_orientation_offset,
      layout.vehicle_transform_read_size,
      layout.maximum_absolute_position,
      layout.basis_length_tolerance,
      layout.basis_dot_tolerance,
  };
  std::array<std::byte, 0x40> bytes{};
  const auto span = std::span(bytes).first(layout.vehicle_transform_read_size);
  if (memory.ReadFast(vehicle_address + layout.vehicle_orientation_offset,
                      span)) {
    result.render_status =
        ht2mp::game::DecodeRigidTransformFast(span, rigid, result.render);
  } else {
    result.render_status = ht2mp::game::ObservationStatus::read_failed;
  }
  if (layout.vehicle_simulation_orientation_offset != 0U &&
      layout.vehicle_simulation_position_offset -
              layout.vehicle_simulation_orientation_offset ==
          rigid.position_offset &&
      memory.ReadFast(
          vehicle_address + layout.vehicle_simulation_orientation_offset,
          span)) {
    result.simulation_status =
        ht2mp::game::DecodeRigidTransformFast(span, rigid, result.simulation);
  } else {
    result.simulation_status = ht2mp::game::ObservationStatus::read_failed;
  }
  return result;
}

void write_transform_csv(std::ostream& output,
                         const ht2mp::game::ObservationStatus status,
                         const ht2mp::game::LocalTransformSample& sample) {
  output << ',' << ht2mp::game::ToString(status);
  if (status == ht2mp::game::ObservationStatus::sample) {
    output << ',' << std::setprecision(10) << sample.position.x << ','
           << sample.position.y << ',' << sample.position.z << ','
           << sample.orientation.x << ',' << sample.orientation.y << ','
           << sample.orientation.z << ',' << sample.orientation.w;
  } else {
    output << ",,,,,,,";
  }
}

const ht2mp::game::ObservedAiPlayer* find_cached_local(
    const ht2mp::game::AiPlayerPoolSnapshot& snapshot) noexcept {
  if (!snapshot.has_snapshot() || snapshot.local_player_id == 0U) {
    return nullptr;
  }
  const auto found = std::find_if(
      snapshot.players.begin(), snapshot.players.end(),
      [&snapshot](const ht2mp::game::ObservedAiPlayer& player) {
        return player.node_address == snapshot.local_player_id;
      });
  return found == snapshot.players.end() ? nullptr : &*found;
}

void write_empty_local_columns(std::ostream& output) {
  // Vehicle/type/self-id/self-match/liveness/constructor-marker/flags + two endpoint RoomIds and
  // their bounds flag + eight raw dwords + distance + finite.
  for (std::size_t index = 0; index < 20U; ++index) output << ',';
  output << '0';
}

void write_empty_vehicle_columns(std::ostream& output) {
  for (std::size_t index = 0; index < 22U; ++index) output << ',';
}

void write_vehicle_csv(
    std::ostream& output,
    const ht2mp::game::ObservedLocalVehicle& vehicle) {
  output << ',' << vehicle.viewer_address << ',' << vehicle.vehicle_address
         << ',' << vehicle.owner_player_id << ',' << vehicle.physics_address
         << ',' << vehicle.moving_item_address << ','
         << vehicle.physics_reverse_moving_item
         << ',' << (vehicle.chain_consistent ? 1 : 0) << ','
         << (vehicle.owner_matches_local ? 1 : 0) << ','
         << (vehicle.physics_available ? 1 : 0) << ','
         << (vehicle.physics_reverse_matches ? 1 : 0) << ','
         << (vehicle.position_finite ? 1 : 0) << ',' << vehicle.position[0]
         << ',' << vehicle.position[1] << ',' << vehicle.position[2] << ','
         << (vehicle.orientation_valid ? 1 : 0) << ','
         << vehicle.orientation.x << ',' << vehicle.orientation.y << ','
         << vehicle.orientation.z << ',' << vehicle.orientation.w << ','
         << vehicle.current_room_name << ',' << vehicle.current_room_id << ','
         << (vehicle.current_room_resolved ? 1 : 0);
}

void write_local_csv(std::ostream& output,
                     const ht2mp::game::ObservedAiPlayer& local) {
  const auto position =
      ht2mp::game::DecodePositionIdDiagnostics(local.raw_position_id);
  output << ',' << local.vehicle_id << ',' << local.actor_type << ','
         << local.self_player_id << ',' << (local.self_matches_node ? 1 : 0)
         << ','
         << local.liveness_marker << ','
         << (local.constructor_marker_matches ? 1 : 0) << ','
         << local.flags << ',' << local.position_room_a << ','
         << local.position_room_b << ','
         << (local.position_rooms_in_bounds ? 1 : 0);
  for (const auto dword : position.dwords) output << ',' << dword;
  output << ',';
  if (position.distance_candidate_finite) {
    output << std::setprecision(17) << position.distance_candidate;
  }
  output << ',' << (position.distance_candidate_finite ? 1 : 0);
}

void write_local_console(std::ostream& output,
                         const ht2mp::game::AiPlayerPoolSnapshot& snapshot,
                         const ht2mp::game::ObservedAiPlayer* local) {
  if (!snapshot.has_snapshot()) return;
  output << " local_pid=0x" << std::hex << snapshot.local_player_id << std::dec
         << " local_valid=" << (snapshot.local_player_valid ? 1 : 0)
         << " world_ready=" << (snapshot.local_world_ready ? 1 : 0)
         << " ai_initialized="
         << (snapshot.ai_subsystem_initialized ? 1 : 0)
         << " rooms=" << snapshot.room_registry_count
         << " viewer=0x" << std::hex
         << snapshot.local_vehicle.viewer_address << " vehicle_ptr=0x"
         << snapshot.local_vehicle.vehicle_address << " owner=0x"
         << snapshot.local_vehicle.owner_player_id << " physics=0x"
         << snapshot.local_vehicle.physics_address << " moving_item=0x"
         << snapshot.local_vehicle.moving_item_address
         << " physics_reverse=0x"
         << snapshot.local_vehicle.physics_reverse_moving_item << std::dec
         << " vehicle_chain="
         << (snapshot.local_vehicle.chain_consistent ? 1 : 0)
         << " owner_match="
         << (snapshot.local_vehicle.owner_matches_local ? 1 : 0)
         << " physics_reverse_match="
         << (snapshot.local_vehicle.physics_reverse_matches ? 1 : 0)
         << " vehicle_pos=(" << snapshot.local_vehicle.position[0] << ','
         << snapshot.local_vehicle.position[1] << ','
         << snapshot.local_vehicle.position[2] << ") vehicle_q=("
         << snapshot.local_vehicle.orientation.x << ','
         << snapshot.local_vehicle.orientation.y << ','
         << snapshot.local_vehicle.orientation.z << ','
         << snapshot.local_vehicle.orientation.w << ") orientation_valid="
         << (snapshot.local_vehicle.orientation_valid ? 1 : 0)
         << " current_room=\""
         << snapshot.local_vehicle.current_room_name << "\"/"
         << snapshot.local_vehicle.current_room_id << " room_resolved="
         << (snapshot.local_vehicle.current_room_resolved ? 1 : 0);
  if (local == nullptr) {
    output << " local_actor=<unavailable>";
    return;
  }

  const auto position =
      ht2mp::game::DecodePositionIdDiagnostics(local->raw_position_id);
  output << " vehicle=" << local->vehicle_id
         << " actor_type=" << local->actor_type << " self_pid=0x" << std::hex
         << local->self_player_id << std::dec
         << " self_match=" << (local->self_matches_node ? 1 : 0)
         << " liveness=0x" << std::hex
         << local->liveness_marker << std::dec
          << " constructor_marker="
          << (local->constructor_marker_matches ? 1 : 0)
          << " flags=0x" << std::hex
          << local->flags << std::dec
          << " endpoint_rooms=[" << local->position_room_a << ','
          << local->position_room_b << "] room_bounds="
          << (local->position_rooms_in_bounds ? 1 : 0) << std::hex
          << " posid=[";
  for (std::size_t index = 0; index < position.dwords.size(); ++index) {
    if (index != 0U) output << ',';
    output << "0x" << position.dwords[index];
  }
  output << ']' << std::dec << " distance_candidate=";
  if (position.distance_candidate_finite) {
    output << std::setprecision(17) << position.distance_candidate;
  } else {
    output << "<non-finite>";
  }
}

}  // namespace

int main(int argc, char** argv) {
  const auto options = parse_options(argc, argv);
  if (!options) {
    return argc > 1 && (std::string_view(argv[1]) == "--help" ||
                        std::string_view(argv[1]) == "-h")
               ? 0
               : 2;
  }

  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ |
                                 SYNCHRONIZE,
                             FALSE, options->pid));
  if (process.get() == nullptr) {
    std::cerr << "Cannot open PID " << options->pid << " read-only (Win32 "
              << GetLastError() << ")\n";
    return 3;
  }
  const auto executable = process_path(process.get());
  const auto base = module_base(options->pid);
  if (!executable || !base) {
    std::cerr << "Cannot resolve executable path or main-module base\n";
    return 3;
  }
  const auto verified =
      ht2mp::game::VerifyGameExecutable(*executable, options->profile);
  if (!verified.accepted()) {
    std::cerr << "Profile verification rejected " << executable->string() << '\n';
    for (const auto& error : verified.errors) std::cerr << "  " << error << '\n';
    return 4;
  }

  ProcessMemory memory(process.get());
  std::string error;
  auto transform = ht2mp::game::LocalTransformObserver::Create(
      verified, *base, memory, error);
  if (!transform) {
    std::cerr << "Observer is not enabled: " << error << '\n';
    return 5;
  }
  auto pool = ht2mp::game::AiPlayerPoolObserver::Create(
      verified, *base, memory, error);
  if (!pool) {
    std::cerr << "AI pool observer is not enabled: " << error << '\n';
    return 5;
  }
  auto world = ht2mp::game::LocalWorldObserver::Create(
      verified, *base, memory, error);
  if (!world) {
    std::cerr << "Fast local-world observer is not enabled: " << error << '\n';
    return 5;
  }
  const auto& vehicle_layout = verified.profile->ai_player_pool.local_vehicle;

  std::ofstream csv;
  if (!options->csv.empty()) {
    csv.open(options->csv, std::ios::out | std::ios::trunc);
    if (!csv) {
      std::cerr << "Cannot create CSV: " << options->csv.string() << '\n';
      return 6;
    }
    csv << "sample,time_us,status,x,y,z,qx,qy,qz,qw,vx,vy,vz,teleport,"
           "ai_status,ai_players,ai_initialized,room_registry_count,"
           "local_player_id,local_valid,local_world_ready,fast_world_status,"
           "fast_world_room_id,viewer_address,local_vehicle_address,"
           "vehicle_owner_player_id,vehicle_physics_address,vehicle_moving_item_address,"
           "physics_reverse_moving_item,vehicle_chain_consistent,"
           "vehicle_owner_matches_local,vehicle_physics_available,physics_reverse_matches,"
           "vehicle_position_finite,vehicle_x,vehicle_y,vehicle_z,"
           "vehicle_orientation_valid,vehicle_qx,vehicle_qy,vehicle_qz,vehicle_qw,"
           "current_room_name,current_room_id,current_room_resolved,"
           "vehicle_id,actor_type,self_player_id,self_matches_node,liveness_marker,"
           "constructor_marker,flags,endpoint_room_a,endpoint_room_b,"
           "endpoint_rooms_in_bounds,position_id_u32_0,"
           "position_id_u32_1,position_id_u32_2,position_id_u32_3,"
           "position_id_u32_4,position_id_u32_5,position_id_u32_6,"
           "position_id_u32_7,position_distance_candidate,"
           "position_distance_finite,"
           "sim_status,sim_x,sim_y,sim_z,sim_qx,sim_qy,sim_qz,sim_qw,"
           "rv_status,rv_x,rv_y,rv_z,rv_qx,rv_qy,rv_qz,rv_qw,"
           "rs_status,rs_x,rs_y,rs_z,rs_qx,rs_qy,rs_qz,rs_qw\n";
  }

  std::cout << "Accepted " << options->profile << " at " << executable->string()
            << ", module base 0x" << std::hex << *base << std::dec << '\n'
            << "Read-only transform 0x" << std::hex
            << transform->transform_address() << std::dec << "; sampling at "
            << options->frequency_hz << " Hz for up to "
            << options->duration_seconds << " seconds\n";

  SetConsoleCtrlHandler(console_handler, TRUE);
  const auto started = std::chrono::steady_clock::now();
  auto next = started;
  auto next_report = started;
  std::size_t valid_samples{};
  std::size_t invalid_samples{};
  std::size_t last_ai_count = std::numeric_limits<std::size_t>::max();
  const auto interval = std::chrono::microseconds(1'000'000U / options->frequency_hz);
  while (g_running.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() - started <
             std::chrono::seconds(options->duration_seconds)) {
    const auto wait = WaitForSingleObject(process.get(), 0U);
    if (wait == WAIT_OBJECT_0) break;
    if (wait == WAIT_FAILED) {
      std::cerr << "Cannot query target process state\n";
      break;
    }

    const auto timestamp = monotonic_us();
    const auto observed = transform->Poll(timestamp);
    const auto ai = pool->Snapshot();
    const auto fast_world = world->PollFast(timestamp);
    const auto* local = find_cached_local(ai);
    const auto local_vehicle_address =
        ai.has_snapshot() && ai.local_vehicle.chain_consistent
            ? ai.local_vehicle.vehicle_address
            : 0U;
    const auto local_pair = read_vehicle_transforms(
        memory, vehicle_layout, local_vehicle_address);
    const auto remote_pair = read_vehicle_transforms(
        memory, vehicle_layout, options->vehicle_address);
    if (observed.has_sample()) {
      ++valid_samples;
    } else {
      ++invalid_samples;
    }
    if (csv) {
      csv << (valid_samples + invalid_samples) << ','
          << observed.sample.monotonic_time_us << ','
          << ht2mp::game::ToString(observed.status) << ','
          << std::setprecision(10) << observed.sample.position.x << ','
          << observed.sample.position.y << ',' << observed.sample.position.z << ','
          << observed.sample.orientation.x << ',' << observed.sample.orientation.y << ','
          << observed.sample.orientation.z << ',' << observed.sample.orientation.w << ','
          << observed.sample.linear_velocity.x << ','
          << observed.sample.linear_velocity.y << ','
          << observed.sample.linear_velocity.z << ','
          << (observed.sample.teleport ? 1 : 0) << ','
          << ht2mp::game::ToString(ai.status) << ','
          << (ai.has_snapshot() ? ai.players.size() : 0U) << ',';
      if (ai.has_snapshot()) {
        csv << (ai.ai_subsystem_initialized ? 1 : 0) << ','
            << ai.room_registry_count << ',' << ai.local_player_id << ','
            << (ai.local_player_valid ? 1 : 0) << ','
            << (ai.local_world_ready ? 1 : 0);
      } else {
        csv << ",,,,";
      }
      csv << ',' << ht2mp::game::ToString(fast_world.status) << ',';
      if (fast_world.has_sample()) {
        csv << fast_world.sample.current_room_id;
      }
      if (ai.has_snapshot()) {
        write_vehicle_csv(csv, ai.local_vehicle);
      } else {
        write_empty_vehicle_columns(csv);
      }
      if (local != nullptr) {
        write_local_csv(csv, *local);
      } else {
        write_empty_local_columns(csv);
      }
      write_transform_csv(csv, local_pair.simulation_status,
                          local_pair.simulation);
      write_transform_csv(csv, remote_pair.render_status, remote_pair.render);
      write_transform_csv(csv, remote_pair.simulation_status,
                          remote_pair.simulation);
      csv << '\n';
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= next_report ||
        (ai.has_snapshot() && ai.players.size() != last_ai_count)) {
      std::cout << "sample=" << (valid_samples + invalid_samples)
                << " transform=" << ht2mp::game::ToString(observed.status);
      if (observed.has_sample()) {
        std::cout << " pos=(" << observed.sample.position.x << ','
                  << observed.sample.position.y << ','
                  << observed.sample.position.z << ')';
      } else if (!observed.detail.empty()) {
        std::cout << " detail=\"" << observed.detail << '\"';
      }
      std::cout << " ai=" << ht2mp::game::ToString(ai.status);
      if (ai.has_snapshot()) {
        std::cout << '[' << ai.players.size() << ']';
        last_ai_count = ai.players.size();
      }
      write_local_console(std::cout, ai, local);
      std::cout << " fast_world=" << ht2mp::game::ToString(fast_world.status);
      if (fast_world.has_sample()) {
        std::cout << "[room=" << fast_world.sample.current_room_id << "]";
      }
      if (local_pair.simulation_status ==
          ht2mp::game::ObservationStatus::sample) {
        std::cout << " sim_pos=(" << local_pair.simulation.position.x << ','
                  << local_pair.simulation.position.y << ','
                  << local_pair.simulation.position.z << ')';
      }
      if (options->vehicle_address != 0U) {
        std::cout << " remote="
                  << ht2mp::game::ToString(remote_pair.render_status);
        if (remote_pair.render_status ==
            ht2mp::game::ObservationStatus::sample) {
          std::cout << " remote_pos=(" << remote_pair.render.position.x << ','
                    << remote_pair.render.position.y << ','
                    << remote_pair.render.position.z << ')';
        }
      }
      std::cout << '\n';
      next_report = now + std::chrono::seconds(1);
    }

    next += interval;
    std::this_thread::sleep_until(next);
    if (std::chrono::steady_clock::now() > next + interval * 4) {
      next = std::chrono::steady_clock::now();
    }
  }
  SetConsoleCtrlHandler(console_handler, FALSE);
  std::cout << "Observer stopped: valid=" << valid_samples
            << ", not-valid=" << invalid_samples << '\n';
  return valid_samples == 0U ? 7 : 0;
}
