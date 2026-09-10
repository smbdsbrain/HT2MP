#include "profile_editor.hpp"

#include "ht2mp/protocol/vehicle_catalog.hpp"
#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace ht2mp::launcher {
namespace fs = std::filesystem;
namespace {

// The exact Steam build ties its native Load action to the internal driver
// identity stored in the template.  That identity is ВОДИТЕЛЬ, so changing
// only the compound-file name leaves the game at the main menu.  Keep the
// proven native identity in the isolated runtime; the user-facing online
// player name is independent and remains configurable in the launcher.
constexpr wchar_t kOnlineDriverFilename[] =
    L"\u0412\u041e\u0414\u0418\u0422\u0415\u041b\u042c.pl1";
constexpr wchar_t kOnlineDriverBackupFilename[] =
    L"\u0412\u041e\u0414\u0418\u0422\u0415\u041b\u042c.pl1.bak";
constexpr wchar_t kLegacyOnlineDriverFilename[] = L"HT2MP.pl1";
constexpr char kLastPlayerLine[] =
    "lastp=\xC2\xCE\xC4\xC8\xD2\xC5\xCB\xDC";  // CP1251: ВОДИТЕЛЬ

template <typename T>
class ComPtr final {
 public:
  ComPtr() = default;
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  [[nodiscard]] T* get() const noexcept { return value_; }
  [[nodiscard]] T** put() noexcept {
    reset();
    return &value_;
  }
  T* operator->() const noexcept { return value_; }
  void reset() noexcept {
    if (value_ != nullptr) value_->Release();
    value_ = nullptr;
  }

 private:
  T* value_{};
};

#pragma pack(push, 1)
struct AppearanceRecord final {
  std::uint32_t magic{0x50413248U};  // H2AP
  std::uint16_t version{1U};
  std::uint16_t selector{};
  std::uint8_t paint{};
  std::array<std::uint8_t, 3> reserved{};
  std::uint32_t checksum{};
};
#pragma pack(pop)
static_assert(sizeof(AppearanceRecord) == 16U);

AppearanceRecord make_record(const std::uint16_t selector,
                             const std::uint8_t paint) noexcept {
  AppearanceRecord record;
  record.selector = selector;
  record.paint = paint;
  record.checksum = record.magic ^ record.version ^ record.selector ^
                    record.paint ^ 0xa5c31f29U;
  return record;
}

bool valid_record(const AppearanceRecord& record, const std::uint16_t selector,
                  const std::uint8_t paint) noexcept {
  const auto expected = make_record(selector, paint);
  return std::memcmp(&record, &expected, sizeof(record)) == 0;
}

bool stream_exists(IStorage* storage, const wchar_t* name) noexcept {
  ComPtr<IStream> stream;
  return SUCCEEDED(storage->OpenStream(name, nullptr,
                                       STGM_READ | STGM_SHARE_EXCLUSIVE, 0U,
                                       stream.put()));
}

bool stream_header_equals(IStorage* storage, const wchar_t* name,
                          const std::uint32_t expected,
                          const bool exact_four_bytes) noexcept {
  ComPtr<IStream> stream;
  if (FAILED(storage->OpenStream(name, nullptr,
                                 STGM_READ | STGM_SHARE_EXCLUSIVE, 0U,
                                 stream.put()))) {
    return false;
  }
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) ||
      stat.cbSize.HighPart != 0U || stat.cbSize.LowPart < sizeof(expected) ||
      (exact_four_bytes && stat.cbSize.LowPart != sizeof(expected)) ||
      stat.cbSize.LowPart > 64U) {
    return false;
  }
  std::uint32_t value{};
  ULONG read{};
  return SUCCEEDED(stream->Read(&value, sizeof(value), &read)) &&
         read == sizeof(value) && value == expected;
}

bool valid_profile_header(IStorage* storage) noexcept {
  // The exact Steam profile format observed for this build is driver format
  // 4 with settings schema 5.  Refuse older/newer containers instead of
  // attempting a best-effort edit.
  return stream_header_equals(storage, L"ver", 4U, true) &&
         stream_header_equals(storage, L"settings", 5U, false);
}

bool king_process_running() noexcept {
  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0U);
  if (snapshot == INVALID_HANDLE_VALUE) return true;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool running = true;
  if (Process32FirstW(snapshot, &entry)) {
    running = false;
    do {
      if (_wcsicmp(entry.szExeFile, L"king.exe") == 0) {
        running = true;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return running;
}

bool complete_slot(IStorage* storage) noexcept {
  constexpr std::array names{L"XAI", L"env", L"anm", L"stat", L"cont"};
  return std::all_of(names.begin(), names.end(),
                     [storage](const wchar_t* name) {
                       return stream_exists(storage, name);
                     });
}

constexpr std::array<std::uint8_t, 4> kTotalTag{'T', 'O', 'T', 'L'};
constexpr std::array<std::uint8_t, 4> kGlobalsTag{'G', 'L', 'B', 'L'};
constexpr std::array<std::uint8_t, 4> kPlayersTag{'P', 'L', 'R', 'S'};
constexpr std::array<std::uint8_t, 4> kPlayerTag{'P', 'L', 'Y', 'R'};
constexpr std::array<std::uint8_t, 4> kPlayerDataTag{'P', 'L', 'Y', 'D'};
constexpr std::array<std::uint8_t, 4> kPlayerVehicleTag{'P', 'V', 'E', 'H'};
constexpr std::string_view kLocalPlayerName{"$$$_LIVE_0_0\0", 13U};
constexpr std::uint32_t kPlayerVehiclePayloadSize = 0xfcU;
constexpr std::uint32_t kMaximumXaiSize = 16U * 1024U * 1024U;
constexpr std::uint32_t kMaximumAnmSize = 64U * 1024U * 1024U;
constexpr std::uint32_t kAnmPageCount = 400U;
constexpr std::uint32_t kAnmSlotsPerPage = 500U;
constexpr std::uint32_t kAnmObjectIdStride = 1000U;
constexpr std::uint32_t kAnmNullObject = 0xffffffffU;
constexpr std::uint32_t kAnmEndPage = 0xfffffffeU;
constexpr std::uint32_t kExactSteamVehiclePayloadSize = 300U;
constexpr std::size_t kExactSteamVehicleTypeOffset = 0U;
constexpr std::size_t kExactSteamVehicleRoomOffset = 16U;
// FUN_0041fcf0 in the exact Steam executable compares every company's market
// share with this serialized value.  Its initial value is 0.51.  Once the
// campaign result has been announced, the native code changes it to 1.05 so
// the one-shot result cannot fire again.  Online mode deliberately starts in
// that native post-result state because removing stock companies leaves the
// local company with 100% of the remaining market.
constexpr std::uint64_t kInitialCampaignResultThreshold =
    0x3fe051eb851eb852ULL;  // 0.51
constexpr std::uint64_t kAcknowledgedCampaignResultThreshold =
    0x3ff0cccccccccccdULL;  // 1.05
constexpr std::size_t kGlobalsPrefixBeforeFirstVector = 296U;
constexpr std::size_t kGlobalsFieldsBetweenVectors = 21U;
constexpr std::size_t kGlobalsTailAfterSecondVector = 32U;

std::uint32_t load_u32(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void store_u32(const std::span<std::uint8_t> bytes, const std::size_t offset,
               const std::uint32_t value) noexcept {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
  bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

std::uint64_t load_u64(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) noexcept {
  return static_cast<std::uint64_t>(load_u32(bytes, offset)) |
         (static_cast<std::uint64_t>(load_u32(bytes, offset + 4U)) << 32U);
}

void store_u64(const std::span<std::uint8_t> bytes, const std::size_t offset,
               const std::uint64_t value) noexcept {
  store_u32(bytes, offset, static_cast<std::uint32_t>(value));
  store_u32(bytes, offset + 4U, static_cast<std::uint32_t>(value >> 32U));
}

bool tag_equals(const std::span<const std::uint8_t> bytes,
                const std::size_t offset,
                const std::array<std::uint8_t, 4>& tag) noexcept {
  return offset <= bytes.size() && tag.size() <= bytes.size() - offset &&
         std::equal(tag.begin(), tag.end(), bytes.begin() + offset);
}

bool chunk_bounds(const std::span<const std::uint8_t> bytes,
                  const std::size_t offset, const std::size_t enclosing_end,
                  std::size_t& payload_begin,
                  std::size_t& payload_end) noexcept {
  if (enclosing_end > bytes.size() || offset > enclosing_end ||
      enclosing_end - offset < 8U) {
    return false;
  }
  const auto size = static_cast<std::size_t>(load_u32(bytes, offset + 4U));
  payload_begin = offset + 8U;
  if (size > enclosing_end - payload_begin) return false;
  payload_end = payload_begin + size;
  return true;
}

bool advance_aligned(const std::size_t payload_end,
                     const std::size_t enclosing_end,
                     std::size_t& next) noexcept {
  if (payload_end > std::numeric_limits<std::size_t>::max() - 3U) return false;
  next = (payload_end + 3U) & ~std::size_t{3U};
  return next <= enclosing_end;
}

bool read_stream_bytes(IStorage* storage, const wchar_t* name,
                       const char* display_name, const std::uint32_t minimum_size,
                       const std::uint32_t maximum_size, ComPtr<IStream>& stream,
                       std::vector<std::uint8_t>& bytes, std::string& error,
                       const bool write) {
  const DWORD mode = (write ? STGM_READWRITE : STGM_READ) |
                     STGM_SHARE_EXCLUSIVE;
  if (FAILED(storage->OpenStream(name, nullptr, mode, 0U, stream.put()))) {
    error = std::string("Cannot open native ") + display_name + " stream";
    return false;
  }
  STATSTG stat{};
  if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.HighPart != 0U ||
      stat.cbSize.LowPart < minimum_size ||
      stat.cbSize.LowPart > maximum_size) {
    error = std::string("Native ") + display_name +
            " stream has an invalid size";
    return false;
  }
  bytes.resize(stat.cbSize.LowPart);
  LARGE_INTEGER beginning{};
  if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr))) {
    error = std::string("Cannot seek native ") + display_name + " stream";
    return false;
  }
  ULONG read{};
  if (FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read)) ||
      read != bytes.size()) {
    error = std::string("Cannot read native ") + display_name + " stream";
    return false;
  }
  return true;
}

bool skip_u32_vector(const std::span<const std::uint8_t> bytes,
                     const std::size_t end, std::size_t& cursor) noexcept {
  if (cursor > end || end - cursor < sizeof(std::uint32_t)) return false;
  const auto count = static_cast<std::size_t>(load_u32(bytes, cursor));
  cursor += sizeof(std::uint32_t);
  if (count > (end - cursor) / sizeof(std::uint32_t)) return false;
  cursor += count * sizeof(std::uint32_t);
  return true;
}

bool edit_native_campaign_result_latch(
    const std::span<const std::uint8_t> view,
    const std::span<std::uint8_t> mutable_bytes,
    const std::size_t globals_begin, const std::size_t globals_end,
    const bool write, const bool verify_edits, std::string& error) {
  if (globals_end > view.size() || globals_begin > globals_end ||
      globals_end - globals_begin < kGlobalsPrefixBeforeFirstVector) {
    error = "Native GLBL campaign state is truncated";
    return false;
  }
  std::size_t cursor = globals_begin + kGlobalsPrefixBeforeFirstVector;
  if (!skip_u32_vector(view, globals_end, cursor) ||
      cursor > globals_end ||
      kGlobalsFieldsBetweenVectors > globals_end - cursor) {
    error = "Native GLBL first vector is malformed";
    return false;
  }
  cursor += kGlobalsFieldsBetweenVectors;
  if (!skip_u32_vector(view, globals_end, cursor) ||
      cursor > globals_end ||
      globals_end - cursor != kGlobalsTailAfterSecondVector) {
    error = "Native GLBL exact-Steam tail is malformed";
    return false;
  }
  // The tail is: an unrelated double, campaign threshold, two DWORDs and an
  // unrelated double.  Decode it from the two validated variable-length
  // vectors rather than relying on an absolute XAI/file offset.
  const auto threshold_offset = cursor + sizeof(std::uint64_t);
  const auto threshold = load_u64(view, threshold_offset);
  if (threshold != kInitialCampaignResultThreshold &&
      threshold != kAcknowledgedCampaignResultThreshold) {
    error = "Native GLBL campaign-result threshold is unsupported";
    return false;
  }
  if (write) {
    store_u64(mutable_bytes, threshold_offset,
              kAcknowledgedCampaignResultThreshold);
  } else if (verify_edits &&
             threshold != kAcknowledgedCampaignResultThreshold) {
    error = "Native GLBL campaign-result latch failed verification";
    return false;
  }
  return true;
}

bool edit_native_vehicle(IStorage* storage, const bool write,
                         const bool verify_edits,
                         const std::uint16_t expected_selector,
                         std::uint32_t& vehicle_object_id,
                         std::string& error) {
  ComPtr<IStream> stream;
  std::vector<std::uint8_t> bytes;
  if (!read_stream_bytes(storage, L"XAI", "XAI", 16U, kMaximumXaiSize,
                         stream, bytes, error, write)) {
    return false;
  }
  std::span<std::uint8_t> mutable_bytes(bytes);
  const std::span<const std::uint8_t> view(bytes);
  std::size_t total_begin{};
  std::size_t total_end{};
  if (!tag_equals(view, 0U, kTotalTag) ||
      !chunk_bounds(view, 0U, view.size(), total_begin, total_end) ||
      total_begin != 8U || total_end != view.size()) {
    error = "Native XAI stream has an invalid TOTL container";
    return false;
  }

  std::size_t players_begin{};
  std::size_t players_end{};
  std::size_t globals_begin{};
  std::size_t globals_end{};
  std::uint32_t players_chunks{};
  std::uint32_t globals_chunks{};
  for (std::size_t cursor = total_begin; cursor < total_end;) {
    std::size_t payload_begin{};
    std::size_t payload_end{};
    if (!chunk_bounds(view, cursor, total_end, payload_begin, payload_end)) {
      error = "Native XAI top-level chunks are malformed";
      return false;
    }
    if (tag_equals(view, cursor, kPlayersTag)) {
      players_begin = payload_begin;
      players_end = payload_end;
      ++players_chunks;
    } else if (tag_equals(view, cursor, kGlobalsTag)) {
      globals_begin = payload_begin;
      globals_end = payload_end;
      ++globals_chunks;
    }
    if (payload_end == total_end) break;
    if (!advance_aligned(payload_end, total_end, cursor)) {
      error = "Native XAI top-level alignment is malformed";
      return false;
    }
  }
  if (players_chunks != 1U || globals_chunks != 1U) {
    error = "Native XAI stream must contain one GLBL and one PLRS container";
    return false;
  }
  if (!edit_native_campaign_result_latch(
          view, mutable_bytes, globals_begin, globals_end, write,
          verify_edits, error)) {
    return false;
  }

  std::uint32_t local_players{};
  for (std::size_t cursor = players_begin; cursor < players_end;) {
    std::size_t player_begin{};
    std::size_t player_end{};
    if (!tag_equals(view, cursor, kPlayerTag) ||
        !chunk_bounds(view, cursor, players_end, player_begin, player_end)) {
      error = "Native XAI PLRS entries are malformed";
      return false;
    }
    const auto available = player_end - player_begin;
    const bool local = available >= kLocalPlayerName.size() &&
        std::memcmp(view.data() + player_begin, kLocalPlayerName.data(),
                    kLocalPlayerName.size()) == 0;
    if (local) {
      ++local_players;
      const auto player_data = player_begin + kLocalPlayerName.size();
      std::size_t data_begin{};
      std::size_t data_end{};
      if (!tag_equals(view, player_data, kPlayerDataTag) ||
          !chunk_bounds(view, player_data, player_end, data_begin, data_end)) {
        error = "Native local player has no valid PLYD container";
        return false;
      }
      std::size_t vehicle_begin{};
      std::size_t vehicle_end{};
      if (!tag_equals(view, data_begin, kPlayerVehicleTag) ||
          !chunk_bounds(view, data_begin, data_end, vehicle_begin, vehicle_end) ||
          vehicle_end - vehicle_begin != kPlayerVehiclePayloadSize ||
          vehicle_end - vehicle_begin < 12U) {
        error = "Native local player has no exact-Steam PVEH record";
        return false;
      }
      const auto first = load_u32(view, vehicle_begin);
      const auto second = load_u32(view, vehicle_begin + 4U);
      const auto object_id = load_u32(view, vehicle_begin + 8U);
      if (first != second || first > UINT16_MAX) {
        error = "Native local PVEH model selectors are inconsistent";
        return false;
      }
      if (object_id == kAnmNullObject || object_id == kAnmEndPage) {
        error = "Native local PVEH object id is invalid";
        return false;
      }
      vehicle_object_id = object_id;
      if (write) {
        store_u32(mutable_bytes, vehicle_begin, expected_selector);
        store_u32(mutable_bytes, vehicle_begin + 4U, expected_selector);
      } else if (verify_edits && first != expected_selector) {
        error = "Native local PVEH model selector failed verification";
        return false;
      }
    }
    if (player_end == players_end) break;
    if (!advance_aligned(player_end, players_end, cursor)) {
      error = "Native XAI player alignment is malformed";
      return false;
    }
  }
  if (local_players != 1U) {
    error = "Native XAI stream must contain exactly one local player";
    return false;
  }
  if (!write) return true;

  LARGE_INTEGER beginning{};
  if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr))) {
    error = "Cannot seek native XAI stream for update";
    return false;
  }
  ULONG written{};
  if (FAILED(stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()),
                           &written)) ||
      written != bytes.size() || FAILED(stream->Commit(STGC_DEFAULT))) {
    error = "Cannot commit native PVEH model selector";
    return false;
  }
  return true;
}

bool room_signature_at(const std::span<const std::uint8_t> payload,
                       const std::size_t offset) noexcept {
  constexpr std::array<std::uint8_t, 5> prefix{'r', 'o', 'o', 'm', '_'};
  if (offset > payload.size() || prefix.size() > payload.size() - offset) {
    return false;
  }
  return std::equal(prefix.begin(), prefix.end(), payload.begin() + offset);
}

bool edit_native_anm_vehicle(IStorage* storage, const bool write,
                             const bool verify_edits,
                             const std::uint32_t vehicle_object_id,
                             const std::uint8_t expected_vehicle_tech_index,
                             std::string& error) {
  ComPtr<IStream> stream;
  std::vector<std::uint8_t> bytes;
  if (!read_stream_bytes(storage, L"anm", "anm", 8U, kMaximumAnmSize,
                         stream, bytes, error, write)) {
    return false;
  }
  std::span<std::uint8_t> mutable_bytes(bytes);
  const std::span<const std::uint8_t> view(bytes);
  std::size_t cursor{};
  std::uint32_t matching_objects{};

  const auto take_u32 = [&view, &cursor](std::uint32_t& value) noexcept {
    if (cursor > view.size() || sizeof(value) > view.size() - cursor) {
      return false;
    }
    value = load_u32(view, cursor);
    cursor += sizeof(value);
    return true;
  };

  for (std::uint32_t page = 0U; page < kAnmPageCount; ++page) {
    std::uint32_t populated{};
    if (!take_u32(populated) || populated > 1U) {
      error = "Native anm page table is malformed";
      return false;
    }
    if (populated != 0U) {
      for (std::uint32_t slot = 0U; slot < kAnmSlotsPerPage; ++slot) {
        std::uint32_t object_id{};
        if (!take_u32(object_id)) {
          error = "Native anm object table is truncated";
          return false;
        }
        if (object_id == kAnmNullObject) continue;
        if (object_id != page * kAnmObjectIdStride + slot) {
          error = "Native anm object id does not match its registry slot";
          return false;
        }
        std::uint32_t class_name_size{};
        if (!take_u32(class_name_size) || class_name_size < 2U ||
            class_name_size > 256U || cursor > view.size() ||
            class_name_size > view.size() - cursor ||
            view[cursor + class_name_size - 1U] != 0U) {
          error = "Native anm object class name is malformed";
          return false;
        }
        cursor += class_name_size;
        std::uint32_t payload_size{};
        if (!take_u32(payload_size) || cursor > view.size() ||
            payload_size > view.size() - cursor) {
          error = "Native anm object payload is malformed";
          return false;
        }
        const auto payload_begin = cursor;
        const std::span<const std::uint8_t> payload(
            view.data() + payload_begin, payload_size);
        if (object_id == vehicle_object_id) {
          ++matching_objects;
          if (payload_size != kExactSteamVehiclePayloadSize ||
              !room_signature_at(payload, kExactSteamVehicleRoomOffset)) {
            error = "Native anm local vehicle record has an unsupported schema";
            return false;
          }
          const auto current_type =
              load_u32(payload, kExactSteamVehicleTypeOffset);
          if (current_type >= ht2mp::protocol::steam_vehicle_catalog().size()) {
            error = "Native anm local vehicle type is unsupported";
            return false;
          }
          if (write) {
            store_u32(mutable_bytes,
                      payload_begin + kExactSteamVehicleTypeOffset,
                      expected_vehicle_tech_index);
          } else if (verify_edits &&
                     current_type != expected_vehicle_tech_index) {
            error = "Native anm local vehicle type failed verification";
            return false;
          }
        }
        cursor += payload_size;
      }
    }
    std::uint32_t end_page{};
    if (!take_u32(end_page) || end_page != kAnmEndPage) {
      error = "Native anm page terminator is malformed";
      return false;
    }
  }
  if (cursor != view.size()) {
    error = "Native anm stream has trailing data";
    return false;
  }
  if (matching_objects != 1U) {
    error = "Native anm stream must contain exactly one local vehicle object";
    return false;
  }
  if (!write) return true;

  LARGE_INTEGER beginning{};
  if (FAILED(stream->Seek(beginning, STREAM_SEEK_SET, nullptr))) {
    error = "Cannot seek native anm stream for update";
    return false;
  }
  ULONG written{};
  if (FAILED(stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()),
                           &written)) ||
      written != bytes.size() || FAILED(stream->Commit(STGC_DEFAULT))) {
    error = "Cannot commit native anm vehicle type";
    return false;
  }
  return true;
}

bool write_record(IStorage* storage, const AppearanceRecord& record,
                  std::string& error) {
  ComPtr<IStream> stream;
  const auto opened = storage->CreateStream(
      L"HT2MP.Appearance", STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
      0U, 0U, stream.put());
  if (FAILED(opened)) {
    error = "Cannot create appearance stream (HRESULT " +
            std::to_string(static_cast<unsigned long>(opened)) + ')';
    return false;
  }
  ULONG written{};
  if (FAILED(stream->Write(&record, sizeof(record), &written)) ||
      written != sizeof(record) || FAILED(stream->Commit(STGC_DEFAULT))) {
    error = "Cannot commit appearance stream";
    return false;
  }
  return true;
}

bool read_record(IStorage* storage, const std::uint16_t selector,
                 const std::uint8_t paint) noexcept {
  ComPtr<IStream> stream;
  if (FAILED(storage->OpenStream(L"HT2MP.Appearance", nullptr,
                                 STGM_READ | STGM_SHARE_EXCLUSIVE, 0U,
                                 stream.put()))) {
    return false;
  }
  AppearanceRecord record{};
  ULONG read{};
  return SUCCEEDED(stream->Read(&record, sizeof(record), &read)) &&
         read == sizeof(record) && valid_record(record, selector, paint);
}

bool visit_complete_slots(IStorage* root, const bool write,
                          const AppearanceRecord& record,
                          const std::uint16_t native_selector,
                          const std::uint8_t vehicle_tech_index,
                          std::uint32_t& count, std::string& error) {
  count = 0U;
  ComPtr<IStorage> games;
  const DWORD mode = (write ? STGM_READWRITE : STGM_READ) |
                     STGM_SHARE_EXCLUSIVE;
  if (FAILED(root->OpenStorage(L"listofgames", nullptr, mode, nullptr, 0U,
                               games.put()))) {
    error = "Profile has no listofgames storage";
    return false;
  }
  ComPtr<IEnumSTATSTG> enumerator;
  if (FAILED(games->EnumElements(0U, nullptr, 0U, enumerator.put()))) {
    error = "Cannot enumerate saved-game slots";
    return false;
  }
  for (;;) {
    STATSTG stat{};
    ULONG fetched{};
    const auto next = enumerator->Next(1U, &stat, &fetched);
    if (next == S_FALSE || fetched == 0U) break;
    if (FAILED(next)) {
      error = "Cannot enumerate saved-game slots";
      return false;
    }
    const std::wstring name = stat.pwcsName == nullptr ? L"" : stat.pwcsName;
    CoTaskMemFree(stat.pwcsName);
    if (stat.type != STGTY_STORAGE || name.empty()) continue;
    ComPtr<IStorage> slot;
    if (FAILED(games->OpenStorage(name.c_str(), nullptr, mode, nullptr, 0U,
                                  slot.put())) ||
        !complete_slot(slot.get())) {
      continue;
    }
    std::uint32_t vehicle_object_id{};
    if (write) {
      if (!edit_native_vehicle(slot.get(), true, true, native_selector,
                               vehicle_object_id, error) ||
          !edit_native_anm_vehicle(slot.get(), true, true, vehicle_object_id,
                                   vehicle_tech_index, error) ||
          !write_record(slot.get(), record, error) ||
          FAILED(slot->Commit(STGC_DEFAULT))) {
        return false;
      }
    } else if (!edit_native_vehicle(slot.get(), false, true, native_selector,
                                    vehicle_object_id, error) ||
               !edit_native_anm_vehicle(slot.get(), false, true,
                                        vehicle_object_id,
                                        vehicle_tech_index, error) ||
               !read_record(slot.get(), record.selector, record.paint)) {
      if (error.empty()) {
        error = "A completed slot has an invalid appearance record";
      }
      return false;
    }
    ++count;
  }
  if (count == 0U) {
    error = "Profile has no completed saved-game slot";
    return false;
  }
  if (write && FAILED(games->Commit(STGC_DEFAULT))) {
    error = "Cannot commit listofgames storage";
    return false;
  }
  return true;
}

bool open_storage(const fs::path& path, const bool write,
                  ComPtr<IStorage>& storage, std::string& error) {
  if (StgIsStorageFile(path.c_str()) != S_OK) {
    error = "Driver template is not a Windows Structured Storage profile";
    return false;
  }
  const DWORD mode = (write ? STGM_READWRITE | STGM_TRANSACTED : STGM_READ) |
                     STGM_SHARE_EXCLUSIVE;
  const auto opened = StgOpenStorage(path.c_str(), nullptr, mode, nullptr, 0U,
                                     storage.put());
  if (FAILED(opened)) {
    error = "Cannot open driver profile storage";
    return false;
  }
  if (!valid_profile_header(storage.get())) {
    error = "Driver profile is not exact Steam save format 4/settings 5";
    return false;
  }
  return true;
}

bool validate_profile_structure(const fs::path& path, std::string& error) {
  ComPtr<IStorage> storage;
  if (!open_storage(path, false, storage, error)) return false;
  std::uint32_t count{};
  // Here only the game slot signature matters; no marker exists in a fresh
  // distribution template yet.
  ComPtr<IStorage> games;
  if (FAILED(storage->OpenStorage(L"listofgames", nullptr,
                                  STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr,
                                  0U, games.put()))) {
    error = "Profile has no listofgames storage";
    return false;
  }
  ComPtr<IEnumSTATSTG> enumerator;
  if (FAILED(games->EnumElements(0U, nullptr, 0U, enumerator.put()))) return false;
  for (;;) {
    STATSTG stat{};
    ULONG fetched{};
    if (enumerator->Next(1U, &stat, &fetched) != S_OK || fetched == 0U) break;
    const std::wstring name = stat.pwcsName == nullptr ? L"" : stat.pwcsName;
    CoTaskMemFree(stat.pwcsName);
    if (stat.type != STGTY_STORAGE) continue;
    ComPtr<IStorage> slot;
    if (SUCCEEDED(games->OpenStorage(name.c_str(), nullptr,
                                     STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr,
                                     0U, slot.put())) &&
        complete_slot(slot.get())) {
      // A complete-looking slot is accepted only if the exact native local
      // player/PVEH structure can be found.  This protects the source
      // template and backup recovery from best-effort byte patching.
      std::uint32_t vehicle_object_id{};
      if (!edit_native_vehicle(slot.get(), false, false, 0U,
                               vehicle_object_id, error) ||
          !edit_native_anm_vehicle(slot.get(), false, false,
                                   vehicle_object_id, 0U, error)) {
        return false;
      }
      ++count;
    }
  }
  if (count == 0U) error = "Profile has no completed saved-game slot";
  return count != 0U;
}

class ComApartment final {
 public:
  ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComApartment() {
    if (result_ == S_OK || result_ == S_FALSE) CoUninitialize();
  }
  [[nodiscard]] bool usable() const noexcept {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_{};
};

}  // namespace

bool prepare_online_profile(const fs::path& template_profile,
                            const fs::path& runtime_directory,
                            const std::uint16_t vehicle_selector,
                            const std::uint8_t paint_variant,
                            ProfileEditResult& result, std::string& error) {
  result = {};
  error.clear();
  if (!ht2mp::protocol::is_allowed_steam_vehicle(vehicle_selector) ||
      paint_variant >= ht2mp::protocol::kPaintVariantCount) {
    error = "Appearance is outside the exact Steam allowlist";
    return false;
  }
  ht2mp::protocol::SteamNativeVehicleAppearance native_appearance;
  if (!ht2mp::protocol::to_steam_native_vehicle(
          vehicle_selector, paint_variant, native_appearance)) {
    error = "Selected paint is not available for this exact-Steam vehicle";
    return false;
  }
  if (king_process_running()) {
    error = "Refusing to edit the online driver profile while king.exe is running";
    return false;
  }
  const ComApartment apartment;
  if (!apartment.usable()) {
    error = "Cannot initialize Windows Structured Storage";
    return false;
  }
  std::error_code ec;
  if (!fs::is_directory(runtime_directory, ec) || ec) {
    error = "Isolated runtime directory does not exist";
    return false;
  }
  const auto destination = runtime_directory / kOnlineDriverFilename;
  const auto backup = runtime_directory / kOnlineDriverBackupFilename;
  const auto temporary = runtime_directory /
      (std::wstring(kOnlineDriverFilename) + L".tmp-" +
       std::to_wstring(GetCurrentProcessId()));
  fs::path base = template_profile;
  std::string validation_error;
  if (fs::is_regular_file(destination, ec) && !ec &&
      validate_profile_structure(destination, validation_error)) {
    base = destination;
    if (!CopyFileW(destination.c_str(), backup.c_str(), FALSE)) {
      error = "Cannot preserve the last good profile backup: " +
              ht2mp::windows::win32_error(GetLastError());
      return false;
    }
  } else if (fs::is_regular_file(backup, ec) && !ec &&
             validate_profile_structure(backup, validation_error)) {
    base = backup;
  } else if (!validate_profile_structure(template_profile, error)) {
    error = "Invalid HT2MP driver template: " + error;
    return false;
  }
  DeleteFileW(temporary.c_str());
  if (!CopyFileW(base.c_str(), temporary.c_str(), TRUE)) {
    error = "Cannot create private profile copy: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  const auto record = make_record(vehicle_selector, paint_variant);
  {
    ComPtr<IStorage> storage;
    if (!open_storage(temporary, true, storage, error) ||
        !write_record(storage.get(), record, error) ||
        !visit_complete_slots(storage.get(), true, record,
                              native_appearance.selector,
                              native_appearance.vehicle_tech_index,
                              result.completed_slots, error) ||
        FAILED(storage->Commit(STGC_OVERWRITE))) {
      DeleteFileW(temporary.c_str());
      if (error.empty()) error = "Cannot commit edited driver profile";
      return false;
    }
  }
  {
    ComPtr<IStorage> storage;
    std::uint32_t verified_slots{};
    if (!open_storage(temporary, false, storage, error) ||
        !read_record(storage.get(), vehicle_selector, paint_variant) ||
        !visit_complete_slots(storage.get(), false, record,
                              native_appearance.selector,
                              native_appearance.vehicle_tech_index,
                              verified_slots, error) ||
        verified_slots != result.completed_slots) {
      DeleteFileW(temporary.c_str());
      if (error.empty()) error = "Edited driver profile failed verification";
      return false;
    }
  }
  if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "Cannot atomically replace the online driver profile: " +
            ht2mp::windows::win32_error(GetLastError());
    DeleteFileW(temporary.c_str());
    return false;
  }
  // Releases before this fix wrote a second driver named HT2MP.pl1.  Native
  // auto-enter deliberately refuses ambiguous stages, so remove that exact
  // app-owned legacy file only after the new profile has been committed.
  const auto legacy = runtime_directory / kLegacyOnlineDriverFilename;
  if (fs::is_regular_file(legacy, ec) && !ec &&
      !DeleteFileW(legacy.c_str())) {
    error = "Cannot remove the obsolete HT2MP.pl1 profile: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  result.destination = destination;
  result.backup = backup;
  return true;
}

bool write_last_player(const fs::path& truck_ini, std::string& error) {
  error.clear();
  std::ifstream input(truck_ini, std::ios::binary);
  if (!input) {
    error = "Cannot open staged TRUCK.INI";
    return false;
  }
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  input.close();
  bool replaced{};
  std::size_t begin{};
  while (begin <= contents.size()) {
    const auto end = contents.find_first_of("\r\n", begin);
    const auto length = (end == std::string::npos ? contents.size() : end) - begin;
    const std::string_view line(contents.data() + begin, length);
    if (line.size() >= 6U &&
        std::equal(line.begin(), line.begin() + 6U, "lastp=",
                   [](const char left, const char right) {
                     return std::tolower(static_cast<unsigned char>(left)) ==
                            std::tolower(static_cast<unsigned char>(right));
                   })) {
      contents.replace(begin, length, kLastPlayerLine);
      replaced = true;
      break;
    }
    if (end == std::string::npos) break;
    begin = end + 1U;
    if (contents[end] == '\r' && begin < contents.size() &&
        contents[begin] == '\n') ++begin;
  }
  if (!replaced) {
    error = "TRUCK.INI has no lastp setting";
    return false;
  }
  const auto temporary = truck_ini.wstring() + L".tmp";
  std::ofstream output(fs::path(temporary), std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  if (!output || !MoveFileExW(temporary.c_str(), truck_ini.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = "Cannot update staged TRUCK.INI";
    DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

}  // namespace ht2mp::launcher
