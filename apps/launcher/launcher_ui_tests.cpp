#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct WindowSearch final {
  DWORD process_id{};
  HWND window{};
};

BOOL CALLBACK find_process_window(HWND window, LPARAM parameter) {
  auto& search = *reinterpret_cast<WindowSearch*>(parameter);
  DWORD owner{};
  GetWindowThreadProcessId(window, &owner);
  std::array<wchar_t, 64> class_name{};
  GetClassNameW(window, class_name.data(), static_cast<int>(class_name.size()));
  if (owner == search.process_id &&
      std::wcscmp(class_name.data(), L"HT2MP.NativeLauncher") == 0) {
    search.window = window;
    return FALSE;
  }
  return TRUE;
}

void check(const bool condition, const char* expression, int line,
           int& failures) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
  }
}

#define CHECK(value) check((value), #value, __LINE__, failures)

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::cerr << "expected absolute launcher path\n";
    return 2;
  }
  const std::filesystem::path launcher(argv[1]);
  std::wstring command = L'"' + launcher.wstring() + L"\" --ui-smoke";
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(launcher.c_str(), writable.data(), nullptr, nullptr,
                      FALSE, 0U, nullptr, launcher.parent_path().c_str(),
                      &startup, &process)) {
    std::cerr << "cannot start launcher UI smoke target\n";
    return 2;
  }
  CloseHandle(process.hThread);
  (void)WaitForInputIdle(process.hProcess, 5'000U);
  WindowSearch search{process.dwProcessId, nullptr};
  const auto deadline = GetTickCount64() + 5'000U;
  do {
    EnumWindows(find_process_window, reinterpret_cast<LPARAM>(&search));
    if (search.window != nullptr) break;
    Sleep(20U);
  } while (GetTickCount64() < deadline);

  int failures{};
  CHECK(search.window != nullptr);
  if (search.window != nullptr) {
    constexpr std::array required_controls{
        100, 101, 102, 103, 104, 105, 106,
        107, 108, 109, 110, 111, 112, 113};
    for (const auto id : required_controls) {
      CHECK(GetDlgItem(search.window, id) != nullptr);
    }
    const auto vehicle = GetDlgItem(search.window, 108);
    const auto paint = GetDlgItem(search.window, 109);
    CHECK(SendMessageW(vehicle, CB_GETCOUNT, 0U, 0U) == 26);
    CHECK(SendMessageW(paint, CB_GETCOUNT, 0U, 0U) == 4);
    SendMessageW(vehicle, CB_SETCURSEL, 1U, 0U);
    SendMessageW(search.window, WM_COMMAND, MAKEWPARAM(108, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(vehicle));
    CHECK(SendMessageW(paint, CB_GETCOUNT, 0U, 0U) == 1);
    SendMessageW(vehicle, CB_SETCURSEL, 2U, 0U);
    SendMessageW(search.window, WM_COMMAND, MAKEWPARAM(108, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(vehicle));
    CHECK(SendMessageW(paint, CB_GETCOUNT, 0U, 0U) == 2);
    SendMessageW(vehicle, CB_SETCURSEL, 5U, 0U);
    SendMessageW(search.window, WM_COMMAND, MAKEWPARAM(108, CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(vehicle));
    CHECK(SendMessageW(paint, CB_GETCOUNT, 0U, 0U) == 4);
    CHECK(IsWindowEnabled(GetDlgItem(search.window, 110)) != FALSE);
    CHECK(IsWindowEnabled(GetDlgItem(search.window, 111)) == FALSE);
    std::array<wchar_t, 128> title{};
    GetWindowTextW(search.window, title.data(), static_cast<int>(title.size()));
    CHECK(std::wstring_view(title.data()).find(L"HT2MP") !=
          std::wstring_view::npos);
    PostMessageW(search.window, WM_CLOSE, 0U, 0U);
  }
  if (WaitForSingleObject(process.hProcess, 5'000U) != WAIT_OBJECT_0) {
    ++failures;
    TerminateProcess(process.hProcess, 125U);
    (void)WaitForSingleObject(process.hProcess, 5'000U);
  }
  CloseHandle(process.hProcess);

  std::array<wchar_t, 32768> temporary{};
  if (GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data()) != 0U) {
    std::error_code ignored;
    std::filesystem::remove_all(
        std::filesystem::path(temporary.data()) /
            (L"HT2MP-launcher-ui-" + std::to_wstring(process.dwProcessId)),
        ignored);
  }
  if (failures == 0) std::cout << "launcher Win32 UI smoke passed\n";
  return failures == 0 ? 0 : 1;
}
