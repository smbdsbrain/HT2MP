#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;

template <typename T>
class ComPtr final {
 public:
  ~ComPtr() {
    if (value_ != nullptr) value_->Release();
  }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr() = default;
  [[nodiscard]] T* get() const noexcept { return value_; }
  [[nodiscard]] T** put() noexcept { return &value_; }
  T* operator->() const noexcept { return value_; }

 private:
  T* value_{};
};

bool stream_exists(IStorage* storage, const wchar_t* name) {
  ComPtr<IStream> stream;
  return SUCCEEDED(storage->OpenStream(name, nullptr,
                                       STGM_READ | STGM_SHARE_EXCLUSIVE, 0U,
                                       stream.put()));
}

bool complete_slot(IStorage* storage) {
  constexpr std::array names{L"XAI", L"env", L"anm", L"stat", L"cont"};
  return std::all_of(names.begin(), names.end(), [storage](const auto* name) {
    return stream_exists(storage, name);
  });
}

struct Slot final {
  std::wstring name;
  bool complete{};
};

std::string utf8(const std::wstring_view value) {
  if (value.empty()) return {};
  const auto length = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0, nullptr, nullptr);
  if (length <= 0) return "<invalid-utf16>";
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), length,
                      nullptr, nullptr);
  return result;
}

bool enumerate_slots(IStorage* root, const DWORD access,
                     ComPtr<IStorage>& games, std::vector<Slot>& slots,
                     std::wstring& error) {
  if (FAILED(root->OpenStorage(L"listofgames", nullptr,
                               access | STGM_SHARE_EXCLUSIVE, nullptr, 0U,
                               games.put()))) {
    error = L"profile has no listofgames storage";
    return false;
  }
  ComPtr<IEnumSTATSTG> enumerator;
  if (FAILED(games->EnumElements(0U, nullptr, 0U, enumerator.put()))) {
    error = L"cannot enumerate listofgames";
    return false;
  }
  for (;;) {
    STATSTG stat{};
    ULONG fetched{};
    const auto next = enumerator->Next(1U, &stat, &fetched);
    if (next == S_FALSE || fetched == 0U) break;
    if (FAILED(next)) {
      error = L"cannot enumerate listofgames";
      return false;
    }
    const std::wstring name = stat.pwcsName == nullptr ? L"" : stat.pwcsName;
    CoTaskMemFree(stat.pwcsName);
    if (stat.type != STGTY_STORAGE || name.empty()) continue;
    ComPtr<IStorage> slot;
    const bool opened = SUCCEEDED(games->OpenStorage(
        name.c_str(), nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0U,
        slot.put()));
    slots.push_back({name, opened && complete_slot(slot.get())});
  }
  return true;
}

bool open_root(const fs::path& path, const DWORD access, ComPtr<IStorage>& root,
               std::wstring& error) {
  if (StgIsStorageFile(path.c_str()) != S_OK) {
    error = L"not a Windows Structured Storage file";
    return false;
  }
  const auto opened = StgOpenStorage(
      path.c_str(), nullptr, access | STGM_SHARE_EXCLUSIVE, nullptr, 0U,
      root.put());
  if (FAILED(opened)) {
    error = L"cannot open profile (HRESULT " +
            std::to_wstring(static_cast<unsigned long>(opened)) + L")";
    return false;
  }
  return true;
}

bool list_slots(const fs::path& source) {
  std::wstring error;
  ComPtr<IStorage> root;
  if (!open_root(source, STGM_READ, root, error)) {
    std::wcerr << error << L'\n';
    return false;
  }
  ComPtr<IStorage> games;
  std::vector<Slot> slots;
  if (!enumerate_slots(root.get(), STGM_READ, games, slots, error)) {
    std::wcerr << error << L'\n';
    return false;
  }
  std::cout << "saved-game storages: " << slots.size() << '\n';
  for (const auto& slot : slots) {
    std::cout << "  " << utf8(slot.name)
              << (slot.complete ? " [complete]" : " [incomplete]") << '\n';
  }
  return true;
}

bool list_tree_recursive(IStorage* storage, const unsigned depth,
                         std::wstring& error) {
  ComPtr<IEnumSTATSTG> enumerator;
  if (FAILED(storage->EnumElements(0U, nullptr, 0U, enumerator.put()))) {
    error = L"cannot enumerate storage";
    return false;
  }
  for (;;) {
    STATSTG stat{};
    ULONG fetched{};
    const auto next = enumerator->Next(1U, &stat, &fetched);
    if (next == S_FALSE || fetched == 0U) break;
    if (FAILED(next)) {
      error = L"cannot enumerate storage";
      return false;
    }
    const std::wstring name = stat.pwcsName == nullptr ? L"" : stat.pwcsName;
    CoTaskMemFree(stat.pwcsName);
    if (name.empty()) continue;
    std::cout << std::string(depth * 2U, ' ') << utf8(name);
    if (stat.type == STGTY_STREAM) {
      std::cout << " [stream " << stat.cbSize.LowPart << "]\n";
    } else if (stat.type == STGTY_STORAGE) {
      std::cout << " [storage]\n";
      ComPtr<IStorage> child;
      if (FAILED(storage->OpenStorage(name.c_str(), nullptr,
                                      STGM_READ | STGM_SHARE_EXCLUSIVE,
                                      nullptr, 0U, child.put())) ||
          !list_tree_recursive(child.get(), depth + 1U, error)) {
        if (error.empty()) error = L"cannot open child storage";
        return false;
      }
    } else {
      std::cout << " [type " << stat.type << "]\n";
    }
  }
  return true;
}

bool list_tree(const fs::path& source) {
  std::wstring error;
  ComPtr<IStorage> root;
  if (!open_root(source, STGM_READ, root, error) ||
      !list_tree_recursive(root.get(), 0U, error)) {
    std::wcerr << error << L'\n';
    return false;
  }
  return true;
}

bool dump_slot_stream(const fs::path& source, const std::wstring& slot_name,
                      const std::wstring& stream_name) {
  std::wstring error;
  ComPtr<IStorage> root;
  ComPtr<IStorage> games;
  ComPtr<IStorage> slot;
  ComPtr<IStream> stream;
  if (!open_root(source, STGM_READ, root, error) ||
      FAILED(root->OpenStorage(L"listofgames", nullptr,
                               STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0U,
                               games.put())) ||
      FAILED(games->OpenStorage(slot_name.c_str(), nullptr,
                                STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0U,
                                slot.put())) ||
      FAILED(slot->OpenStream(stream_name.c_str(), nullptr,
                              STGM_READ | STGM_SHARE_EXCLUSIVE, 0U,
                              stream.put()))) {
    if (error.empty()) error = L"cannot open requested saved-game stream";
    std::wcerr << error << L'\n';
    return false;
  }
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) ||
      stat.cbSize.HighPart != 0U || stat.cbSize.LowPart > 16U * 1024U * 1024U) {
    std::wcerr << L"invalid saved-game stream size\n";
    return false;
  }
  std::vector<std::uint8_t> bytes(stat.cbSize.LowPart);
  ULONG read{};
  if ((!bytes.empty() &&
       FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()),
                           &read))) ||
      read != bytes.size()) {
    std::wcerr << L"cannot read requested saved-game stream\n";
    return false;
  }
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    if (index != 0U) std::cout << ' ';
    std::cout << std::hex << std::uppercase << std::setw(2)
              << std::setfill('0') << static_cast<unsigned>(bytes[index]);
  }
  std::cout << '\n';
  return true;
}

bool extract_slot(const fs::path& source, const fs::path& destination,
                  const std::wstring& selected) {
  std::error_code ec;
  const auto source_absolute = fs::absolute(source, ec).lexically_normal();
  if (ec) return false;
  const auto destination_absolute =
      fs::absolute(destination, ec).lexically_normal();
  if (ec || _wcsicmp(source_absolute.c_str(), destination_absolute.c_str()) == 0) {
    std::wcerr << L"source and destination must be different files\n";
    return false;
  }
  if (!fs::is_regular_file(source, ec) || ec ||
      !fs::is_directory(destination.parent_path(), ec) || ec) {
    std::wcerr << L"source file or destination directory is missing\n";
    return false;
  }

  const auto temporary = destination.wstring() + L".tmp-" +
                         std::to_wstring(GetCurrentProcessId());
  DeleteFileW(temporary.c_str());

  bool ok{};
  std::wstring error;
  {
    ComPtr<IStorage> source_root;
    if (open_root(source, STGM_READ, source_root, error)) {
      ComPtr<IStorage> source_games;
      std::vector<Slot> slots;
      if (enumerate_slots(source_root.get(), STGM_READ, source_games, slots,
                          error)) {
        const auto selected_slot = std::find_if(
            slots.begin(), slots.end(), [&selected](const Slot& slot) {
              return slot.name == selected && slot.complete;
            });
        if (selected_slot == slots.end()) {
          error = L"requested complete slot was not found";
        } else {
          ComPtr<IStorage> destination_root;
          const auto created = StgCreateDocfile(
              temporary.c_str(),
              STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0U,
              destination_root.put());
          if (FAILED(created)) {
            error = L"cannot create compact destination storage";
          } else {
            // Copy the driver-level data into a brand-new compound file while
            // excluding listofgames.  Rebuilding rather than deleting in place
            // ensures data from discarded saves cannot remain in free sectors.
            wchar_t excluded_name[] = L"listofgames";
            OLECHAR* excluded[] = {excluded_name, nullptr};
            if (FAILED(source_root->CopyTo(0U, nullptr, excluded,
                                           destination_root.get()))) {
              error = L"cannot copy driver-level profile data";
            } else {
              ComPtr<IStorage> source_slot;
              ComPtr<IStorage> destination_games;
              ComPtr<IStorage> destination_slot;
              const auto opened_slot = source_games->OpenStorage(
                  selected.c_str(), nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE,
                  nullptr, 0U, source_slot.put());
              const auto created_games = destination_root->CreateStorage(
                  L"listofgames",
                  STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0U, 0U,
                  destination_games.put());
              const auto created_slot =
                  SUCCEEDED(created_games)
                      ? destination_games->CreateStorage(
                            selected.c_str(),
                            STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
                            0U, 0U, destination_slot.put())
                      : E_FAIL;
              ok = SUCCEEDED(opened_slot) && SUCCEEDED(created_games) &&
                   SUCCEEDED(created_slot) &&
                   SUCCEEDED(source_slot->CopyTo(
                       0U, nullptr, nullptr, destination_slot.get())) &&
                   SUCCEEDED(destination_slot->Commit(STGC_DEFAULT)) &&
                   SUCCEEDED(destination_games->Commit(STGC_DEFAULT)) &&
                   SUCCEEDED(destination_root->Commit(STGC_DEFAULT));
              if (!ok) error = L"cannot copy selected saved-game slot";
            }
          }
        }
      }
    }
  }

  if (ok) {
    ComPtr<IStorage> root;
    ComPtr<IStorage> games;
    std::vector<Slot> slots;
    ok = open_root(temporary, STGM_READ, root, error) &&
         enumerate_slots(root.get(), STGM_READ, games, slots, error) &&
         slots.size() == 1U && slots.front().name == selected &&
         slots.front().complete;
    if (!ok && error.empty()) error = L"pruned template failed verification";
  }

  if (ok && !MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = L"cannot atomically replace destination template";
    ok = false;
  }
  if (!ok) {
    DeleteFileW(temporary.c_str());
    std::wcerr << error << L'\n';
  }
  return ok;
}

bool prune_copy(const fs::path& source, const fs::path& destination,
                const std::wstring& selected) {
  std::error_code ec;
  const auto source_absolute = fs::absolute(source, ec).lexically_normal();
  if (ec) return false;
  const auto destination_absolute =
      fs::absolute(destination, ec).lexically_normal();
  if (ec ||
      _wcsicmp(source_absolute.c_str(), destination_absolute.c_str()) == 0 ||
      !fs::is_regular_file(source, ec) || ec ||
      !fs::is_directory(destination.parent_path(), ec) || ec) {
    std::wcerr << L"invalid source or destination\n";
    return false;
  }

  const auto temporary = destination.wstring() + L".tmp-" +
                         std::to_wstring(GetCurrentProcessId());
  DeleteFileW(temporary.c_str());
  if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE)) {
    std::wcerr << L"cannot copy source profile\n";
    return false;
  }

  bool ok{};
  std::wstring error;
  {
    ComPtr<IStorage> root;
    if (open_root(temporary, STGM_READWRITE | STGM_TRANSACTED, root, error)) {
      ComPtr<IStorage> games;
      std::vector<Slot> slots;
      if (enumerate_slots(root.get(), STGM_READWRITE, games, slots, error)) {
        const auto retained = std::find_if(
            slots.begin(), slots.end(), [&selected](const Slot& slot) {
              return slot.name == selected && slot.complete;
            });
        if (retained == slots.end()) {
          error = L"requested complete slot was not found";
        } else {
          ok = true;
          for (const auto& slot : slots) {
            if (slot.name != selected &&
                FAILED(games->DestroyElement(slot.name.c_str()))) {
              error = L"cannot remove saved-game slot " + slot.name;
              ok = false;
              break;
            }
          }
          if (ok && (FAILED(games->Commit(STGC_DEFAULT)) ||
                     FAILED(root->Commit(STGC_OVERWRITE)))) {
            error = L"cannot commit pruned profile";
            ok = false;
          }
        }
      }
    }
  }

  if (ok) {
    ComPtr<IStorage> root;
    ComPtr<IStorage> games;
    std::vector<Slot> slots;
    ok = open_root(temporary, STGM_READ, root, error) &&
         enumerate_slots(root.get(), STGM_READ, games, slots, error) &&
         slots.size() == 1U && slots.front().name == selected &&
         slots.front().complete;
    if (!ok && error.empty()) error = L"pruned profile failed verification";
  }
  if (ok && !MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = L"cannot atomically replace destination profile";
    ok = false;
  }
  if (!ok) {
    DeleteFileW(temporary.c_str());
    std::wcerr << error << L'\n';
  }
  return ok;
}

}  // namespace

int wmain(const int argc, const wchar_t* const argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) {
    std::wcerr << L"cannot initialize COM\n";
    return 1;
  }
  bool ok{};
  if (argc == 3 && std::wstring_view(argv[1]) == L"--list") {
    ok = list_slots(argv[2]);
  } else if (argc == 3 && std::wstring_view(argv[1]) == L"--tree") {
    ok = list_tree(argv[2]);
  } else if (argc == 5 &&
             std::wstring_view(argv[1]) == L"--dump-slot-stream") {
    ok = dump_slot_stream(argv[2], argv[3], argv[4]);
  } else if (argc == 5 && std::wstring_view(argv[1]) == L"--extract") {
    ok = extract_slot(argv[2], argv[3], argv[4]);
  } else if (argc == 5 && std::wstring_view(argv[1]) == L"--prune-copy") {
    ok = prune_copy(argv[2], argv[3], argv[4]);
  } else {
    std::wcerr << L"usage:\n"
                  L"  ht2mp-profile-template-tool --list <profile.pl1>\n"
                  L"  ht2mp-profile-template-tool --tree <profile.pl1>\n"
                  L"  ht2mp-profile-template-tool --dump-slot-stream "
                  L"<profile.pl1> <slot-name> <stream-name>\n"
                  L"  ht2mp-profile-template-tool --extract <source.pl1> "
                  L"<destination.pl1> <slot-name>\n"
                  L"  ht2mp-profile-template-tool --prune-copy <source.pl1> "
                  L"<destination.pl1> <slot-name>\n";
  }
  CoUninitialize();
  return ok ? 0 : 1;
}
