#include "injector.hpp"

#include "ht2mp/game/profile.hpp"
#include "ht2mp/ipc/bootstrap.hpp"
#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>
#include <vector>

namespace ht2mp::injector {
namespace fs = std::filesystem;
namespace {

class Handle final {
public:
  Handle() noexcept = default;
  explicit Handle(HANDLE value) noexcept : value_(value) {}
  ~Handle() {
    if (valid()) {
      CloseHandle(value_);
    }
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      if (valid()) {
        CloseHandle(value_);
      }
      value_ = other.release();
    }
    return *this;
  }
  [[nodiscard]] bool valid() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
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

class RemoteAllocation final {
public:
  RemoteAllocation(HANDLE process, const std::size_t size)
      : process_(process), address_(VirtualAllocEx(process, nullptr, size,
                                                   MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_READWRITE)) {}
  ~RemoteAllocation() {
    if (address_ != nullptr) {
      VirtualFreeEx(process_, address_, 0U, MEM_RELEASE);
    }
  }
  RemoteAllocation(const RemoteAllocation&) = delete;
  RemoteAllocation& operator=(const RemoteAllocation&) = delete;
  [[nodiscard]] void* get() const noexcept { return address_; }
  void release() noexcept { address_ = nullptr; }

private:
  HANDLE process_{};
  void* address_{};
};

bool write_remote(HANDLE process, void* destination, const void* source,
                  const std::size_t size, std::string& error) {
  SIZE_T written{};
  if (!WriteProcessMemory(process, destination, source, size, &written) ||
      written != size) {
    error = "WriteProcessMemory failed: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  return true;
}

bool wait_thread(HANDLE thread, const DWORD timeout_ms, DWORD& result,
                 std::string& error) {
  const auto wait = WaitForSingleObject(thread, timeout_ms);
  if (wait == WAIT_TIMEOUT) {
    error = "Remote call timed out";
    return false;
  }
  if (wait != WAIT_OBJECT_0) {
    error = "Cannot wait for remote call: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  if (!GetExitCodeThread(thread, &result)) {
    error = "Cannot obtain remote call result: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  return true;
}

bool query_process_path(HANDLE process, fs::path& path, std::string& error) {
  std::wstring buffer(32768U, L'\0');
  DWORD size = static_cast<DWORD>(buffer.size());
  if (!QueryFullProcessImageNameW(process, 0U, buffer.data(), &size)) {
    error = "Cannot query target process path: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  buffer.resize(size);
  path = fs::path(buffer);
  return true;
}

bool target_is_i386(HANDLE process, std::string& error) {
  using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
  const auto kernel = GetModuleHandleW(L"kernel32.dll");
  const auto is_wow64_process2 = reinterpret_cast<IsWow64Process2Fn>(
      GetProcAddress(kernel, "IsWow64Process2"));
  if (is_wow64_process2 != nullptr) {
    USHORT process_machine{};
    USHORT native_machine{};
    if (!is_wow64_process2(process, &process_machine, &native_machine)) {
      error = "IsWow64Process2 failed: " +
              ht2mp::windows::win32_error(GetLastError());
      return false;
    }
    const bool i386 = process_machine == IMAGE_FILE_MACHINE_I386 ||
                      (process_machine == IMAGE_FILE_MACHINE_UNKNOWN &&
                       native_machine == IMAGE_FILE_MACHINE_I386);
    if (!i386) {
      error = "Target process is not x86";
    }
    return i386;
  }

  BOOL self_wow64{};
  BOOL target_wow64{};
  if (!IsWow64Process(GetCurrentProcess(), &self_wow64) ||
      !IsWow64Process(process, &target_wow64)) {
    error = "IsWow64Process failed: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  if (self_wow64 && !target_wow64) {
    error = "Target process is native 64-bit";
    return false;
  }
  return true;
}

std::optional<std::uintptr_t> remote_module_base(
    const DWORD process_id, const std::wstring_view module_name,
    const fs::path* exact_path, std::string& error) {
  // Toolhelp documents ERROR_BAD_LENGTH as a transient race while a process is
  // loading modules.  The sidecar deliberately waits for the protected game
  // DLLs first, but Steam may still be loading unrelated modules when the
  // short-lived injector resolves kernel32/bridge.  Retry the whole snapshot;
  // never turn this into a name-only or unchecked-path fallback.
  for (unsigned attempt = 0U; attempt < 8U; ++attempt) {
    Handle snapshot(CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id));
    if (!snapshot.valid()) {
      const DWORD code = GetLastError();
      if (code == ERROR_BAD_LENGTH) {
        Sleep(10U);
        continue;
      }
      error = "Cannot enumerate target modules: " +
              ht2mp::windows::win32_error(code);
      return std::nullopt;
    }

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.get(), &module)) {
      const DWORD code = GetLastError();
      if (code == ERROR_BAD_LENGTH) {
        Sleep(10U);
        continue;
      }
      error = "Cannot read target module list: " +
              ht2mp::windows::win32_error(code);
      return std::nullopt;
    }

    do {
      bool match =
          _wcsicmp(module.szModule, std::wstring(module_name).c_str()) == 0;
      if (match && exact_path != nullptr) {
        std::string identity_error;
        match = ht2mp::windows::same_existing_file(
            module.szExePath, *exact_path, identity_error);
        if (!match && !identity_error.empty()) {
          continue;
        }
      }
      if (match) {
        return reinterpret_cast<std::uintptr_t>(module.modBaseAddr);
      }
    } while (Module32NextW(snapshot.get(), &module));

    const DWORD code = GetLastError();
    if (code == ERROR_BAD_LENGTH) {
      Sleep(10U);
      continue;
    }
    if (code != ERROR_NO_MORE_FILES) {
      error = "Cannot finish reading target module list: " +
              ht2mp::windows::win32_error(code);
      return std::nullopt;
    }
    error = "Required module is not loaded in the target process";
    return std::nullopt;
  }

  error = "Cannot obtain a stable target module snapshot after retries";
  return std::nullopt;
}

bool resolve_remote_load_library(const DWORD process_id,
                                 LPTHREAD_START_ROUTINE& function,
                                 std::string& error) {
  const auto local_kernel = GetModuleHandleW(L"kernel32.dll");
  const auto local_load_library = GetProcAddress(local_kernel, "LoadLibraryW");
  if (local_kernel == nullptr || local_load_library == nullptr) {
    error = "Cannot resolve local LoadLibraryW";
    return false;
  }
  const auto remote_kernel = remote_module_base(process_id, L"kernel32.dll", nullptr,
                                                 error);
  if (!remote_kernel) {
    return false;
  }
  const auto offset = reinterpret_cast<std::uintptr_t>(local_load_library) -
                      reinterpret_cast<std::uintptr_t>(local_kernel);
  function = reinterpret_cast<LPTHREAD_START_ROUTINE>(*remote_kernel + offset);
  return true;
}

bool load_file(const fs::path& path, std::vector<std::byte>& bytes,
               std::string& error) {
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr));
  if (!file.valid()) {
    error = "Cannot open bridge DLL: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
      size.QuadPart > 64LL * 1024LL * 1024LL) {
    error = "Bridge DLL has an invalid file size";
    return false;
  }
  bytes.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t cursor{};
  while (cursor < bytes.size()) {
    DWORD read{};
    const auto amount = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - cursor, std::numeric_limits<DWORD>::max()));
    if (!ReadFile(file.get(), bytes.data() + cursor, amount, &read, nullptr) ||
        read == 0U) {
      error = "Cannot read bridge DLL: " +
              ht2mp::windows::win32_error(GetLastError());
      return false;
    }
    cursor += read;
  }
  return true;
}

template <typename T>
const T* object_at(const std::vector<std::byte>& bytes, const std::size_t offset) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
    return nullptr;
  }
  return reinterpret_cast<const T*>(bytes.data() + offset);
}

bool resolve_export_rva(const fs::path& path, const char* export_name,
                        std::uint32_t& function_rva, std::string& error) {
  std::vector<std::byte> bytes;
  if (!load_file(path, bytes, error)) {
    return false;
  }
  const auto dos = object_at<IMAGE_DOS_HEADER>(bytes, 0U);
  if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
    error = "Bridge DLL has an invalid DOS header";
    return false;
  }
  const auto nt_offset = static_cast<std::size_t>(dos->e_lfanew);
  const auto nt = object_at<IMAGE_NT_HEADERS32>(bytes, nt_offset);
  if (nt == nullptr || nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
      nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
    error = "Bridge DLL is not a valid x86 PE32 image";
    return false;
  }
  const auto sections_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
                               nt->FileHeader.SizeOfOptionalHeader;
  if (nt->FileHeader.NumberOfSections == 0U ||
      nt->FileHeader.NumberOfSections > 96U ||
      object_at<IMAGE_SECTION_HEADER>(bytes, sections_offset) == nullptr ||
      sections_offset + sizeof(IMAGE_SECTION_HEADER) * nt->FileHeader.NumberOfSections >
          bytes.size()) {
    error = "Bridge DLL has an invalid section table";
    return false;
  }
  const auto sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
      bytes.data() + sections_offset);
  const auto rva_to_offset = [&](const std::uint32_t rva,
                                 const std::size_t requested) -> std::optional<std::size_t> {
    if (rva < nt->OptionalHeader.SizeOfHeaders &&
        rva <= bytes.size() && requested <= bytes.size() - rva) {
      return static_cast<std::size_t>(rva);
    }
    for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
      const auto begin = sections[i].VirtualAddress;
      const auto raw_size = sections[i].SizeOfRawData;
      if (rva < begin || rva - begin > raw_size || requested > raw_size - (rva - begin)) {
        continue;
      }
      const auto offset = static_cast<std::size_t>(sections[i].PointerToRawData) +
                          (rva - begin);
      if (offset <= bytes.size() && requested <= bytes.size() - offset) {
        return offset;
      }
    }
    return std::nullopt;
  };

  const auto& export_data =
      nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  const auto export_offset = rva_to_offset(export_data.VirtualAddress,
                                           sizeof(IMAGE_EXPORT_DIRECTORY));
  if (!export_offset || export_data.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) {
    error = "Bridge DLL has no valid export directory";
    return false;
  }
  const auto exports = object_at<IMAGE_EXPORT_DIRECTORY>(bytes, *export_offset);
  if (exports == nullptr || exports->NumberOfNames > 65'536U ||
      exports->NumberOfFunctions > 65'536U) {
    error = "Bridge DLL has an invalid export table";
    return false;
  }
  const auto names_offset = rva_to_offset(
      exports->AddressOfNames, sizeof(std::uint32_t) * exports->NumberOfNames);
  const auto ordinals_offset = rva_to_offset(
      exports->AddressOfNameOrdinals, sizeof(std::uint16_t) * exports->NumberOfNames);
  const auto functions_offset = rva_to_offset(
      exports->AddressOfFunctions, sizeof(std::uint32_t) * exports->NumberOfFunctions);
  if (!names_offset || !ordinals_offset || !functions_offset) {
    error = "Bridge DLL export arrays are out of bounds";
    return false;
  }
  const auto names = reinterpret_cast<const std::uint32_t*>(bytes.data() + *names_offset);
  const auto ordinals = reinterpret_cast<const std::uint16_t*>(bytes.data() + *ordinals_offset);
  const auto functions = reinterpret_cast<const std::uint32_t*>(bytes.data() + *functions_offset);
  for (std::uint32_t i = 0; i < exports->NumberOfNames; ++i) {
    const auto name_offset = rva_to_offset(names[i], 1U);
    if (!name_offset) {
      continue;
    }
    const auto begin = reinterpret_cast<const char*>(bytes.data() + *name_offset);
    const auto maximum = bytes.size() - *name_offset;
    const auto terminator = static_cast<const char*>(std::memchr(begin, '\0', maximum));
    if (terminator == nullptr ||
        std::string_view(begin, static_cast<std::size_t>(terminator - begin)) != export_name) {
      continue;
    }
    const auto ordinal = ordinals[i];
    if (ordinal >= exports->NumberOfFunctions) {
      error = "Bridge export has an invalid ordinal";
      return false;
    }
    function_rva = functions[ordinal];
    const auto export_end = static_cast<std::uint64_t>(export_data.VirtualAddress) +
                            export_data.Size;
    if (function_rva >= export_data.VirtualAddress && function_rva < export_end) {
      error = "Forwarded bridge bootstrap exports are not supported";
      return false;
    }
    if (!rva_to_offset(function_rva, 1U)) {
      error = "Bridge bootstrap export points outside the image";
      return false;
    }
    return true;
  }
  error = "Ht2mpBootstrap export was not found in the bridge DLL";
  return false;
}

bool copy_bootstrap_string(const std::string_view value,
                           std::span<char> destination) {
  if (value.empty() || value.size() >= destination.size()) {
    return false;
  }
  std::copy(value.begin(), value.end(), destination.begin());
  destination[value.size()] = '\0';
  return true;
}

} // namespace

bool inject_and_bootstrap(const Options& options, std::string& error) {
  error.clear();
  static_assert(sizeof(void*) == 4U, "ht2mp-injector32 must be built as Win32");

  if (options.process_id == 0U || options.run_id == 0U ||
      options.profile_id.empty() ||
      options.profile_id.size() >= ht2mp::ipc::kProfileIdCapacity ||
      !options.pipe_name.starts_with(L"\\\\.\\pipe\\HT2MP-") ||
      options.pipe_name.size() >= ht2mp::ipc::kPipeNameCapacity) {
    error = "Invalid injector request";
    return false;
  }
  if (options.experimental_gog_actor_diagnostics &&
      !options.experimental_gog_telemetry) {
    error = "Actor diagnostics require experimental GOG telemetry";
    return false;
  }
  if (options.auto_enter_world && !options.experimental_gog_telemetry) {
    error = "Auto-enter requires experimental GOG telemetry";
    return false;
  }
  if (options.experimental_gog_actor_diagnostics &&
      options.profile_id != "gog-05588140") {
    error = "Experimental GOG actor diagnostics require profile gog-05588140";
    return false;
  }
  if (options.experimental_gog_remote_actors &&
      options.profile_id != "gog-05588140" &&
      options.profile_id != "steam-8138acee") {
    error = "Experimental remote actors require an exact supported profile";
    return false;
  }
  if (options.online_mode &&
      (options.profile_id != "steam-8138acee" ||
       !options.experimental_gog_telemetry ||
       !options.experimental_gog_remote_actors)) {
    error = "Online mode requires telemetry and remote actors on exact profile steam-8138acee";
    return false;
  }
  if ((options.appearance_override &&
       (options.profile_id != "steam-8138acee" ||
        options.vehicle_selector < 62U || options.vehicle_selector > 87U ||
        options.paint_variant > 3U)) ||
      (!options.appearance_override &&
       (options.vehicle_selector != 0xffffffffU ||
        options.paint_variant != 0U))) {
    error = "Appearance override requires an allowed exact-Steam vehicle and paint 0..3";
    return false;
  }
  if (!options.expected_executable.is_absolute() ||
      !options.bridge_dll.is_absolute()) {
    error = "Executable and bridge DLL paths must be absolute";
    return false;
  }
  std::error_code fs_error;
  if (!fs::is_regular_file(options.expected_executable, fs_error) || fs_error ||
      !fs::is_regular_file(options.bridge_dll, fs_error) || fs_error) {
    error = "Expected executable or bridge DLL does not exist";
    return false;
  }
  if (!ht2mp::windows::is_i386_pe(options.expected_executable, error) ||
      !ht2mp::windows::is_i386_pe(options.bridge_dll, error)) {
    return false;
  }
  ht2mp::windows::Sha256 expected_hash{};
  ht2mp::windows::Sha256 profile_hash{};
  ht2mp::windows::Sha256 actual_hash{};
  if (!ht2mp::windows::parse_sha256(options.expected_sha256, expected_hash)) {
    error = "Expected SHA-256 is malformed";
    return false;
  }
  const auto* profile = ht2mp::game::FindGameProfile(options.profile_id);
  if (profile == nullptr ||
      !ht2mp::windows::parse_sha256(profile->sha256, profile_hash)) {
    error = "Unknown or internally invalid game profile";
    return false;
  }
  if (expected_hash != profile_hash) {
    error = "Expected SHA-256 does not match requested profile";
    return false;
  }
  if (!ht2mp::windows::sha256_file(options.expected_executable, actual_hash, error)) {
    return false;
  }
  if (actual_hash != expected_hash) {
    error = "Target executable SHA-256 mismatch";
    return false;
  }
  const auto verification = ht2mp::game::VerifyGameExecutable(
      options.expected_executable, options.profile_id);
  if (!verification.accepted()) {
    error = "Target executable failed exact profile verification";
    if (!verification.errors.empty()) error += ": " + verification.errors.front();
    return false;
  }

  std::uint32_t bootstrap_rva{};
  if (!resolve_export_rva(options.bridge_dll, "Ht2mpBootstrap", bootstrap_rva, error)) {
    return false;
  }

  Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                 PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION |
                                 PROCESS_VM_READ | PROCESS_VM_WRITE | SYNCHRONIZE,
                             FALSE, options.process_id));
  if (!process.valid()) {
    error = "Cannot open target process: " +
            ht2mp::windows::win32_error(GetLastError()) +
            ". Run the game and sidecar as the same user; elevation is not required.";
    return false;
  }
  fs::path actual_process_path;
  if (!query_process_path(process.get(), actual_process_path, error)) {
    return false;
  }
  std::string identity_error;
  if (!ht2mp::windows::same_existing_file(actual_process_path,
                                           options.expected_executable,
                                           identity_error)) {
    error = "PID does not refer to the expected staged king.exe";
    if (!identity_error.empty()) {
      error += ": " + identity_error;
    }
    return false;
  }
  if (!target_is_i386(process.get(), error)) {
    return false;
  }

  LPTHREAD_START_ROUTINE remote_load_library{};
  if (!resolve_remote_load_library(options.process_id, remote_load_library, error)) {
    return false;
  }
  const auto dll_path = fs::absolute(options.bridge_dll).wstring();
  const auto dll_path_bytes = (dll_path.size() + 1U) * sizeof(wchar_t);
  RemoteAllocation remote_path(process.get(), dll_path_bytes);
  if (remote_path.get() == nullptr) {
    error = "Cannot allocate remote DLL path: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  if (!write_remote(process.get(), remote_path.get(), dll_path.c_str(),
                    dll_path_bytes, error)) {
    return false;
  }
  Handle load_thread(CreateRemoteThread(process.get(), nullptr, 0U,
                                        remote_load_library, remote_path.get(), 0U,
                                        nullptr));
  if (!load_thread.valid()) {
    error = "Cannot start remote LoadLibraryW: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  DWORD remote_module_result{};
  if (!wait_thread(load_thread.get(), 15'000U, remote_module_result, error)) {
    // The remote thread may still dereference this buffer. Leaking a few path
    // bytes until king.exe exits is safer than freeing live remote memory.
    remote_path.release();
    return false;
  }
  if (remote_module_result == 0U) {
    error = "Remote LoadLibraryW rejected the bridge DLL";
    return false;
  }
  const auto loaded_module = remote_module_base(options.process_id,
                                                options.bridge_dll.filename().wstring(),
                                                &options.bridge_dll, error);
  if (!loaded_module || *loaded_module != remote_module_result) {
    error = "Loaded bridge module identity/base did not match LoadLibraryW result";
    return false;
  }

  ht2mp::ipc::BootstrapV1 bootstrap;
  bootstrap.run_id = options.run_id;
  bootstrap.nonce = options.nonce;
  if (options.experimental_gog_telemetry) {
    bootstrap.flags |= static_cast<std::uint32_t>(
        ht2mp::ipc::BootstrapFlags::experimental_gog_telemetry);
  }
  if (options.experimental_gog_actor_diagnostics) {
    bootstrap.flags |= static_cast<std::uint32_t>(
        ht2mp::ipc::BootstrapFlags::experimental_gog_actor_diagnostics);
  }
  if (options.experimental_gog_remote_actors) {
    bootstrap.flags |= static_cast<std::uint32_t>(
        ht2mp::ipc::BootstrapFlags::experimental_gog_remote_actors);
  }
  if (options.online_mode) {
    bootstrap.flags |= static_cast<std::uint32_t>(
        ht2mp::ipc::BootstrapFlags::stock_npc_suppression);
    bootstrap.flags |= static_cast<std::uint32_t>(
        ht2mp::ipc::BootstrapFlags::background_tick);
  }
  if (options.appearance_override) {
    bootstrap.flags |= static_cast<std::uint32_t>(
        ht2mp::ipc::BootstrapFlags::appearance_override);
    bootstrap.vehicle_selector = options.vehicle_selector;
    bootstrap.paint_variant = options.paint_variant;
  }
  if (options.auto_enter_world) {
    bootstrap.flags |= static_cast<std::uint32_t>(
        ht2mp::ipc::BootstrapFlags::auto_enter_world);
  }
  if (!copy_bootstrap_string(options.profile_id, bootstrap.profile_id)) {
    error = "Profile ID is too long for BootstrapV1";
    return false;
  }
  std::copy(options.pipe_name.begin(), options.pipe_name.end(),
            bootstrap.pipe_name.begin());
  bootstrap.pipe_name[options.pipe_name.size()] = L'\0';

  RemoteAllocation remote_bootstrap(process.get(), sizeof(bootstrap));
  if (remote_bootstrap.get() == nullptr) {
    error = "Cannot allocate remote BootstrapV1: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  if (!write_remote(process.get(), remote_bootstrap.get(), &bootstrap,
                    sizeof(bootstrap), error)) {
    return false;
  }
  const auto remote_entrypoint = reinterpret_cast<LPTHREAD_START_ROUTINE>(
      *loaded_module + bootstrap_rva);
  Handle bootstrap_thread(CreateRemoteThread(process.get(), nullptr, 0U,
                                             remote_entrypoint,
                                             remote_bootstrap.get(), 0U, nullptr));
  if (!bootstrap_thread.valid()) {
    error = "Cannot invoke remote Ht2mpBootstrap: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  DWORD bootstrap_result{};
  if (!wait_thread(bootstrap_thread.get(), 15'000U, bootstrap_result, error)) {
    // Same rule for an indeterminate bootstrap call: never create a remote UAF.
    remote_bootstrap.release();
    return false;
  }
  if (bootstrap_result != static_cast<DWORD>(ht2mp::ipc::BootstrapResult::ok)) {
    error = "Ht2mpBootstrap failed with result " + std::to_string(bootstrap_result);
    return false;
  }
  return true;
}

} // namespace ht2mp::injector
