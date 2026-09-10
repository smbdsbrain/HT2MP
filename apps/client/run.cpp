#include "run.hpp"

#include "network_adapter.hpp"
#include "pipe_server.hpp"
#include "staging.hpp"

#include "ht2mp/game/profile.hpp"
#include "ht2mp/ipc/bootstrap.hpp"
#include "ht2mp/ipc/authentication.hpp"
#include "ht2mp/ipc/codec.hpp"
#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace ht2mp::client {
namespace fs = std::filesystem;
namespace {

class Handle final {
public:
  Handle() noexcept = default;
  explicit Handle(HANDLE value) noexcept : value_(value) {}
  ~Handle() {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
      CloseHandle(value_);
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
        CloseHandle(value_);
      }
      value_ = other.release();
    }
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] HANDLE release() noexcept {
    const auto result = value_;
    value_ = nullptr;
    return result;
  }

private:
  HANDLE value_{};
};

struct Process final {
  Handle process;
  Handle thread;
  DWORD id{};
};

bool resolve_auto_enter_driver(const fs::path& stage,
                               fs::path& driver,
                               std::string& error) {
  driver.clear();
  std::error_code iterator_error;
  std::vector<fs::path> candidates;
  for (fs::directory_iterator current(stage, iterator_error), end;
       !iterator_error && current != end; current.increment(iterator_error)) {
    std::error_code type_error;
    if (!current->is_regular_file(type_error) || type_error) continue;
    if (_wcsicmp(current->path().extension().c_str(), L".pl1") == 0) {
      candidates.push_back(current->path());
    }
  }
  if (iterator_error) {
    error = "Cannot enumerate staged driver profiles: " +
            iterator_error.message();
    return false;
  }
  if (candidates.size() != 1U) {
    std::ostringstream detail;
    detail << "--auto-enter-world requires exactly one .pl1 driver in the "
              "staged runtime; found "
           << candidates.size();
    error = detail.str();
    return false;
  }
  driver = std::move(candidates.front());
  return true;
}

std::atomic_bool g_stop_requested{false};

// Online playback forwards every datagram within a few milliseconds, which is
// only meaningful when Sleep/wait granularity is 1 ms rather than 15.6 ms.
class TimerResolution final {
public:
  explicit TimerResolution(const bool enable) noexcept
      : active_(enable && timeBeginPeriod(1U) == TIMERR_NOERROR) {}
  ~TimerResolution() {
    if (active_) timeEndPeriod(1U);
  }
  TimerResolution(const TimerResolution&) = delete;
  TimerResolution& operator=(const TimerResolution&) = delete;

private:
  bool active_{};
};

BOOL WINAPI console_handler(const DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
    g_stop_requested.store(true, std::memory_order_relaxed);
    return TRUE;
  }
  return FALSE;
}

std::string hex_bytes(const std::span<const std::uint8_t> bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output(bytes.size() * 2U, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    output[i * 2U] = kHex[bytes[i] >> 4U];
    output[i * 2U + 1U] = kHex[bytes[i] & 0xFU];
  }
  return output;
}

std::wstring make_command_line(const fs::path& executable,
                               const std::vector<std::wstring>& arguments) {
  std::wstring command =
      ht2mp::windows::quote_command_line_argument(executable.wstring());
  for (const auto& argument : arguments) {
    command.push_back(L' ');
    command += ht2mp::windows::quote_command_line_argument(argument);
  }
  return command;
}

bool create_process(const fs::path& executable,
                    const std::vector<std::wstring>& arguments,
                    const fs::path& working_directory,
                    const DWORD creation_flags,
                    Process& process,
                    std::string& error,
                    const HANDLE child_output = nullptr) {
  auto command = make_command_line(executable, arguments);
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (child_output != nullptr && child_output != INVALID_HANDLE_VALUE) {
    startup.dwFlags |= STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = child_output;
    startup.hStdError = child_output;
  }
  PROCESS_INFORMATION information{};
  const BOOL inherit_handles = child_output != nullptr ? TRUE : FALSE;
  if (!CreateProcessW(executable.c_str(), writable.data(), nullptr, nullptr,
                      inherit_handles,
                      creation_flags, nullptr,
                      working_directory.empty() ? nullptr : working_directory.c_str(),
                      &startup, &information)) {
    error = "Cannot launch " + executable.string() + ": " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  process.process = Handle(information.hProcess);
  process.thread = Handle(information.hThread);
  process.id = information.dwProcessId;
  return true;
}

bool verify_binary(const fs::path& path, const char* description, std::string& error) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) {
    error = std::string(description) + " does not exist: " + path.string();
    return false;
  }
  if (!ht2mp::windows::is_i386_pe(path, error)) {
    error = std::string(description) + " is not an x86 PE: " + error;
    return false;
  }
  return true;
}

bool loaded_module_set(
                       const DWORD process_id,
                       const fs::path& staged_root,
                       const std::span<const ProtectedModuleDescriptor> required,
                       std::vector<std::wstring>& missing,
                       std::string& error) {
  error.clear();
  for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
    missing.clear();
    for (const auto& module : required) missing.emplace_back(module.filename);

    Handle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
      const DWORD code = GetLastError();
      if (code == ERROR_BAD_LENGTH) {
        Sleep(10U);
        continue;
      }
      error = "Cannot enumerate king.exe modules: " +
              ht2mp::windows::win32_error(code);
      return false;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Module32FirstW(snapshot.get(), &entry)) {
      const DWORD code = GetLastError();
      if (code == ERROR_BAD_LENGTH) {
        Sleep(10U);
        continue;
      }
      error = "Cannot read king.exe module snapshot: " +
              ht2mp::windows::win32_error(code);
      return false;
    }

    do {
      const auto found = std::find_if(
          missing.begin(), missing.end(), [&entry](const std::wstring& wanted) {
            return _wcsicmp(wanted.c_str(), entry.szModule) == 0;
          });
      if (found != missing.end()) {
        const auto expected = staged_root / *found;
        std::string identity_error;
        if (!ht2mp::windows::same_existing_file(
                expected, fs::path(entry.szExePath), identity_error)) {
          error = "Loaded module " + expected.filename().string() +
                  " is not the exact staged file";
          if (!identity_error.empty()) error += ": " + identity_error;
          return false;
        }
        missing.erase(found);
      }
    } while (Module32NextW(snapshot.get(), &entry));

    const DWORD code = GetLastError();
    if (code == ERROR_BAD_LENGTH) {
      Sleep(10U);
      continue;
    }
    if (code != ERROR_NO_MORE_FILES) {
      error = "Cannot finish reading king.exe module snapshot: " +
              ht2mp::windows::win32_error(code);
      return false;
    }
    return true;
  }

  error = "Cannot obtain a stable king.exe module snapshot after retries";
  return false;
}

bool wait_for_required_modules(const EditionDescriptor& edition,
                               const fs::path& staged_root, HANDLE process,
                               const DWORD process_id,
                               std::string& error) {
  const auto deadline = GetTickCount64() + 15'000U;
  std::vector<std::wstring> missing;
  while (GetTickCount64() < deadline) {
    if (WaitForSingleObject(process, 0U) == WAIT_OBJECT_0) {
      error = "king.exe exited before required modules were loaded";
      return false;
    }
    std::string snapshot_error;
    if (loaded_module_set(process_id, staged_root, edition.protected_modules,
                          missing, snapshot_error) &&
        missing.empty()) {
      return true;
    }
    if (!snapshot_error.empty()) {
      error = std::move(snapshot_error);
      return false;
    }
    Sleep(50U);
  }
  std::ostringstream detail;
  detail << "Timed out waiting for required game modules:";
  for (const auto& module : missing) {
    std::string conversion_error;
    detail << ' ' << ht2mp::windows::narrow_utf8(module, conversion_error);
  }
  error = detail.str();
  return false;
}

bool run_injector(const RunOptions& options, const fs::path& king,
                  const DWORD game_pid, const std::uint64_t run_id,
                  const std::array<std::uint8_t, ht2mp::ipc::kNonceSize>& nonce,
                  const std::wstring& pipe_name, std::string& error) {
  std::string conversion_error;
  const auto profile = ht2mp::windows::widen_utf8(options.edition.profile_id,
                                                  conversion_error);
  if (!conversion_error.empty()) {
    error = conversion_error;
    return false;
  }
  std::ostringstream run_id_stream;
  run_id_stream << run_id;
  std::vector<std::wstring> arguments{
      L"--pid", std::to_wstring(game_pid),
      L"--exe", king.wstring(),
      L"--sha256", ht2mp::windows::widen_utf8(options.edition.king_sha256,
                                               conversion_error),
      L"--dll", options.bridge_path.wstring(),
      L"--profile", profile,
      L"--pipe", pipe_name,
      L"--nonce", ht2mp::windows::widen_utf8(hex_bytes(nonce), conversion_error),
      L"--run-id", std::to_wstring(run_id),
  };
  if (!conversion_error.empty()) {
    error = conversion_error;
    return false;
  }
  if (options.experimental_gog_telemetry) {
    arguments.emplace_back(L"--experimental-telemetry");
  }
  if (options.experimental_gog_actor_diagnostics) {
    arguments.emplace_back(L"--experimental-gog-actor-diagnostics");
  }
  if (options.experimental_gog_remote_actors) {
    arguments.emplace_back(L"--experimental-remote-actors");
  }
  if (options.online_mode) {
    arguments.emplace_back(L"--online-mode");
  }
  if (options.auto_enter_world) {
    arguments.emplace_back(L"--auto-enter-world");
  }
  if (options.vehicle_type != kDiscoverVehicleType) {
    arguments.emplace_back(L"--vehicle-selector");
    arguments.emplace_back(std::to_wstring(options.vehicle_type));
    arguments.emplace_back(L"--paint");
    arguments.emplace_back(std::to_wstring(options.paint_variant));
  }

  SECURITY_ATTRIBUTES pipe_attributes{};
  pipe_attributes.nLength = sizeof(pipe_attributes);
  pipe_attributes.bInheritHandle = TRUE;
  HANDLE read_raw{};
  HANDLE write_raw{};
  if (!CreatePipe(&read_raw, &write_raw, &pipe_attributes, 0U)) {
    error = "Cannot create injector diagnostics pipe: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  Handle diagnostic_read(read_raw);
  Handle diagnostic_write(write_raw);
  if (!SetHandleInformation(diagnostic_read.get(), HANDLE_FLAG_INHERIT, 0U)) {
    error = "Cannot protect injector diagnostics pipe: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }

  Process injector;
  if (!create_process(options.injector_path, arguments,
                      options.injector_path.parent_path(), CREATE_NO_WINDOW,
                      injector, error, diagnostic_write.get())) {
    return false;
  }
  diagnostic_write = Handle{};

  std::string diagnostics;
  const auto drain_diagnostics = [&] {
    constexpr std::size_t kDiagnosticLimit = 16U * 1024U;
    std::array<char, 512> buffer{};
    for (;;) {
      DWORD available{};
      if (!PeekNamedPipe(diagnostic_read.get(), nullptr, 0U, nullptr,
                         &available, nullptr)) {
        return GetLastError() == ERROR_BROKEN_PIPE;
      }
      if (available == 0U) return true;
      DWORD count{};
      const auto wanted = std::min<DWORD>(available,
                                          static_cast<DWORD>(buffer.size()));
      if (!ReadFile(diagnostic_read.get(), buffer.data(), wanted,
                    &count, nullptr)) {
        return GetLastError() == ERROR_BROKEN_PIPE;
      }
      if (count == 0U) return true;
      const auto remaining = kDiagnosticLimit -
                             std::min(kDiagnosticLimit, diagnostics.size());
      diagnostics.append(buffer.data(),
                         std::min<std::size_t>(remaining, count));
    }
  };

  const auto finish_diagnostics = [&] {
    (void)drain_diagnostics();
    const auto first = diagnostics.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      diagnostics.clear();
      return;
    }
    diagnostics.erase(0U, first);
    const auto last = diagnostics.find_last_not_of(" \t\r\n");
    diagnostics.erase(last + 1U);
  };
  const auto append_diagnostics = [&] {
    if (!diagnostics.empty()) {
      error += "\nInjector diagnostics:\n" + diagnostics;
    }
  };

  DWORD wait = WAIT_TIMEOUT;
  DWORD wait_error{};
  const auto deadline = GetTickCount64() + 30'000ULL;
  while (wait == WAIT_TIMEOUT && GetTickCount64() < deadline) {
    (void)drain_diagnostics();
    const auto remaining = deadline - GetTickCount64();
    const auto slice = static_cast<DWORD>(std::min<std::uint64_t>(50U, remaining));
    wait = WaitForSingleObject(injector.process.get(), slice);
    if (wait == WAIT_FAILED) wait_error = GetLastError();
  }
  if (wait == WAIT_TIMEOUT) {
    const bool terminated = TerminateProcess(injector.process.get(), 124U) != FALSE;
    const auto terminated_wait =
        terminated ? WaitForSingleObject(injector.process.get(), 5'000U)
                   : WAIT_FAILED;
    finish_diagnostics();
    error = "Injector timed out after 30 seconds";
    if (!terminated || terminated_wait != WAIT_OBJECT_0) {
      error += "; injector termination could not be confirmed";
    }
    append_diagnostics();
    return false;
  }
  if (wait != WAIT_OBJECT_0) {
    const bool terminated = TerminateProcess(injector.process.get(), 125U) != FALSE;
    if (terminated) {
      (void)WaitForSingleObject(injector.process.get(), 5'000U);
    }
    finish_diagnostics();
    error = "Cannot wait for injector: " +
            ht2mp::windows::win32_error(wait_error);
    append_diagnostics();
    return false;
  }
  finish_diagnostics();
  DWORD exit_code{};
  if (!GetExitCodeProcess(injector.process.get(), &exit_code)) {
    error = "Cannot read injector result: " +
            ht2mp::windows::win32_error(GetLastError());
    append_diagnostics();
    return false;
  }
  if (exit_code != 0U) {
    error = "Injector rejected the launch (exit code " + std::to_string(exit_code) + ")";
    append_diagnostics();
    return false;
  }
  return true;
}

bool read_and_validate_handshake(
    PipeServer& pipe,
    const EditionDescriptor& edition,
    const bool allow_experimental_remote_actors,
    const std::uint64_t run_id,
    const std::array<std::uint8_t, ht2mp::ipc::kNonceSize>& nonce,
    std::uint32_t& capabilities,
    ht2mp::ipc::BridgeMode& mode,
    std::string& error) {
  capabilities = 0U;
  mode = ht2mp::ipc::BridgeMode::safe;
  ReceivedFrame frame;
  if (!pipe.read(frame, error)) {
    return false;
  }
  if (frame.header.type != ht2mp::ipc::MessageType::bridge_hello) {
    error = "Bridge sent a non-hello frame first";
    return false;
  }
  ht2mp::ipc::BridgeHelloV1 hello;
  if (ht2mp::ipc::decode_bridge_hello(frame.payload(), hello) !=
      ht2mp::ipc::CodecError::none) {
    error = "Malformed BridgeHello";
    return false;
  }
  const ht2mp::ipc::BridgeExpectation expected{
      run_id, nonce, edition.profile_id};
  const auto authentication =
      ht2mp::ipc::authenticate_bridge_hello(hello, expected);
  if (authentication != ht2mp::ipc::AuthenticationError::none) {
    error = "BridgeHello authentication failed: " +
            std::string(ht2mp::ipc::to_string(authentication));
    return false;
  }

  if (!pipe.read(frame, error)) {
    return false;
  }
  if (frame.header.type != ht2mp::ipc::MessageType::bridge_ready) {
    error = "Bridge did not send BridgeReady";
    return false;
  }
  ht2mp::ipc::BridgeReadyV1 ready;
  if (ht2mp::ipc::decode_bridge_ready(frame.payload(), ready) !=
      ht2mp::ipc::CodecError::none) {
    error = "Malformed BridgeReady";
    return false;
  }
  constexpr auto kObserver =
      static_cast<std::uint32_t>(ht2mp::ipc::Capability::observer_mode);
  constexpr auto kLocalTelemetry =
      static_cast<std::uint32_t>(ht2mp::ipc::Capability::local_telemetry);
  constexpr auto kRemoteActors =
      static_cast<std::uint32_t>(ht2mp::ipc::Capability::remote_actors);
  constexpr auto kAutoEnter =
      static_cast<std::uint32_t>(ht2mp::ipc::Capability::auto_enter_world);
  constexpr auto kStockNpcSuppression = static_cast<std::uint32_t>(
      ht2mp::ipc::Capability::stock_npc_suppression);
  constexpr auto kBackgroundTick = static_cast<std::uint32_t>(
      ht2mp::ipc::Capability::background_tick);
  constexpr auto kReplication = static_cast<std::uint32_t>(ht2mp::ipc::Capability::vehicle_state) |
      static_cast<std::uint32_t>(ht2mp::ipc::Capability::environment);
  constexpr auto kKnownCapabilities =
      kObserver | kLocalTelemetry | kRemoteActors | kAutoEnter |
      kStockNpcSuppression | kBackgroundTick | kReplication;
  if (ready.result != ht2mp::ipc::BootstrapResult::ok ||
      (ready.capabilities & ~kKnownCapabilities) != 0U ||
      (ready.capabilities & kObserver) == 0U) {
    error = "Bridge advertised an invalid result or capability mask";
    return false;
  }
  const auto* profile =
      ht2mp::game::FindGameProfile(edition.profile_id);
  const bool profile_allows_writes =
      profile != nullptr &&
      (profile->legacy_multiplayer.runtime_write_validated ||
       (allow_experimental_remote_actors &&
        (edition.profile_id == "gog-05588140" ||
         edition.profile_id == "steam-8138acee")));
  if (ready.mode == ht2mp::ipc::BridgeMode::safe &&
      (ready.capabilities &
       (kLocalTelemetry | kRemoteActors | kAutoEnter | kStockNpcSuppression |
        kBackgroundTick | kReplication)) != 0U) {
    error = "Safe bridge advertised active telemetry or actor capabilities";
    return false;
  }
  if (ready.mode == ht2mp::ipc::BridgeMode::observer &&
      (ready.capabilities &
       (kRemoteActors | kStockNpcSuppression | kBackgroundTick | kReplication)) != 0U) {
    error = "Observer bridge advertised online-world writes";
    return false;
  }
  if (ready.mode == ht2mp::ipc::BridgeMode::active &&
      (((ready.capabilities & kRemoteActors) == 0U) ||
       !profile_allows_writes)) {
    error = "Active bridge is not authorized by the exact game profile";
    return false;
  }
  capabilities = ready.capabilities;
  mode = ready.mode;
  return true;
}

bool send_observer_mode(PipeServer& pipe, std::string& error) {
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> frame{};
  std::size_t size{};
  const ht2mp::ipc::SetObserverModeV1 command{true};
  if (ht2mp::ipc::encode_set_observer_mode(command, frame, size) !=
      ht2mp::ipc::CodecError::none) {
    error = "Cannot encode observer-mode command";
    return false;
  }
  return pipe.write(std::span(frame).first(size), error);
}

void report_frame(const ReceivedFrame& frame) {
  if (frame.header.type != ht2mp::ipc::MessageType::bridge_status) {
    return;
  }
  ht2mp::ipc::BridgeStatusV1 status;
  if (ht2mp::ipc::decode_bridge_status(frame.payload(), status) !=
      ht2mp::ipc::CodecError::none) {
    std::cerr << "bridge: malformed status frame\n";
    return;
  }
  const auto end = std::find(status.detail.begin(), status.detail.end(), '\0');
  std::cerr << "bridge[" << static_cast<std::uint32_t>(status.code) << "]: "
            << std::string_view(status.detail.data(),
                                static_cast<std::size_t>(end - status.detail.begin()))
            << '\n';
}

void request_safe_mode(PipeServer& pipe, const std::uint32_t reason) noexcept {
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> frame{};
  std::size_t size{};
  const ht2mp::ipc::EnterSafeModeV1 command{reason};
  if (ht2mp::ipc::encode_enter_safe_mode(command, frame, size) ==
      ht2mp::ipc::CodecError::none) {
    std::string ignored;
    (void)pipe.write(std::span(frame).first(size), ignored);
  }
}

struct CloseWindowContext final {
  DWORD process_id{};
  bool posted{};
};

BOOL CALLBACK close_game_window(HWND window, LPARAM raw_context) {
  auto& context = *reinterpret_cast<CloseWindowContext*>(raw_context);
  DWORD owner{};
  GetWindowThreadProcessId(window, &owner);
  if (owner == context.process_id && IsWindowVisible(window) &&
      GetWindow(window, GW_OWNER) == nullptr) {
    context.posted = PostMessageW(window, WM_CLOSE, 0U, 0U) != FALSE ||
                     context.posted;
  }
  return TRUE;
}

bool request_game_close(const DWORD process_id) noexcept {
  CloseWindowContext context{process_id, false};
  EnumWindows(close_game_window, reinterpret_cast<LPARAM>(&context));
  return context.posted;
}

void status_event(const bool enabled, const std::string_view state,
                  const std::string_view detail = {}) {
  if (!enabled) return;
  std::cout << "HT2MP-EVENT/1 state=" << state;
  if (!detail.empty()) std::cout << " detail=" << detail;
  std::cout << '\n' << std::flush;
}

} // namespace

bool run_staged_game(const RunOptions& options, unsigned long& game_exit_code,
                     std::string& error) {
  game_exit_code = STILL_ACTIVE;
  error.clear();
  g_stop_requested.store(false, std::memory_order_relaxed);

  const auto configured_stage =
      staged_game_directory(options.edition, error, options.instance);
  if (!error.empty()) {
    return false;
  }
  StageValidation stage_validation;
  if (!validate_staged_game(options.edition, configured_stage,
                            stage_validation, error, options.instance)) {
    return false;
  }
  const auto& stage = stage_validation.runtime;
  const auto& king = stage_validation.king;
  fs::path auto_enter_driver;
  if (options.auto_enter_world &&
      !resolve_auto_enter_driver(stage, auto_enter_driver, error)) {
    return false;
  }
  if (!verify_binary(options.injector_path, "Injector", error) ||
      !verify_binary(options.bridge_path, "Bridge DLL", error)) {
    return false;
  }

  Handle shutdown_event;
  if (!options.shutdown_event.empty()) {
    shutdown_event = Handle(CreateEventW(nullptr, TRUE, FALSE,
                                         options.shutdown_event.c_str()));
    if (shutdown_event.get() == nullptr) {
      error = "Cannot create/open shutdown event: " +
              ht2mp::windows::win32_error(GetLastError());
      return false;
    }
  }

  std::array<std::uint8_t, ht2mp::ipc::kNonceSize> nonce{};
  std::array<std::uint8_t, sizeof(std::uint64_t)> run_bytes{};
  if (!ht2mp::windows::secure_random(nonce, error) ||
      !ht2mp::windows::secure_random(run_bytes, error)) {
    return false;
  }
  std::uint64_t run_id{};
  std::memcpy(&run_id, run_bytes.data(), sizeof(run_id));
  if (run_id == 0U) {
    run_id = 1U;
  }
  std::wostringstream pipe_name_stream;
  pipe_name_stream << L"\\\\.\\pipe\\HT2MP-" << GetCurrentProcessId() << L'-'
                   << std::hex << run_id;
  const auto pipe_name = pipe_name_stream.str();

  PipeServer pipe;
  if (!pipe.create(pipe_name, error)) {
    return false;
  }

  Process game;
  if (!create_process(king, options.game_arguments, stage, CREATE_NEW_PROCESS_GROUP,
                      game, error)) {
    return false;
  }
  std::cout << "Started staged king.exe (PID " << game.id << ")\n";
  status_event(options.status_events, "game-started",
               "pid=" + std::to_string(game.id));

  const auto input_idle = WaitForInputIdle(game.process.get(), 15'000U);
  if (input_idle == WAIT_TIMEOUT) {
    error = "king.exe did not become input-idle within 15 seconds; no injection was attempted";
    return false;
  }
  if (input_idle == WAIT_FAILED && GetLastError() != ERROR_NOT_GUI_PROCESS) {
    error = "king.exe did not initialize its GUI: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  if (!wait_for_required_modules(options.edition, stage, game.process.get(),
                                 game.id, error)) {
    error += "; king.exe was left running without injection";
    return false;
  }
  if (!run_injector(options, king, game.id, run_id, nonce, pipe_name, error)) {
    error += "; king.exe was left running in its fail-closed unmodified state";
    return false;
  }
  std::uint32_t bridge_capabilities{};
  ht2mp::ipc::BridgeMode bridge_mode{};
  if (!pipe.connect(error) ||
      !read_and_validate_handshake(pipe, options.edition,
                                   options.experimental_gog_remote_actors,
                                   run_id, nonce,
                                   bridge_capabilities, bridge_mode, error)) {
    error += "; king.exe remains running and the bridge performs no memory writes";
    return false;
  }
  const auto auto_enter_capability = static_cast<std::uint32_t>(
      ht2mp::ipc::Capability::auto_enter_world);
  if (options.auto_enter_world &&
      (bridge_capabilities & auto_enter_capability) == 0U) {
    request_safe_mode(pipe, 4U);
    error = "Bridge rejected the requested native auto-enter path; king.exe "
            "was left at the normal menu";
    return false;
  }
  const auto remote_actor_capability = static_cast<std::uint32_t>(
      ht2mp::ipc::Capability::remote_actors);
  if (options.experimental_gog_remote_actors &&
      (bridge_capabilities & remote_actor_capability) == 0U) {
    request_safe_mode(pipe, 5U);
    error = "Bridge rejected the requested exact-profile remote actor backend";
    return false;
  }
  if (options.online_mode) {
    const auto required_online =
        static_cast<std::uint32_t>(ht2mp::ipc::Capability::local_telemetry) |
        static_cast<std::uint32_t>(ht2mp::ipc::Capability::remote_actors) |
        static_cast<std::uint32_t>(
            ht2mp::ipc::Capability::stock_npc_suppression) |
        static_cast<std::uint32_t>(ht2mp::ipc::Capability::background_tick);
    if ((bridge_capabilities & required_online) != required_online) {
      request_safe_mode(pipe, 6U);
      error = "Bridge did not confirm the complete online capability set; "
              "coordinator connection was not attempted";
      return false;
    }
  }
  const auto local_telemetry = static_cast<std::uint32_t>(
      ht2mp::ipc::Capability::local_telemetry);
  if ((bridge_capabilities & local_telemetry) != 0U &&
      !send_observer_mode(pipe, error)) {
    error += "; king.exe remains running and the bridge performs no memory writes";
    return false;
  }

  std::cout << "Bridge authenticated in "
            << (bridge_mode == ht2mp::ipc::BridgeMode::active
                    ? "active"
                    : bridge_mode == ht2mp::ipc::BridgeMode::observer
                          ? "observer"
                          : "passive-safe")
            << " mode for profile " << options.edition.profile_id << "\n";
  status_event(options.status_events, "bridge-ready");
  if (options.auto_enter_world) {
    std::cout << "Native auto-enter selected staged driver: "
              << auto_enter_driver.filename().string() << '\n';
  }
  if (!options.server.empty()) {
    std::cout << "Connecting to coordinator: " << options.server << "\n";
  }

  NetworkOptions network_options;
  network_options.edition = options.edition;
  network_options.endpoint = options.server;
  network_options.token = options.token;
  network_options.player_name = options.player_name;
  network_options.trace_dir = options.trace_dir;
  network_options.vehicle_type =
      options.edition.profile_id == "steam-8138acee"
          ? options.vehicle_type
          : 0U;
  network_options.paint_variant = options.paint_variant;
  network_options.status_events = options.status_events;
  const auto* game_profile =
      ht2mp::game::FindGameProfile(options.edition.profile_id);
  const bool profile_remote_writes_validated =
      game_profile != nullptr &&
      (game_profile->legacy_multiplayer.runtime_write_validated ||
       (options.experimental_gog_remote_actors &&
        (options.edition.profile_id == "gog-05588140" ||
         options.edition.profile_id == "steam-8138acee")));
  NetworkAdapter network(std::move(network_options), bridge_capabilities,
                         bridge_mode, profile_remote_writes_validated);
  if (!network.start(error)) {
    error = "Network adapter could not start: " + error +
            "; king.exe remains in observer-only mode";
    return false;
  }

  SetConsoleCtrlHandler(console_handler, TRUE);
  const TimerResolution timer_resolution(options.online_mode);
  bool pipe_alive = true;
  std::uint64_t local_samples_received{};
  bool shutdown_requested{};
  const auto handle_pipe_failure = [&](const std::string& detail) {
    pipe_alive = false;
    network.stop(pipe);
    pipe.close();
    // king.exe closes the bridge endpoint just before its process handle is
    // signaled.  Give an orderly shutdown a short grace period so a normal
    // exit is not reported as an IPC fault.  If the game remains alive, the
    // same event is still surfaced and the adapter is already fail-closed.
    if (WaitForSingleObject(game.process.get(), 250U) == WAIT_OBJECT_0) {
      std::cout << "Bridge pipe closed during orderly game shutdown\n";
    } else {
      std::cerr << detail << "; game remains fail-closed\n";
    }
  };
  while (!g_stop_requested.load(std::memory_order_relaxed)) {
    if (shutdown_event.get() != nullptr &&
        WaitForSingleObject(shutdown_event.get(), 0U) == WAIT_OBJECT_0) {
      shutdown_requested = true;
      g_stop_requested.store(true, std::memory_order_relaxed);
      status_event(options.status_events, "disconnecting");
      break;
    }
    // Online mode: the loop wakes on every datagram (blocking socket poll
    // below) and otherwise every few milliseconds to relay bridge frames, so
    // a network state reaches the game-side timeline within a frame.
    const auto game_wait =
        WaitForSingleObject(game.process.get(), options.online_mode ? 2U : 50U);
    if (game_wait == WAIT_OBJECT_0) {
      break;
    }
    if (game_wait == WAIT_FAILED) {
      error = "Cannot wait for king.exe: " +
              ht2mp::windows::win32_error(GetLastError());
      network.stop(pipe);
      pipe.close();
      SetConsoleCtrlHandler(console_handler, FALSE);
      return false;
    }
    if (pipe_alive) {
      // Drain a bounded batch so a 20 Hz LocalSample stream cannot starve
      // control/status frames, while still returning to ENet every game tick.
      for (std::size_t drained = 0U; drained < 64U && pipe_alive; ++drained) {
        std::uint32_t available{};
        std::string pipe_error;
        if (!pipe.available(available, pipe_error)) {
          handle_pipe_failure(pipe_error);
          break;
        }
        if (available < ht2mp::ipc::kFrameHeaderSize) {
          break;
        }
        ReceivedFrame frame;
        if (!pipe.read(frame, pipe_error)) {
          handle_pipe_failure(pipe_error);
          break;
        }
        report_frame(frame);
        if (frame.header.type == ht2mp::ipc::MessageType::local_sample) {
          ++local_samples_received;
        }
        if (!network.handle_bridge_frame(frame, pipe_error)) {
          request_safe_mode(pipe, 3U);
          pipe_alive = false;
          network.stop(pipe);
          pipe.close();
          std::cerr << pipe_error << "; game remains fail-closed\n";
          break;
        }
      }
    }
    if (pipe_alive) {
      std::string network_error;
      if (!network.pump(pipe, network_error, options.online_mode ? 2U : 0U)) {
        request_safe_mode(pipe, 3U);
        pipe_alive = false;
        network.stop(pipe);
        pipe.close();
        std::cerr << "network/IPC adapter: " << network_error
                  << "; game remains fail-closed\n";
      }
    }
  }

  network.stop(pipe);
  if (g_stop_requested.load(std::memory_order_relaxed) && pipe_alive) {
    request_safe_mode(pipe, 1U);
  }
  if (shutdown_requested) {
    const auto close_posted = request_game_close(game.id);
    status_event(options.status_events,
                 close_posted ? "game-close-requested" : "game-window-not-found");
    if (close_posted) {
      (void)WaitForSingleObject(game.process.get(), 15'000U);
    }
  }
  SetConsoleCtrlHandler(console_handler, FALSE);

  if (!GetExitCodeProcess(game.process.get(), &game_exit_code)) {
    error = "Cannot read king.exe exit code: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  if (g_stop_requested.load(std::memory_order_relaxed)) {
    if (game_exit_code == STILL_ACTIVE) {
      std::cout << "Sidecar stopped; king.exe did not accept WM_CLOSE yet\n";
    }
    status_event(options.status_events, "stopped");
  } else if (game_exit_code != 0U) {
    std::ostringstream detail;
    detail << "king.exe exited abnormally with code 0x" << std::hex
           << game_exit_code;
    error = detail.str();
    return false;
  }
  std::cout << "Validated LocalSample frames received: "
            << local_samples_received << '\n';
  return true;
}

} // namespace ht2mp::client
