#include "config.hpp"
#include "profile_editor.hpp"
#include "status.hpp"

#include "edition.hpp"
#include "staging.hpp"

#include "ht2mp/protocol/validation.hpp"
#include "ht2mp/protocol/vehicle_catalog.hpp"
#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <CommCtrl.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr wchar_t kWindowClass[] = L"HT2MP.NativeLauncher";
constexpr UINT kStatusMessage = WM_APP + 1U;
constexpr UINT kSidecarDoneMessage = WM_APP + 2U;
bool g_ui_smoke_mode{};

enum ControlId : int {
  server_list = 100,
  endpoint_edit,
  token_edit,
  player_edit,
  game_path_edit,
  browse_button,
  save_button,
  delete_button,
  vehicle_combo,
  paint_combo,
  play_button,
  disconnect_button,
  log_button,
  status_label,
};

class Handle final {
 public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() { reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& value) noexcept : value_(value.release()) {}
  Handle& operator=(Handle&& value) noexcept {
    if (this != &value) reset(value.release());
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] HANDLE release() noexcept {
    const auto value = value_;
    value_ = nullptr;
    return value;
  }
  void reset(HANDLE value = nullptr) noexcept {
    if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    value_ = value;
  }

 private:
  HANDLE value_{};
};

struct LaunchRequest final {
  HWND window{};
  fs::path source;
  fs::path executable_directory;
  fs::path log_path;
  std::string endpoint;
  std::string token;
  std::string player_name;
  std::string vehicle_key;
  std::uint16_t vehicle_selector{};
  std::uint8_t paint_variant{};
  std::wstring shutdown_event;
  Handle shutdown_keepalive;
};

struct LauncherState final {
  ht2mp::launcher::LauncherConfig config;
  fs::path config_path;
  fs::path log_path;
  HWND servers{};
  HWND endpoint{};
  HWND token{};
  HWND player{};
  HWND game_path{};
  HWND vehicle{};
  HWND paint{};
  HWND play{};
  HWND disconnect{};
  HWND status{};
  HFONT font{};
  Handle shutdown_event;
  ht2mp::launcher::RunState run_state;
  bool closing{};
};

std::wstring widen(const std::string_view value) {
  std::string error;
  auto result = ht2mp::windows::widen_utf8(value, error);
  return error.empty() ? result : L"";
}

std::string narrow(const std::wstring_view value) {
  std::string error;
  auto result = ht2mp::windows::narrow_utf8(value, error);
  return error.empty() ? result : std::string{};
}

std::wstring control_text(HWND control) {
  const auto length = GetWindowTextLengthW(control);
  std::wstring result(static_cast<std::size_t>(length) + 1U, L'\0');
  GetWindowTextW(control, result.data(), length + 1);
  result.resize(static_cast<std::size_t>(length));
  return result;
}

void set_status(HWND window, std::wstring text) {
  auto* state = reinterpret_cast<LauncherState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (state != nullptr && state->status != nullptr) {
    SetWindowTextW(state->status, text.c_str());
  }
}

void post_status(HWND window, const std::string_view text) {
  auto value = std::make_unique<std::wstring>(widen(text));
  if (!PostMessageW(window, kStatusMessage, 0U,
                    reinterpret_cast<LPARAM>(value.get()))) {
    return;
  }
  value.release();
}

std::wstring quote(const std::wstring_view value) {
  return ht2mp::windows::quote_command_line_argument(value);
}

std::wstring make_sidecar_command(const LaunchRequest& request) {
  std::vector<std::wstring> args{
      L"run", L"--edition", L"steam", L"--instance", L"online",
      L"--online-mode", L"--server", widen(request.endpoint)};
  if (!request.token.empty()) {
    args.push_back(L"--token");
    args.push_back(widen(request.token));
  }
  args.insert(args.end(), {
      L"--name", widen(request.player_name),
      L"--vehicle", widen(request.vehicle_key), L"--paint",
      std::to_wstring(request.paint_variant), L"--auto-enter-world",
      L"--status-events", L"--shutdown-event", request.shutdown_event});
  std::wstring result = quote((request.executable_directory /
                               L"ht2mp-client.exe").wstring());
  for (const auto& arg : args) {
    result.push_back(L' ');
    result += quote(arg);
  }
  return result;
}

bool process_named_king_running() {
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U));
  if (snapshot.get() == INVALID_HANDLE_VALUE) return true;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry)) return true;
  do {
    if (_wcsicmp(entry.szExeFile, L"king.exe") == 0) return true;
  } while (Process32NextW(snapshot.get(), &entry));
  return false;
}

fs::path find_template(const LaunchRequest& request, std::string& error) {
  std::error_code ec;
  const auto bundled = request.executable_directory / L"assets" / L"HT2MP.pl1";
  if (fs::is_regular_file(bundled, ec) && !ec) return bundled;
  std::vector<fs::path> candidates;
  for (fs::directory_iterator iterator(request.source, ec), end;
       !ec && iterator != end; iterator.increment(ec)) {
    if (iterator->is_regular_file(ec) && !ec &&
        _wcsicmp(iterator->path().extension().c_str(), L".pl1") == 0) {
      candidates.push_back(iterator->path());
    }
  }
  if (ec || candidates.empty()) {
    error = "В assets рядом с лаунчером нет HT2MP.pl1, а в папке игры нет профиля для частной копии";
    return {};
  }
  std::sort(candidates.begin(), candidates.end());
  return candidates.front();
}

void append_log(HANDLE log, const std::span<const char> bytes) {
  if (log == nullptr || log == INVALID_HANDLE_VALUE || bytes.empty()) return;
  DWORD written{};
  WriteFile(log, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

void parse_sidecar_line(HWND window, const std::string& line) {
  const auto event = ht2mp::launcher::parse_status_event(line);
  if (!event) return;
  using ht2mp::launcher::RunPhase;
  switch (event->phase) {
    case RunPhase::game_started:
      post_status(window, "Игра запущена, подключаю bridge…"); break;
    case RunPhase::bridge_ready:
      post_status(window, "Bridge готов, ожидаю загрузку машины…"); break;
    case RunPhase::appearance_ready:
      post_status(window, "Машина проверена, подключаюсь к серверу…"); break;
    case RunPhase::connected:
      post_status(window, "Подключено к серверу"); break;
    case RunPhase::reconnecting:
      post_status(window, "Связь потеряна, переподключение: " + event->detail); break;
    case RunPhase::rejected:
      post_status(window, "Сервер отказал: " + event->detail); break;
    case RunPhase::shutting_down:
      post_status(window, "Отключаюсь и закрываю игру…"); break;
    case RunPhase::failed:
      post_status(window, event->detail.empty()
                              ? "Загруженная машина или раскраска не совпала с выбором"
                              : "Ошибка: " + event->detail); break;
    case RunPhase::idle:
    case RunPhase::preparing:
    case RunPhase::stopped:
      break;
  }
}

void monitor_sidecar(HWND window, HANDLE process, HANDLE output_read,
                     fs::path log_path) {
  Handle process_handle(process);
  Handle read_handle(output_read);
  std::error_code ec;
  fs::create_directories(log_path.parent_path(), ec);
  Handle log(CreateFileW(log_path.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  std::array<char, 1024> buffer{};
  std::string pending;
  for (;;) {
    DWORD received{};
    if (!ReadFile(read_handle.get(), buffer.data(),
                  static_cast<DWORD>(buffer.size()), &received, nullptr) ||
        received == 0U) break;
    append_log(log.get(), std::span(buffer).first(received));
    pending.append(buffer.data(), received);
    for (;;) {
      const auto newline = pending.find('\n');
      if (newline == std::string::npos) break;
      auto line = pending.substr(0U, newline);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      parse_sidecar_line(window, line);
      pending.erase(0U, newline + 1U);
    }
  }
  WaitForSingleObject(process_handle.get(), INFINITE);
  DWORD exit_code{};
  GetExitCodeProcess(process_handle.get(), &exit_code);
  PostMessageW(window, kSidecarDoneMessage, static_cast<WPARAM>(exit_code), 0U);
}

bool prepare_runtime(const LaunchRequest& request, fs::path& runtime,
                     std::string& error) {
  const auto edition = ht2mp::client::find_edition("steam");
  if (!edition) {
    error = "Internal Steam profile is unavailable";
    return false;
  }
  runtime = ht2mp::client::staged_game_directory(*edition, error, "online");
  if (!error.empty()) return false;
  ht2mp::client::StageValidation validation;
  auto stage_is_current = ht2mp::client::validate_staged_game(
      *edition, runtime, validation, error, "online");
  if (stage_is_current) {
    std::string identity_error;
    stage_is_current = ht2mp::windows::same_existing_file(
        validation.source, request.source, identity_error);
  }
  if (!stage_is_current) {
    post_status(request.window, "Подготавливаю изолированную Steam-копию…");
    ht2mp::client::StageResult stage;
    if (!ht2mp::client::stage_game(*edition, request.source, stage, error,
                                   "online")) {
      return false;
    }
    runtime = stage.destination;
  }
  auto template_profile = find_template(request, error);
  if (template_profile.empty()) return false;
  ht2mp::launcher::ProfileEditResult profile;
  if (!ht2mp::launcher::prepare_online_profile(
          template_profile, runtime, request.vehicle_selector,
          request.paint_variant, profile, error)) {
    return false;
  }
  if (!ht2mp::launcher::write_last_player(runtime / L"TRUCK.INI", error)) {
    return false;
  }
  return true;
}

void launch_worker(LaunchRequest request) {
  if (WaitForSingleObject(request.shutdown_keepalive.get(), 0U) ==
      WAIT_OBJECT_0) {
    PostMessageW(request.window, kSidecarDoneMessage, 0U, 0U);
    return;
  }
  if (process_named_king_running()) {
    post_status(request.window,
                "Закройте уже запущенный king.exe перед изменением профиля");
    PostMessageW(request.window, kSidecarDoneMessage, 4U, 0U);
    return;
  }
  fs::path runtime;
  std::string error;
  if (!prepare_runtime(request, runtime, error)) {
    post_status(request.window, "Ошибка подготовки: " + error);
    PostMessageW(request.window, kSidecarDoneMessage, 4U, 0U);
    return;
  }
  if (WaitForSingleObject(request.shutdown_keepalive.get(), 0U) ==
      WAIT_OBJECT_0) {
    PostMessageW(request.window, kSidecarDoneMessage, 0U, 0U);
    return;
  }
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE read_raw{};
  HANDLE write_raw{};
  if (!CreatePipe(&read_raw, &write_raw, &attributes, 0U)) {
    post_status(request.window, "Не удалось создать канал состояния sidecar");
    PostMessageW(request.window, kSidecarDoneMessage, 4U, 0U);
    return;
  }
  Handle read(read_raw);
  Handle write(write_raw);
  SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0U);
  auto command = make_sidecar_command(request);
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write.get();
  startup.hStdError = write.get();
  PROCESS_INFORMATION process{};
  const auto sidecar = request.executable_directory / L"ht2mp-client.exe";
  if (!CreateProcessW(sidecar.c_str(), writable.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr,
                      request.executable_directory.c_str(), &startup,
                      &process)) {
    post_status(request.window, "Не удалось запустить ht2mp-client.exe");
    PostMessageW(request.window, kSidecarDoneMessage, 4U, 0U);
    return;
  }
  CloseHandle(process.hThread);
  write.reset();
  post_status(request.window, "Sidecar запущен…");
  monitor_sidecar(request.window, process.hProcess, read.release(),
                  std::move(request.log_path));
}

HWND add_control(HWND parent, const wchar_t* type, const wchar_t* text,
                 DWORD style, int x, int y, int width, int height, int id,
                 HFONT font) {
  const auto control = CreateWindowExW(
      std::wcscmp(type, WC_EDITW) == 0 ? WS_EX_CLIENTEDGE : 0U, type, text,
      WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  return control;
}

void refresh_servers(LauncherState& state) {
  SendMessageW(state.servers, LB_RESETCONTENT, 0U, 0U);
  for (const auto& server : state.config.servers) {
    const auto label = widen(server.endpoint +
                             (server.token.empty() ? " (public)" : ""));
    SendMessageW(state.servers, LB_ADDSTRING, 0U,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
}

void refresh_paints(LauncherState& state, const std::uint8_t requested_paint) {
  const auto catalog = ht2mp::protocol::steam_vehicle_catalog();
  const auto vehicle_index =
      SendMessageW(state.vehicle, CB_GETCURSEL, 0U, 0U);
  std::uint8_t paint_count{};
  if (vehicle_index >= 0 &&
      static_cast<std::size_t>(vehicle_index) < catalog.size()) {
    paint_count =
        catalog[static_cast<std::size_t>(vehicle_index)].steam_native_paint_count;
  }
  SendMessageW(state.paint, CB_RESETCONTENT, 0U, 0U);
  for (std::uint8_t paint = 0U; paint < paint_count; ++paint) {
    const auto label = L"Раскраска " + std::to_wstring(paint + 1U);
    SendMessageW(state.paint, CB_ADDSTRING, 0U,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  if (paint_count != 0U) {
    const auto selection = std::min<std::uint8_t>(
        requested_paint, static_cast<std::uint8_t>(paint_count - 1U));
    SendMessageW(state.paint, CB_SETCURSEL, selection, 0U);
  }
}

void fill_from_config(LauncherState& state) {
  SetWindowTextW(state.game_path, state.config.steam_directory.c_str());
  SetWindowTextW(state.player, widen(state.config.player_name).c_str());
  const auto catalog = ht2mp::protocol::steam_vehicle_catalog();
  LRESULT vehicle_selection = 0;
  for (std::size_t index = 0U; index < catalog.size(); ++index) {
    if (catalog[index].key == state.config.vehicle_key) {
      vehicle_selection = static_cast<LRESULT>(index);
      break;
    }
  }
  SendMessageW(state.vehicle, CB_SETCURSEL, vehicle_selection, 0U);
  refresh_paints(state, state.config.paint_variant);
  refresh_servers(state);
  if (!state.config.servers.empty()) {
    SendMessageW(state.servers, LB_SETCURSEL, 0U, 0U);
    SetWindowTextW(state.endpoint,
                   widen(state.config.servers.front().endpoint).c_str());
    SetWindowTextW(state.token, widen(state.config.servers.front().token).c_str());
  }
}

fs::path detect_steam_game() {
  std::vector<fs::path> roots;
  const std::array<std::pair<HKEY, const wchar_t*>, 2> keys{{
      {HKEY_CURRENT_USER, L"Software\\Valve\\Steam"},
      {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam"}}};
  for (const auto& [hive, key] : keys) {
    std::array<wchar_t, 32768> value{};
    DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    auto result = RegGetValueW(hive, key, L"InstallPath", RRF_RT_REG_SZ,
                               nullptr, value.data(), &size);
    if (result != ERROR_SUCCESS) {
      value.fill(L'\0');
      size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
      result = RegGetValueW(hive, key, L"SteamPath", RRF_RT_REG_SZ, nullptr,
                            value.data(), &size);
    }
    if (result == ERROR_SUCCESS) {
      roots.emplace_back(value.data());
    }
  }
  roots.emplace_back(L"C:\\Program Files (x86)\\Steam");
  for (const auto& root : roots) {
    const auto candidate = root / L"steamapps" / L"common" /
                           L"Hard Truck 2 King of the Road";
    std::error_code ec;
    if (fs::is_regular_file(candidate / L"king.exe", ec) && !ec) return candidate;
  }
  return {};
}

void browse_game(HWND window, LauncherState& state) {
  BROWSEINFOW browse{};
  browse.hwndOwner = window;
  browse.lpszTitle = L"Выберите папку Steam-версии с king.exe";
  browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
  if (item == nullptr) return;
  std::array<wchar_t, MAX_PATH> path{};
  if (SHGetPathFromIDListW(item, path.data())) {
    state.config.steam_directory = path.data();
    SetWindowTextW(state.game_path, path.data());
  }
  CoTaskMemFree(item);
}

bool read_identity(HWND window, LauncherState& state, LaunchRequest& request,
                   std::string& error) {
  request.source = control_text(state.game_path);
  request.endpoint = narrow(control_text(state.endpoint));
  request.token = narrow(control_text(state.token));
  request.player_name = narrow(control_text(state.player));
  if (!ht2mp::launcher::normalize_endpoint(request.endpoint, request.endpoint,
                                            error)) return false;
  if (!ht2mp::launcher::validate_token(request.token)) {
    error = "Токен должен быть пустым либо содержать ровно 32 шестнадцатеричных символа";
    return false;
  }
  if (request.player_name.empty() || request.player_name.size() > 32U ||
      !ht2mp::protocol::is_valid_utf8(request.player_name)) {
    error = "Имя игрока должно быть корректной строкой UTF-8 до 32 байт";
    return false;
  }
  const auto vehicle_index = SendMessageW(state.vehicle, CB_GETCURSEL, 0U, 0U);
  const auto catalog = ht2mp::protocol::steam_vehicle_catalog();
  if (vehicle_index < 0 || static_cast<std::size_t>(vehicle_index) >= catalog.size()) {
    error = "Выберите машину";
    return false;
  }
  const auto& vehicle = catalog[static_cast<std::size_t>(vehicle_index)];
  request.vehicle_key = std::string(vehicle.key);
  request.vehicle_selector = vehicle.selector;
  const auto paint = SendMessageW(state.paint, CB_GETCURSEL, 0U, 0U);
  if (paint < 0 ||
      paint >= static_cast<LRESULT>(vehicle.steam_native_paint_count)) {
    error = "Выберите штатную раскраску";
    return false;
  }
  request.paint_variant = static_cast<std::uint8_t>(paint);
  const auto limits = ht2mp::protocol::limits_for_profile("steam-8138acee");
  const ht2mp::protocol::ClientHello identity_probe{
      "steam-8138acee", {}, request.player_name, 1U,
      request.vehicle_selector, request.paint_variant};
  if (!limits) {
    error = "Внутренняя таблица Steam-профиля недоступна";
    return false;
  }
  if (const auto valid =
          ht2mp::protocol::validate(identity_probe, *limits);
      !valid) {
    error = "Некорректное имя или выбор машины: " + valid.detail;
    return false;
  }
  std::string executable_error;
  request.executable_directory =
      ht2mp::windows::executable_path(executable_error).parent_path();
  if (!executable_error.empty()) {
    error = executable_error;
    return false;
  }
  request.log_path = state.log_path;
  request.window = window;
  return true;
}

bool persist_controls(LauncherState& state, std::string& error) {
  state.config.steam_directory = control_text(state.game_path);
  const auto name = narrow(control_text(state.player));
  if (!name.empty()) state.config.player_name = name;
  const auto index = SendMessageW(state.vehicle, CB_GETCURSEL, 0U, 0U);
  const auto catalog = ht2mp::protocol::steam_vehicle_catalog();
  if (index >= 0 && static_cast<std::size_t>(index) < catalog.size()) {
    state.config.vehicle_key = catalog[static_cast<std::size_t>(index)].key;
  }
  const auto paint = SendMessageW(state.paint, CB_GETCURSEL, 0U, 0U);
  if (paint >= 0 && paint <= 3) state.config.paint_variant = static_cast<std::uint8_t>(paint);
  return ht2mp::launcher::save_config(state.config_path, state.config, error);
}

void begin_launch(HWND window, LauncherState& state) {
  if (state.run_state.active()) {
    set_status(window, L"Запуск уже выполняется");
    return;
  }
  LaunchRequest request;
  std::string error;
  if (!read_identity(window, state, request, error)) {
    set_status(window, L"Ошибка: " + widen(error));
    return;
  }
  state.config.steam_directory = request.source;
  state.config.player_name = request.player_name;
  state.config.vehicle_key = request.vehicle_key;
  state.config.paint_variant = request.paint_variant;
  ht2mp::launcher::upsert_server(state.config,
                                  {request.endpoint, request.token});
  if (!ht2mp::launcher::save_config(state.config_path, state.config, error)) {
    set_status(window, L"Ошибка настроек: " + widen(error));
    return;
  }
  refresh_servers(state);
  request.shutdown_event = L"Local\\HT2MP-Launcher-" +
                           std::to_wstring(GetCurrentProcessId()) + L'-' +
                           std::to_wstring(GetTickCount64());
  state.shutdown_event.reset(CreateEventW(nullptr, TRUE, FALSE,
                                          request.shutdown_event.c_str()));
  if (state.shutdown_event.get() == nullptr) {
    set_status(window, L"Не удалось создать событие отключения");
    return;
  }
  request.shutdown_keepalive.reset(OpenEventW(
      SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, request.shutdown_event.c_str()));
  if (request.shutdown_keepalive.get() == nullptr) {
    state.shutdown_event.reset();
    set_status(window, L"Не удалось закрепить событие отключения");
    return;
  }
  if (!state.run_state.begin()) {
    set_status(window, L"Запуск уже выполняется");
    return;
  }
  EnableWindow(state.play, FALSE);
  EnableWindow(state.disconnect, TRUE);
  set_status(window, L"Проверяю Steam-сборку и профиль…");
  std::thread(launch_worker, std::move(request)).detach();
}

void select_server(LauncherState& state) {
  const auto index = SendMessageW(state.servers, LB_GETCURSEL, 0U, 0U);
  if (index < 0 || static_cast<std::size_t>(index) >= state.config.servers.size()) return;
  const auto& server = state.config.servers[static_cast<std::size_t>(index)];
  SetWindowTextW(state.endpoint, widen(server.endpoint).c_str());
  SetWindowTextW(state.token, widen(server.token).c_str());
}

void open_log(HWND window, LauncherState& state) {
  std::error_code ec;
  fs::create_directories(state.log_path.parent_path(), ec);
  if (ec) {
    set_status(window, L"Не удалось создать каталог журнала");
    return;
  }
  Handle file(CreateFileW(state.log_path.c_str(), FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    set_status(window, L"Не удалось открыть файл журнала");
    return;
  }
  file.reset();
  const auto result = ShellExecuteW(window, L"open", state.log_path.c_str(),
                                    nullptr, state.log_path.parent_path().c_str(),
                                    SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    set_status(window, L"Windows не смог открыть журнал");
  }
}

LRESULT on_create(HWND window) {
  auto state = std::make_unique<LauncherState>();
  state->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
  auto label = [&](const wchar_t* text, int x, int y, int width) {
    add_control(window, WC_STATICW, text, 0U, x, y, width, 22, 0, state->font);
  };
  label(L"Сохранённые серверы", 18, 16, 250);
  state->servers = add_control(window, WC_LISTBOXW, L"",
      LBS_NOTIFY | WS_VSCROLL, 18, 42, 255, 165, server_list, state->font);
  add_control(window, WC_BUTTONW, L"Сохранить сервер", 0U, 18, 214, 155, 30,
              save_button, state->font);
  add_control(window, WC_BUTTONW, L"Удалить", 0U, 180, 214, 93, 30,
              delete_button, state->font);

  label(L"Адрес сервера (порт по умолчанию 28020)", 298, 16, 390);
  state->endpoint = add_control(window, WC_EDITW, L"127.0.0.1:28020",
      ES_AUTOHSCROLL, 298, 42, 430, 25, endpoint_edit, state->font);
  label(L"Токен (необязательно для публичного сервера)", 298, 75, 410);
  state->token = add_control(window, WC_EDITW, L"", ES_AUTOHSCROLL,
      298, 99, 430, 25, token_edit, state->font);
  label(L"Имя игрока", 298, 132, 200);
  state->player = add_control(window, WC_EDITW, L"HT2MP", ES_AUTOHSCROLL,
      298, 156, 430, 25, player_edit, state->font);

  label(L"Папка точной Steam-версии", 18, 262, 300);
  state->game_path = add_control(window, WC_EDITW, L"", ES_AUTOHSCROLL,
      18, 287, 610, 25, game_path_edit, state->font);
  add_control(window, WC_BUTTONW, L"Обзор…", 0U, 638, 285, 90, 29,
              browse_button, state->font);

  label(L"Машина (тягачи исключены)", 18, 330, 300);
  state->vehicle = add_control(window, WC_COMBOBOXW, L"",
      CBS_DROPDOWNLIST | WS_VSCROLL, 18, 355, 350, 300, vehicle_combo,
      state->font);
  for (const auto& vehicle : ht2mp::protocol::steam_vehicle_catalog()) {
    const auto display = widen(vehicle.display_name);
    SendMessageW(state->vehicle, CB_ADDSTRING, 0U,
                 reinterpret_cast<LPARAM>(display.c_str()));
  }
  label(L"Штатная раскраска", 395, 330, 200);
  state->paint = add_control(window, WC_COMBOBOXW, L"",
      CBS_DROPDOWNLIST, 395, 355, 333, 180, paint_combo, state->font);

  state->play = add_control(window, WC_BUTTONW, L"Играть", BS_DEFPUSHBUTTON,
      18, 414, 220, 38, play_button, state->font);
  state->disconnect = add_control(window, WC_BUTTONW,
      L"Отключиться и закрыть игру", 0U, 248, 414, 285, 38,
      disconnect_button, state->font);
  EnableWindow(state->disconnect, FALSE);
  add_control(window, WC_BUTTONW, L"Открыть журнал", 0U, 543, 414, 185, 38,
              log_button, state->font);
  label(L"Состояние", 18, 470, 100);
  state->status = add_control(window, WC_STATICW, L"Готово",
      SS_LEFT, 18, 494, 710, 52, status_label, state->font);

  std::string error;
  if (g_ui_smoke_mode) {
    std::array<wchar_t, 32768> temporary{};
    const auto length = GetTempPathW(static_cast<DWORD>(temporary.size()),
                                     temporary.data());
    if (length == 0U || length >= temporary.size()) return -1;
    const auto root = fs::path(temporary.data()) /
                      (L"HT2MP-launcher-ui-" +
                       std::to_wstring(GetCurrentProcessId()));
    state->config_path = root / L"launcher.ini";
    state->log_path = root / L"launcher.log";
  } else {
    state->config_path = ht2mp::launcher::launcher_config_path(error);
    state->log_path = ht2mp::launcher::launcher_log_path(error);
  }
  std::string config_error;
  if (!ht2mp::launcher::load_config(state->config_path, state->config,
                                     config_error)) {
    SetWindowTextW(state->status,
                   (L"launcher.ini повреждён; используются безопасные настройки: " +
                    widen(config_error)).c_str());
    state->config = {};
  }
  if (state->config.steam_directory.empty()) {
    state->config.steam_directory = detect_steam_game();
  }
  fill_from_config(*state);
  if (state->config.steam_directory.empty()) {
    SetWindowTextW(
        state->status,
        L"Steam-версия не найдена автоматически — выберите папку кнопкой «Обзор…»");
  }
  SetWindowLongPtrW(window, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(state.release()));
  return 0;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  auto* state = reinterpret_cast<LauncherState*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_CREATE:
      return on_create(window);
    case WM_COMMAND:
      if (state == nullptr) break;
      switch (LOWORD(wparam)) {
        case server_list:
          if (HIWORD(wparam) == LBN_SELCHANGE) select_server(*state);
          break;
        case vehicle_combo:
          if (HIWORD(wparam) == CBN_SELCHANGE) {
            refresh_paints(*state, 0U);
          }
          break;
        case browse_button:
          browse_game(window, *state);
          break;
        case save_button: {
          std::string endpoint = narrow(control_text(state->endpoint));
          std::string normalized;
          std::string error;
          const auto token = narrow(control_text(state->token));
          if (!ht2mp::launcher::normalize_endpoint(endpoint, normalized, error)) {
            set_status(window, L"Неверный адрес: " + widen(error));
            break;
          }
          if (!ht2mp::launcher::validate_token(token)) {
            set_status(window,
                       L"Токен должен быть пустым либо содержать ровно 32 hex-символа");
            break;
          }
          ht2mp::launcher::upsert_server(state->config, {normalized, token});
          if (!persist_controls(*state, error)) {
            set_status(window, L"Не удалось сохранить сервер: " + widen(error));
            break;
          }
          refresh_servers(*state);
          set_status(window, L"Сервер сохранён");
          break;
        }
        case delete_button: {
          const auto index = SendMessageW(state->servers, LB_GETCURSEL, 0U, 0U);
          if (index >= 0 && static_cast<std::size_t>(index) < state->config.servers.size()) {
            state->config.servers.erase(state->config.servers.begin() + index);
            std::string error;
            if (!persist_controls(*state, error)) {
              set_status(window, L"Не удалось сохранить настройки: " + widen(error));
              break;
            }
            refresh_servers(*state);
          }
          break;
        }
        case play_button:
          begin_launch(window, *state);
          break;
        case disconnect_button:
          if (state->shutdown_event.get() != nullptr) {
            (void)state->run_state.request_shutdown();
            SetEvent(state->shutdown_event.get());
            set_status(window, L"Отключаюсь и запрашиваю штатное закрытие игры…");
            EnableWindow(state->disconnect, FALSE);
          }
          break;
        case log_button:
          open_log(window, *state);
          break;
      }
      return 0;
    case kStatusMessage: {
      std::unique_ptr<std::wstring> text(
          reinterpret_cast<std::wstring*>(lparam));
      if (text) set_status(window, *text);
      return 0;
    }
    case kSidecarDoneMessage:
      if (state != nullptr) {
        state->run_state.finish(static_cast<std::uint32_t>(wparam));
        state->shutdown_event.reset();
        EnableWindow(state->play, TRUE);
        EnableWindow(state->disconnect, FALSE);
        if (!state->closing && wparam == 0U) set_status(window, L"Игра закрыта");
        else if (!state->closing && wparam != 0U)
          set_status(window, L"Sidecar завершился с ошибкой; откройте журнал");
      }
      return 0;
    case WM_CLOSE:
      if (state != nullptr) {
        state->closing = true;
        std::string ignored;
        (void)persist_controls(*state, ignored);
        if (state->shutdown_event.get() != nullptr) {
          (void)state->run_state.request_shutdown();
          SetEvent(state->shutdown_event.get());
        }
      }
      DestroyWindow(window);
      return 0;
    case WM_DESTROY:
      if (state != nullptr) {
        if (state->shutdown_event.get() != nullptr) SetEvent(state->shutdown_event.get());
        if (state->font != nullptr) DeleteObject(state->font);
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0U);
      }
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line,
                    int show) {
  g_ui_smoke_mode = command_line != nullptr &&
                     std::wcscmp(command_line, L"--ui-smoke") == 0;
  const wchar_t* mutex_name = g_ui_smoke_mode
      ? L"Local\\HT2MP-Native-Launcher-UI-Smoke"
      : L"Local\\HT2MP-Native-Launcher";
  Handle single_instance(CreateMutexW(nullptr, FALSE, mutex_name));
  if (single_instance.get() == nullptr) return 1;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"Лаунчер HT2MP уже запущен.", L"HT2MP",
                MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WNDCLASSEXW type{};
  type.cbSize = sizeof(type);
  type.lpfnWndProc = window_proc;
  type.hInstance = instance;
  type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  type.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  type.lpszClassName = kWindowClass;
  if (!RegisterClassExW(&type)) return 1;
  const auto window = CreateWindowExW(
      0U, kWindowClass, L"HT2MP — Онлайн-режим",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 765, 600, nullptr, nullptr, instance,
      nullptr);
  if (window == nullptr) return 1;
  ShowWindow(window, show);
  UpdateWindow(window);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (com == S_OK || com == S_FALSE) CoUninitialize();
  return static_cast<int>(message.wParam);
}
