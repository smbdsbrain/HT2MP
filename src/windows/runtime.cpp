#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <system_error>
#include <vector>

namespace ht2mp::windows {
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

class Algorithm final {
public:
  ~Algorithm() {
    if (value_ != nullptr) {
      BCryptCloseAlgorithmProvider(value_, 0U);
    }
  }
  BCRYPT_ALG_HANDLE value_{};
};

class Hash final {
public:
  ~Hash() {
    if (value_ != nullptr) {
      BCryptDestroyHash(value_);
    }
  }
  BCRYPT_HASH_HANDLE value_{};
};

std::string ntstatus_error(const char* operation, const NTSTATUS status) {
  std::ostringstream stream;
  stream << operation << " failed with NTSTATUS 0x" << std::hex
         << static_cast<std::uint32_t>(status);
  return stream.str();
}

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
    return static_cast<wchar_t>(towlower(ch));
  });
  return value;
}

bool read_exact(HANDLE file, void* destination, const DWORD size, std::string& error) {
  DWORD read{};
  if (!ReadFile(file, destination, size, &read, nullptr) || read != size) {
    error = "Cannot read PE header: " + win32_error(GetLastError());
    return false;
  }
  return true;
}

} // namespace

bool sha256_file(const std::filesystem::path& path, Sha256& digest, std::string& error) {
  error.clear();
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr));
  if (!file.valid()) {
    error = "Cannot open file for SHA-256: " + win32_error(GetLastError());
    return false;
  }

  Algorithm algorithm;
  auto status = BCryptOpenAlgorithmProvider(&algorithm.value_, BCRYPT_SHA256_ALGORITHM,
                                             nullptr, 0U);
  if (!BCRYPT_SUCCESS(status)) {
    error = ntstatus_error("BCryptOpenAlgorithmProvider", status);
    return false;
  }

  DWORD object_size{};
  DWORD result_size{};
  status = BCryptGetProperty(algorithm.value_, BCRYPT_OBJECT_LENGTH,
                             reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                             &result_size, 0U);
  if (!BCRYPT_SUCCESS(status)) {
    error = ntstatus_error("BCryptGetProperty(BCRYPT_OBJECT_LENGTH)", status);
    return false;
  }

  std::vector<std::uint8_t> object(object_size);
  Hash hash;
  status = BCryptCreateHash(algorithm.value_, &hash.value_, object.data(), object_size,
                            nullptr, 0U, 0U);
  if (!BCRYPT_SUCCESS(status)) {
    error = ntstatus_error("BCryptCreateHash", status);
    return false;
  }

  std::array<std::uint8_t, 64U * 1024U> buffer{};
  for (;;) {
    DWORD read{};
    if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                  nullptr)) {
      error = "Cannot read file for SHA-256: " + win32_error(GetLastError());
      return false;
    }
    if (read == 0U) {
      break;
    }
    status = BCryptHashData(hash.value_, buffer.data(), read, 0U);
    if (!BCRYPT_SUCCESS(status)) {
      error = ntstatus_error("BCryptHashData", status);
      return false;
    }
  }

  status = BCryptFinishHash(hash.value_, digest.data(), static_cast<ULONG>(digest.size()), 0U);
  if (!BCRYPT_SUCCESS(status)) {
    error = ntstatus_error("BCryptFinishHash", status);
    return false;
  }
  return true;
}

std::string sha256_hex(const Sha256& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.resize(digest.size() * 2U);
  for (std::size_t i = 0; i < digest.size(); ++i) {
    output[i * 2U] = kHex[digest[i] >> 4U];
    output[i * 2U + 1U] = kHex[digest[i] & 0xFU];
  }
  return output;
}

bool parse_sha256(const std::string_view text, Sha256& digest) noexcept {
  if (text.size() != digest.size() * 2U) {
    return false;
  }
  const auto nibble = [](const char ch) -> int {
    if (ch >= '0' && ch <= '9') {
      return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
      return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
      return ch - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t i = 0; i < digest.size(); ++i) {
    const auto high = nibble(text[i * 2U]);
    const auto low = nibble(text[i * 2U + 1U]);
    if (high < 0 || low < 0) {
      return false;
    }
    digest[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

bool secure_random(const std::span<std::uint8_t> destination, std::string& error) {
  error.clear();
  if (destination.size() > std::numeric_limits<ULONG>::max()) {
    error = "Random destination is too large";
    return false;
  }
  const auto status = BCryptGenRandom(nullptr, destination.data(),
                                      static_cast<ULONG>(destination.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!BCRYPT_SUCCESS(status)) {
    error = ntstatus_error("BCryptGenRandom", status);
    return false;
  }
  return true;
}

bool is_i386_pe(const std::filesystem::path& path, std::string& error) {
  error.clear();
  Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.valid()) {
    error = "Cannot open PE file: " + win32_error(GetLastError());
    return false;
  }
  IMAGE_DOS_HEADER dos{};
  if (!read_exact(file.get(), &dos, sizeof(dos), error)) {
    return false;
  }
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
      dos.e_lfanew > 16 * 1024 * 1024) {
    error = "File has an invalid DOS/PE header";
    return false;
  }
  LARGE_INTEGER offset{};
  offset.QuadPart = dos.e_lfanew;
  if (!SetFilePointerEx(file.get(), offset, nullptr, FILE_BEGIN)) {
    error = "Cannot seek to PE header: " + win32_error(GetLastError());
    return false;
  }
  DWORD signature{};
  IMAGE_FILE_HEADER header{};
  if (!read_exact(file.get(), &signature, sizeof(signature), error) ||
      !read_exact(file.get(), &header, sizeof(header), error)) {
    return false;
  }
  if (signature != IMAGE_NT_SIGNATURE || header.Machine != IMAGE_FILE_MACHINE_I386) {
    error = "Expected a 32-bit i386 PE image";
    return false;
  }
  return true;
}

bool same_existing_file(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs,
    std::string& error) {
  error.clear();
  Handle left(CreateFileW(lhs.c_str(), FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!left.valid()) {
    error = "Cannot identify first path: " + win32_error(GetLastError());
    return false;
  }
  Handle right(CreateFileW(rhs.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!right.valid()) {
    error = "Cannot identify second path: " + win32_error(GetLastError());
    return false;
  }
  BY_HANDLE_FILE_INFORMATION left_info{};
  BY_HANDLE_FILE_INFORMATION right_info{};
  if (!GetFileInformationByHandle(left.get(), &left_info) ||
      !GetFileInformationByHandle(right.get(), &right_info)) {
    error = "Cannot query file identity: " + win32_error(GetLastError());
    return false;
  }
  return left_info.dwVolumeSerialNumber == right_info.dwVolumeSerialNumber &&
         left_info.nFileIndexHigh == right_info.nFileIndexHigh &&
         left_info.nFileIndexLow == right_info.nFileIndexLow;
}

bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate,
    std::string& error) {
  error.clear();
  std::error_code ec;
  // Do not let the spelling of an already-existing final component change the
  // answer.  MSVC's weakly_canonical may return a device-resolved spelling for
  // an existing child while retaining the DOS spelling for a not-yet-created
  // child, which broke transactional restaging on the second pass.  Security-
  // sensitive callers separately reject reparse points before using this
  // lexical containment result.
  const auto normalized_root =
      std::filesystem::absolute(root, ec).lexically_normal();
  if (ec) {
    error = "Cannot normalize root path: " + ec.message();
    return false;
  }
  const auto normalized_candidate =
      std::filesystem::absolute(candidate, ec).lexically_normal();
  if (ec) {
    error = "Cannot normalize candidate path: " + ec.message();
    return false;
  }

  auto root_it = normalized_root.begin();
  auto candidate_it = normalized_candidate.begin();
  for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
    if (candidate_it == normalized_candidate.end() ||
        lowercase(root_it->wstring()) != lowercase(candidate_it->wstring())) {
      return false;
    }
  }
  return true;
}

std::filesystem::path local_app_data(std::string& error) {
  error.clear();
  PWSTR raw_path{};
  const auto status = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                           nullptr, &raw_path);
  if (FAILED(status)) {
    std::ostringstream stream;
    stream << "SHGetKnownFolderPath failed with HRESULT 0x" << std::hex
           << static_cast<std::uint32_t>(status);
    error = stream.str();
    return {};
  }
  std::filesystem::path result(raw_path);
  CoTaskMemFree(raw_path);
  return result;
}

std::filesystem::path executable_path(std::string& error) {
  error.clear();
  std::wstring buffer(32768U, L'\0');
  const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    error = "GetModuleFileNameW failed: " + win32_error(GetLastError());
    return {};
  }
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

std::wstring quote_command_line_argument(const std::wstring_view argument) {
  if (argument.empty()) {
    return L"\"\"";
  }
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }
  std::wstring output;
  output.push_back(L'\"');
  std::size_t backslashes{};
  for (const auto ch : argument) {
    if (ch == L'\\') {
      ++backslashes;
      continue;
    }
    if (ch == L'\"') {
      output.append(backslashes * 2U + 1U, L'\\');
      output.push_back(L'\"');
      backslashes = 0U;
      continue;
    }
    output.append(backslashes, L'\\');
    backslashes = 0U;
    output.push_back(ch);
  }
  output.append(backslashes * 2U, L'\\');
  output.push_back(L'\"');
  return output;
}

std::wstring widen_utf8(const std::string_view input, std::string& error) {
  error.clear();
  if (input.empty()) {
    return {};
  }
  const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                             static_cast<int>(input.size()), nullptr, 0);
  if (required <= 0) {
    error = "Invalid UTF-8: " + win32_error(GetLastError());
    return {};
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                          static_cast<int>(input.size()), result.data(), required) != required) {
    error = "UTF-8 conversion failed: " + win32_error(GetLastError());
    return {};
  }
  return result;
}

std::string narrow_utf8(const std::wstring_view input, std::string& error) {
  error.clear();
  if (input.empty()) {
    return {};
  }
  const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                             static_cast<int>(input.size()), nullptr, 0,
                                             nullptr, nullptr);
  if (required <= 0) {
    error = "Invalid UTF-16: " + win32_error(GetLastError());
    return {};
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                          static_cast<int>(input.size()), result.data(), required,
                          nullptr, nullptr) != required) {
    error = "UTF-16 conversion failed: " + win32_error(GetLastError());
    return {};
  }
  return result;
}

std::string win32_error(const std::uint32_t code) {
  wchar_t* raw_message{};
  const auto length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<wchar_t*>(&raw_message), 0U, nullptr);
  if (length == 0U || raw_message == nullptr) {
    return "Win32 error " + std::to_string(code);
  }
  std::wstring message(raw_message, length);
  LocalFree(raw_message);
  while (!message.empty() &&
         (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
    message.pop_back();
  }
  std::string conversion_error;
  auto result = narrow_utf8(message, conversion_error);
  if (!conversion_error.empty()) {
    return "Win32 error " + std::to_string(code);
  }
  return result + " (" + std::to_string(code) + ")";
}

} // namespace ht2mp::windows
