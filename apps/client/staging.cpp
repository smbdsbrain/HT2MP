#include "staging.hpp"

#include "ht2mp/windows/runtime.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace ht2mp::client {
namespace fs = std::filesystem;
namespace {

std::wstring lowercase(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t ch) {
    return static_cast<wchar_t>(towlower(ch));
  });
  return value;
}

bool is_reparse_point(const fs::path& path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
}

bool excluded_directory(const fs::path& relative) {
  for (const auto& part : relative) {
    const auto name = lowercase(part.wstring());
    if (name == L"screenshots" || name == L"screenshot" || name == L"logs" ||
        name == L"crashdumps" || name == L"crash dumps") {
      return true;
    }
  }
  return false;
}

bool excluded_file(const fs::path& relative) {
  if (excluded_directory(relative.parent_path())) {
    return true;
  }
  const auto extension = lowercase(relative.extension().wstring());
  if (extension == L".pl1" || extension == L".log" || extension == L".dmp" ||
      extension == L".mdmp") {
    return true;
  }
  const auto stem = lowercase(relative.stem().wstring());
  const auto image = extension == L".bmp" || extension == L".png" ||
                     extension == L".jpg" || extension == L".jpeg";
  return image && (stem.starts_with(L"screenshot") || stem.starts_with(L"scrshot") ||
                   stem.starts_with(L"shot_"));
}

std::string json_escape(const std::string_view input) {
  std::ostringstream output;
  for (const unsigned char ch : input) {
    switch (ch) {
    case '\\': output << "\\\\"; break;
    case '"': output << "\\\""; break;
    case '\b': output << "\\b"; break;
    case '\f': output << "\\f"; break;
    case '\n': output << "\\n"; break;
    case '\r': output << "\\r"; break;
    case '\t': output << "\\t"; break;
    default:
      if (ch < 0x20U) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
               << static_cast<unsigned>(ch) << std::dec;
      } else {
        output << static_cast<char>(ch);
      }
    }
  }
  return output.str();
}

struct StageManifest final {
  std::uint32_t schema{};
  std::string profile;
  std::string instance;
  std::string king_sha256;
  std::string source;
  std::string save_policy;
  bool has_instance{};
};

class ManifestParser final {
public:
  explicit ManifestParser(const std::string_view input) noexcept : input_(input) {}

  [[nodiscard]] bool parse(StageManifest& manifest, std::string& error) {
    skip_whitespace();
    if (!consume('{')) {
      return fail("expected a JSON object", error);
    }

    bool have_schema{};
    bool have_profile{};
    bool have_instance{};
    bool have_hash{};
    bool have_source{};
    bool have_save_policy{};
    skip_whitespace();
    if (consume('}')) {
      return fail("manifest object is empty", error);
    }
    for (;;) {
      std::string key;
      if (!parse_string(key, error)) {
        return false;
      }
      skip_whitespace();
      if (!consume(':')) {
        return fail("expected ':' after a manifest key", error);
      }
      skip_whitespace();

      if (key == "schema") {
        if (have_schema) return fail("duplicate 'schema' key", error);
        have_schema = true;
        if (!parse_u32(manifest.schema, error)) return false;
      } else {
        std::string* destination{};
        bool* present{};
        if (key == "profile") {
          destination = &manifest.profile;
          present = &have_profile;
        } else if (key == "instance") {
          destination = &manifest.instance;
          present = &have_instance;
        } else if (key == "king_sha256") {
          destination = &manifest.king_sha256;
          present = &have_hash;
        } else if (key == "source") {
          destination = &manifest.source;
          present = &have_source;
        } else if (key == "save_policy") {
          destination = &manifest.save_policy;
          present = &have_save_policy;
        } else {
          return fail("unknown manifest key '" + key + "'", error);
        }
        if (*present) return fail("duplicate '" + key + "' key", error);
        *present = true;
        if (!parse_string(*destination, error)) return false;
      }

      skip_whitespace();
      if (consume('}')) break;
      if (!consume(',')) {
        return fail("expected ',' or '}' in manifest", error);
      }
      skip_whitespace();
    }
    skip_whitespace();
    if (position_ != input_.size()) {
      return fail("trailing data after manifest object", error);
    }
    if (!have_schema || !have_profile || !have_hash || !have_source ||
        !have_save_policy) {
      return fail("manifest is missing one or more required keys", error);
    }
    manifest.has_instance = have_instance;
    return true;
  }

private:
  void skip_whitespace() noexcept {
    while (position_ < input_.size()) {
      const auto ch = input_[position_];
      if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
      ++position_;
    }
  }

  bool consume(const char expected) noexcept {
    if (position_ >= input_.size() || input_[position_] != expected) return false;
    ++position_;
    return true;
  }

  [[nodiscard]] bool parse_string(std::string& output, std::string& error) {
    output.clear();
    if (!consume('"')) return fail("expected a JSON string", error);
    while (position_ < input_.size()) {
      const auto ch = static_cast<unsigned char>(input_[position_++]);
      if (ch == '"') return true;
      if (ch < 0x20U) return fail("unescaped control byte in JSON string", error);
      if (ch != '\\') {
        output.push_back(static_cast<char>(ch));
      } else {
        if (position_ >= input_.size()) return fail("unterminated JSON escape", error);
        const auto escaped = input_[position_++];
        switch (escaped) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        default:
          // json_escape() never emits \u for a valid Windows path.  Rejecting
          // it keeps this security-sensitive parser small and unambiguous.
          return fail("unsupported JSON escape in manifest", error);
        }
      }
      if (output.size() > 32U * 1024U) {
        return fail("manifest string is too large", error);
      }
    }
    return fail("unterminated JSON string", error);
  }

  [[nodiscard]] bool parse_u32(std::uint32_t& output, std::string& error) {
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return fail("expected an unsigned schema number", error);
    }
    std::uint32_t value{};
    do {
      const auto digit = static_cast<std::uint32_t>(input_[position_] - '0');
      if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
        return fail("schema number is out of range", error);
      }
      value = value * 10U + digit;
      ++position_;
    } while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9');
    output = value;
    return true;
  }

  [[nodiscard]] bool fail(const std::string& detail, std::string& error) const {
    error = "Invalid staging manifest at byte " + std::to_string(position_) +
            ": " + detail;
    return false;
  }

  std::string_view input_;
  std::size_t position_{};
};

bool paths_equal_lexically(const fs::path& lhs, const fs::path& rhs,
                           std::string& error) {
  std::error_code ec;
  const auto left = fs::absolute(lhs, ec).lexically_normal();
  if (ec) {
    error = "Cannot make first path absolute: " + ec.message();
    return false;
  }
  const auto right = fs::absolute(rhs, ec).lexically_normal();
  if (ec) {
    error = "Cannot make second path absolute: " + ec.message();
    return false;
  }
  auto left_it = left.begin();
  auto right_it = right.begin();
  for (; left_it != left.end() && right_it != right.end(); ++left_it, ++right_it) {
    if (lowercase(left_it->wstring()) != lowercase(right_it->wstring())) return false;
  }
  return left_it == left.end() && right_it == right.end();
}

bool verify_no_reparse_chain(const fs::path& path, std::string& error) {
  std::error_code ec;
  const auto absolute = fs::absolute(path, ec).lexically_normal();
  if (ec || !absolute.is_absolute()) {
    error = "Cannot normalize runtime path for reparse validation";
    if (ec) error += ": " + ec.message();
    return false;
  }

  fs::path current;
  for (const auto& part : absolute) {
    current /= part;
    const auto attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      error = "Cannot inspect runtime path component '" + current.string() +
              "': " + ht2mp::windows::win32_error(GetLastError());
      return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      error = "Runtime path contains a reparse point/junction: " + current.string();
      return false;
    }
  }
  return true;
}

bool verify_no_reparse_tree(const fs::path& root, std::string& error) {
  if (!verify_no_reparse_chain(root, error)) return false;
  std::error_code ec;
  fs::recursive_directory_iterator iterator(root, fs::directory_options::none, ec);
  const fs::recursive_directory_iterator end;
  if (ec) {
    error = "Cannot enumerate staged runtime for reparse validation: " + ec.message();
    return false;
  }
  while (iterator != end) {
    const auto attributes = GetFileAttributesW(iterator->path().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      error = "Cannot inspect staged runtime entry '" + iterator->path().string() +
              "': " + ht2mp::windows::win32_error(GetLastError());
      return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        iterator.disable_recursion_pending();
      }
      error = "Staged runtime contains a reparse point/junction: " +
              iterator->path().string();
      return false;
    }
    iterator.increment(ec);
    if (ec) {
      error = "Cannot enumerate staged runtime for reparse validation: " + ec.message();
      return false;
    }
  }
  return true;
}

bool read_manifest(const fs::path& path, StageManifest& manifest,
                   std::string& error) {
  constexpr std::uintmax_t kMaximumManifestBytes = 64U * 1024U;
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec) {
    error = "Staging manifest is not a regular file: " + path.string();
    return false;
  }
  const auto size = fs::file_size(path, ec);
  if (ec || size == 0U || size > kMaximumManifestBytes) {
    error = "Staging manifest has an invalid size";
    if (ec) error += ": " + ec.message();
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "Cannot open staging manifest: " + path.string();
    return false;
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input || input.peek() != std::char_traits<char>::eof()) {
    error = "Cannot read staging manifest atomically: " + path.string();
    return false;
  }
  return ManifestParser(bytes).parse(manifest, error);
}

bool remove_runtime_tree(const fs::path& runtime_root, const fs::path& target,
                         std::string& error) {
  std::string containment_error;
  std::error_code identity_error;
  const bool same_path = fs::equivalent(runtime_root, target, identity_error);
  if (!ht2mp::windows::path_is_within(runtime_root, target, containment_error) ||
      (!identity_error && same_path)) {
    error = "Refusing to remove a path outside the HT2MP runtime root";
    if (!containment_error.empty()) {
      error += ": " + containment_error;
    }
    return false;
  }
  std::error_code ec;
  fs::remove_all(target, ec);
  if (ec) {
    error = "Cannot remove temporary runtime tree: " + ec.message();
    return false;
  }
  return true;
}

bool recover_failed_transaction(const fs::path& runtime_root,
                                const fs::path& incoming,
                                const fs::path& target,
                                const fs::path& backup,
                                const bool target_was_backed_up,
                                std::string& error) {
  error.clear();
  std::string incoming_error;
  std::error_code ec;
  if (fs::exists(incoming, ec) && !ec &&
      !remove_runtime_tree(runtime_root, incoming, incoming_error)) {
    error = "Could not remove incomplete staging tree '" + incoming.string() +
            "': " + incoming_error;
  } else if (ec) {
    error = "Could not inspect incomplete staging tree '" + incoming.string() +
            "': " + ec.message();
  }

  if (!target_was_backed_up) return error.empty();

  ec.clear();
  const bool backup_exists = fs::exists(backup, ec);
  if (ec || !backup_exists) {
    const auto reason = ec ? ec.message() : std::string("backup is missing");
    if (!error.empty()) error += "; ";
    error += "AUTOMATIC RECOVERY FAILED; previous runtime backup '" +
             backup.string() + "' could not be restored: " + reason;
    return false;
  }

  ec.clear();
  const bool target_exists = fs::exists(target, ec);
  if (ec || target_exists) {
    const auto reason = ec ? ec.message() : std::string("destination already exists");
    if (!error.empty()) error += "; ";
    error += "AUTOMATIC RECOVERY FAILED; previous runtime backup remains at '" +
             backup.string() + "': " + reason;
    return false;
  }

  fs::rename(backup, target, ec);
  if (ec) {
    if (!error.empty()) error += "; ";
    error += "AUTOMATIC RECOVERY FAILED; previous runtime backup remains at '" +
             backup.string() + "': " + ec.message();
    return false;
  }
  return error.empty();
}

bool verify_king(const EditionDescriptor& edition, const fs::path& root,
                 std::string& error) {
  const auto king = root / L"king.exe";
  if (!fs::is_regular_file(king)) {
    error = "king.exe was not found in " + root.string();
    return false;
  }
  if (!ht2mp::windows::is_i386_pe(king, error)) {
    error = "Invalid king.exe: " + error;
    return false;
  }
  ht2mp::windows::Sha256 digest{};
  if (!ht2mp::windows::sha256_file(king, digest, error)) {
    return false;
  }
  const auto actual = ht2mp::windows::sha256_hex(digest);
  if (actual != edition.king_sha256) {
    error = "Unsupported king.exe for profile " + std::string(edition.profile_id) +
            "; expected SHA-256 " + std::string(edition.king_sha256) +
            ", got " + actual;
    return false;
  }
  return true;
}

bool verify_protected_modules(const EditionDescriptor& edition,
                              const fs::path& root,
                              const std::string_view label,
                              std::string& error) {
  for (const auto& module : edition.protected_modules) {
    const auto path = root / fs::path(module.filename);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec || is_reparse_point(path)) {
      error = std::string(label) + " protected module is missing, unreadable, "
              "or a reparse point: " + path.string();
      return false;
    }
    ht2mp::windows::Sha256 digest{};
    if (!ht2mp::windows::sha256_file(path, digest, error)) {
      error = std::string(label) + " protected module cannot be hashed: " +
              path.string() + ": " + error;
      return false;
    }
    const auto actual = ht2mp::windows::sha256_hex(digest);
    if (actual != module.sha256) {
      error = std::string(label) + " protected module hash mismatch for " +
              path.filename().string() + "; expected " +
              std::string(module.sha256) + ", got " + actual;
      return false;
    }
  }
  return true;
}

bool verify_protected_module_copies_are_distinct(
    const EditionDescriptor& edition, const fs::path& source,
    const fs::path& runtime, std::string& error) {
  for (const auto& module : edition.protected_modules) {
    const auto source_path = source / fs::path(module.filename);
    const auto runtime_path = runtime / fs::path(module.filename);
    std::string identity_error;
    const bool same_file = ht2mp::windows::same_existing_file(
        source_path, runtime_path, identity_error);
    if (!identity_error.empty()) {
      error = "Cannot compare source/runtime identity for protected module " +
              runtime_path.filename().string() + ": " + identity_error;
      return false;
    }
    if (same_file) {
      error = "Staged protected module aliases the source installation: " +
              runtime_path.filename().string();
      return false;
    }
  }
  return true;
}

} // namespace

bool valid_instance_id(const std::string_view instance) noexcept {
  if (instance.empty()) return true;
  if (instance.size() > 32U) return false;
  const auto lower_alnum = [](const char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
  };
  if (!lower_alnum(instance.front())) return false;
  return std::all_of(instance.begin() + 1, instance.end(),
                     [&](const char ch) noexcept {
                       return lower_alnum(ch) || ch == '_' || ch == '-';
                     });
}

fs::path staged_game_directory(const EditionDescriptor& edition,
                               std::string& error,
                               const std::string_view instance) {
  error.clear();
  if (!valid_instance_id(instance)) {
    error = "Invalid instance id; expected lowercase [a-z0-9][a-z0-9_-]{0,31}";
    return {};
  }
  const auto local = ht2mp::windows::local_app_data(error);
  if (!error.empty()) {
    return {};
  }
  auto runtime_name = std::string(edition.profile_id);
  if (!instance.empty()) {
    runtime_name += "--";
    runtime_name.append(instance);
  }
  return local / L"HT2MP" / L"runtime" / fs::path(runtime_name);
}

bool validate_staged_game(const EditionDescriptor& edition,
                          const fs::path& expected_runtime,
                          StageValidation& validation,
                          std::string& error,
                          const std::string_view instance) {
  validation = {};
  error.clear();

  const auto configured_runtime = staged_game_directory(edition, error, instance);
  if (!error.empty()) return false;
  std::string comparison_error;
  if (!paths_equal_lexically(configured_runtime, expected_runtime,
                             comparison_error)) {
    error = comparison_error.empty()
                ? "Staged runtime path does not match the selected profile: expected '" +
                      configured_runtime.string() + "', got '" +
                      expected_runtime.string() + "'"
                : comparison_error;
    return false;
  }

  std::error_code ec;
  if (!fs::is_directory(expected_runtime, ec) || ec) {
    error = "Staged runtime is not a readable directory: " +
            expected_runtime.string();
    return false;
  }
  if (!verify_no_reparse_tree(expected_runtime, error)) return false;

  std::string identity_error;
  if (!ht2mp::windows::same_existing_file(configured_runtime, expected_runtime,
                                           identity_error)) {
    error = "Staged runtime does not have the expected directory identity: " +
            identity_error;
    return false;
  }

  const auto manifest_path = expected_runtime / L".ht2mp-stage.json";
  StageManifest manifest;
  if (!read_manifest(manifest_path, manifest, error)) return false;
  if (manifest.schema != 1U && manifest.schema != 2U) {
    error = "Unsupported staging manifest schema " +
            std::to_string(manifest.schema) + "; expected schema 1 or 2";
    return false;
  }
  if (manifest.schema == 1U && !instance.empty()) {
    error = "Staging manifest schema 1 is accepted only for the default runtime";
    return false;
  }
  if (manifest.schema == 1U && manifest.has_instance) {
    error = "Staging manifest schema 1 must not contain an instance field";
    return false;
  }
  if (manifest.schema == 2U &&
      (!manifest.has_instance || manifest.instance != instance ||
       !valid_instance_id(manifest.instance))) {
    error = "Staging manifest instance does not match the selected runtime";
    return false;
  }
  if (manifest.profile != edition.profile_id) {
    error = "Staging manifest profile mismatch; expected '" +
            std::string(edition.profile_id) + "', got '" + manifest.profile + "'";
    return false;
  }
  if (manifest.king_sha256 != edition.king_sha256) {
    error = "Staging manifest king_sha256 mismatch for profile " +
            std::string(edition.profile_id);
    return false;
  }
  if (manifest.save_policy != "excluded") {
    error = "Staging manifest save_policy must be 'excluded'";
    return false;
  }

  std::string conversion_error;
  const auto source_wide = ht2mp::windows::widen_utf8(manifest.source,
                                                       conversion_error);
  if (!conversion_error.empty() || source_wide.empty()) {
    error = conversion_error.empty() ? "Staging manifest source is empty"
                                     : "Invalid source path in staging manifest: " +
                                           conversion_error;
    return false;
  }
  fs::path source(source_wide);
  if (!source.is_absolute()) {
    error = "Staging manifest source must be an absolute path";
    return false;
  }
  source = fs::weakly_canonical(source, ec);
  if (ec || !fs::is_directory(source, ec) || ec) {
    error = "The source installation recorded by the staging manifest is not "
            "available: " + fs::path(source_wide).string();
    return false;
  }

  bool same_directory = ht2mp::windows::same_existing_file(
      source, expected_runtime, identity_error);
  if (!identity_error.empty()) {
    error = "Cannot compare source/runtime directory identity: " + identity_error;
    return false;
  }
  if (same_directory) {
    error = "Source installation and staged runtime resolve to the same directory";
    return false;
  }
  std::string source_contains_error;
  const bool runtime_in_source = ht2mp::windows::path_is_within(
      source, expected_runtime, source_contains_error);
  if (!source_contains_error.empty()) {
    error = "Cannot prove source/runtime separation: " + source_contains_error;
    return false;
  }
  std::string runtime_contains_error;
  const bool source_in_runtime = ht2mp::windows::path_is_within(
      expected_runtime, source, runtime_contains_error);
  if (!runtime_contains_error.empty()) {
    error = "Cannot prove source/runtime separation: " + runtime_contains_error;
    return false;
  }
  if (runtime_in_source || source_in_runtime) {
    error = "Source installation and staged runtime must not contain one another";
    return false;
  }

  if (!verify_king(edition, source, error)) {
    error = "Source installation no longer matches its staging manifest: " + error;
    return false;
  }
  if (!verify_protected_modules(edition, source, "Source", error)) {
    return false;
  }
  if (!verify_king(edition, expected_runtime, error)) {
    error = "Staged runtime no longer matches its staging manifest: " + error;
    return false;
  }
  if (!verify_protected_modules(edition, expected_runtime, "Staged", error)) {
    return false;
  }
  const auto source_king = source / L"king.exe";
  const auto runtime_king = expected_runtime / L"king.exe";
  const bool same_king = ht2mp::windows::same_existing_file(
      source_king, runtime_king, identity_error);
  if (!identity_error.empty()) {
    error = "Cannot compare source/runtime king.exe identity: " + identity_error;
    return false;
  }
  if (same_king) {
    error = "Staged king.exe aliases the source installation instead of being a copy";
    return false;
  }
  if (!verify_protected_module_copies_are_distinct(
          edition, source, expected_runtime, error)) {
    return false;
  }

  validation.runtime = fs::weakly_canonical(expected_runtime, ec);
  if (ec) {
    error = "Cannot canonicalize validated runtime: " + ec.message();
    return false;
  }
  validation.source = std::move(source);
  validation.king = validation.runtime / L"king.exe";
  validation.source_king = validation.source / L"king.exe";
  return true;
}

bool seed_staged_driver(const EditionDescriptor& edition,
                        const fs::path& source_save,
                        DriverSeedResult& result,
                        std::string& error,
                        const std::string_view instance) {
  constexpr std::uintmax_t kMaximumDriverBytes = 64U * 1024U * 1024U;
  result = {};
  error.clear();

  std::error_code ec;
  if (!fs::is_regular_file(source_save, ec) || ec ||
      is_reparse_point(source_save)) {
    error = "Driver seed is not a regular non-reparse file: " +
            source_save.string();
    if (ec) error += ": " + ec.message();
    return false;
  }
  if (lowercase(source_save.extension().wstring()) != L".pl1") {
    error = "Driver seed must have the .pl1 extension";
    return false;
  }
  const auto size = fs::file_size(source_save, ec);
  if (ec || size == 0U || size > kMaximumDriverBytes) {
    error = "Driver seed has an invalid size";
    if (ec) error += ": " + ec.message();
    return false;
  }

  const auto canonical_source = fs::canonical(source_save, ec);
  if (ec || !canonical_source.is_absolute()) {
    error = "Cannot canonicalize driver seed: " +
            (ec ? ec.message() : std::string("path is not absolute"));
    return false;
  }

  const auto runtime = staged_game_directory(edition, error, instance);
  if (!error.empty()) return false;
  StageValidation validation;
  if (!validate_staged_game(edition, runtime, validation, error, instance)) {
    error = "Cannot seed an unvalidated staged runtime: " + error;
    return false;
  }

  std::string containment_error;
  const bool source_inside_runtime = ht2mp::windows::path_is_within(
      validation.runtime, canonical_source, containment_error);
  if (!containment_error.empty()) {
    error = "Cannot prove driver/runtime separation: " + containment_error;
    return false;
  }
  if (source_inside_runtime) {
    error = "Driver seed must be outside the staged runtime";
    return false;
  }

  std::uint32_t staged_save_count{};
  fs::directory_iterator iterator(validation.runtime, ec);
  const fs::directory_iterator end;
  if (ec) {
    error = "Cannot enumerate staged driver profiles: " + ec.message();
    return false;
  }
  for (; iterator != end; iterator.increment(ec)) {
    if (ec) {
      error = "Cannot enumerate staged driver profiles: " + ec.message();
      return false;
    }
    std::error_code type_error;
    if (iterator->is_regular_file(type_error) && !type_error &&
        lowercase(iterator->path().extension().wstring()) == L".pl1") {
      ++staged_save_count;
    }
  }
  if (staged_save_count != 0U) {
    error = "Staged runtime already contains a .pl1 driver; refusing to overwrite it";
    return false;
  }

  const auto destination = validation.runtime / canonical_source.filename();
  const auto temporary = validation.runtime /
      (L".ht2mp-driver-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
       std::to_wstring(GetTickCount64()) + L".incoming");
  const auto cleanup_temporary = [&]() {
    std::error_code ignored;
    fs::remove(temporary, ignored);
  };

  ht2mp::windows::Sha256 source_digest{};
  if (!ht2mp::windows::sha256_file(canonical_source, source_digest, error)) {
    error = "Cannot hash driver seed: " + error;
    return false;
  }

  fs::copy_file(canonical_source, temporary, fs::copy_options::none, ec);
  if (ec) {
    cleanup_temporary();
    error = "Cannot copy driver seed into staged runtime: " + ec.message();
    return false;
  }

  ht2mp::windows::Sha256 copied_digest{};
  if (!ht2mp::windows::sha256_file(temporary, copied_digest, error) ||
      copied_digest != source_digest) {
    cleanup_temporary();
    if (error.empty()) error = "Copied driver seed hash mismatch";
    return false;
  }
  std::string identity_error;
  const bool aliases_source = ht2mp::windows::same_existing_file(
      canonical_source, temporary, identity_error);
  if (!identity_error.empty() || aliases_source) {
    cleanup_temporary();
    error = !identity_error.empty()
                ? "Cannot compare driver seed identity: " + identity_error
                : "Copied driver seed aliases its source instead of being isolated";
    return false;
  }

  fs::rename(temporary, destination, ec);
  if (ec) {
    cleanup_temporary();
    error = "Cannot commit staged driver seed: " + ec.message();
    return false;
  }

  result.destination = destination;
  result.bytes_copied = static_cast<std::uint64_t>(size);
  return true;
}

bool stage_game(const EditionDescriptor& edition, const fs::path& source,
                StageResult& result, std::string& error,
                const std::string_view instance) {
  result = {};
  error.clear();

  std::error_code ec;
  if (!fs::is_directory(source, ec) || ec) {
    error = "Source is not a readable directory: " + source.string();
    return false;
  }
  const auto normalized_source = fs::weakly_canonical(source, ec);
  if (ec || !normalized_source.is_absolute()) {
    error = "Cannot canonicalize source installation: " +
            (ec ? ec.message() : std::string("path is not absolute"));
    return false;
  }
  if (!verify_king(edition, normalized_source, error)) {
    return false;
  }
  if (!verify_protected_modules(edition, normalized_source, "Source", error)) {
    return false;
  }

  const auto target = staged_game_directory(edition, error, instance);
  if (!error.empty()) {
    return false;
  }
  const auto runtime_root = target.parent_path();
  fs::create_directories(runtime_root, ec);
  if (ec) {
    error = "Cannot create HT2MP runtime root: " + ec.message();
    return false;
  }
  if (!verify_no_reparse_chain(runtime_root, error)) {
    return false;
  }

  // The destination is generated as one direct child of runtime_root.  Check
  // that invariant lexically: weakly_canonical(target) can produce a different
  // spelling on Windows depending on whether that child already exists, which
  // used to make repairing an existing/corrupt stage fail spuriously.
  std::string containment_error;
  if (!paths_equal_lexically(runtime_root, target.parent_path(),
                             containment_error)) {
    error = "Invalid staging destination";
    if (!containment_error.empty()) error += ": " + containment_error;
    return false;
  }
  std::string reverse_containment_error;
  const bool target_in_source = ht2mp::windows::path_is_within(
      normalized_source, target, containment_error);
  if (!containment_error.empty()) {
    error = "Cannot prove source/staging separation: " + containment_error;
    return false;
  }
  const bool source_in_target = ht2mp::windows::path_is_within(
      target, normalized_source, reverse_containment_error);
  if (!reverse_containment_error.empty()) {
    error = "Cannot prove source/staging separation: " +
            reverse_containment_error;
    return false;
  }
  if (target_in_source || source_in_target) {
    error = "Source and staging destination must not contain one another";
    return false;
  }
  if (fs::exists(target, ec)) {
    if (ec) {
      error = "Cannot inspect existing staged runtime: " + ec.message();
      return false;
    }
    if (!verify_no_reparse_tree(target, error)) return false;
  } else if (ec) {
    error = "Cannot inspect staging destination: " + ec.message();
    return false;
  }

  const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64());
  const auto incoming = runtime_root / (L".incoming-" + suffix);
  const auto backup = runtime_root / (L".backup-" + suffix);
  fs::create_directory(incoming, ec);
  if (ec) {
    error = "Cannot create transactional staging directory: " + ec.message();
    return false;
  }

  bool target_was_backed_up = false;
  bool target_committed = false;
  const auto cleanup = [&](std::string& cleanup_error) {
    if (target_committed) {
      cleanup_error.clear();
      return true;
    }
    return recover_failed_transaction(runtime_root, incoming, target, backup,
                                      target_was_backed_up, cleanup_error);
  };

  try {
    fs::recursive_directory_iterator iterator(normalized_source,
                                               fs::directory_options::none);
    const fs::recursive_directory_iterator end;
    for (; iterator != end; ++iterator) {
      const auto& entry = *iterator;
      const auto relative = entry.path().lexically_relative(normalized_source);
      if (relative.empty() || relative.native().starts_with(L"..")) {
        throw std::runtime_error("Source traversal escaped its root");
      }
      if (is_reparse_point(entry.path())) {
        if (entry.is_directory()) {
          iterator.disable_recursion_pending();
        }
        ++result.files_skipped;
        continue;
      }
      if (entry.is_directory()) {
        if (excluded_directory(relative)) {
          iterator.disable_recursion_pending();
          ++result.files_skipped;
          continue;
        }
        fs::create_directories(incoming / relative);
        continue;
      }
      if (!entry.is_regular_file() || excluded_file(relative)) {
        ++result.files_skipped;
        continue;
      }
      fs::create_directories((incoming / relative).parent_path());
      fs::copy_file(entry.path(), incoming / relative, fs::copy_options::none);
      ++result.files_copied;
      result.bytes_copied += entry.file_size();
    }

    if (!verify_king(edition, incoming, error)) {
      std::string cleanup_error;
      (void)cleanup(cleanup_error);
      if (!cleanup_error.empty()) error += "; " + cleanup_error;
      return false;
    }
    if (!verify_protected_modules(edition, incoming, "Staged", error)) {
      std::string cleanup_error;
      (void)cleanup(cleanup_error);
      if (!cleanup_error.empty()) error += "; " + cleanup_error;
      return false;
    }
    if (!verify_protected_module_copies_are_distinct(
            edition, normalized_source, incoming, error)) {
      std::string cleanup_error;
      (void)cleanup(cleanup_error);
      if (!cleanup_error.empty()) error += "; " + cleanup_error;
      return false;
    }

    std::string conversion_error;
    const auto source_utf8 = ht2mp::windows::narrow_utf8(
        normalized_source.wstring(), conversion_error);
    if (!conversion_error.empty()) {
      throw std::runtime_error(conversion_error);
    }
    std::ofstream manifest(incoming / L".ht2mp-stage.json", std::ios::binary);
    if (!manifest) {
      throw std::runtime_error("Cannot create staging manifest");
    }
    manifest << "{\n"
             << "  \"schema\": 2,\n"
             << "  \"profile\": \"" << edition.profile_id << "\",\n"
             << "  \"instance\": \"" << json_escape(instance) << "\",\n"
             << "  \"king_sha256\": \"" << edition.king_sha256 << "\",\n"
             << "  \"source\": \"" << json_escape(source_utf8) << "\",\n"
             << "  \"save_policy\": \"excluded\"\n"
             << "}\n";
    manifest.close();
    if (!manifest) {
      throw std::runtime_error("Cannot finalize staging manifest");
    }

    if (fs::exists(target)) {
      fs::rename(target, backup);
      target_was_backed_up = true;
    }
    fs::rename(incoming, target);
    target_committed = true;
    if (target_was_backed_up) {
      if (!remove_runtime_tree(runtime_root, backup, error)) {
        // The new valid stage is already committed. Report the cleanup issue,
        // but never roll it back in favor of the stale tree.
        result.destination = target;
        return false;
      }
    }
  } catch (const std::exception& exception) {
    error = std::string("Staging failed: ") + exception.what();
    std::string cleanup_error;
    (void)cleanup(cleanup_error);
    if (!cleanup_error.empty()) error += "; " + cleanup_error;
    return false;
  }

  result.destination = target;
  return true;
}

#if defined(HT2MP_STAGING_TESTING)
namespace staging_testing {

bool recover_backup(const fs::path& runtime_root, const fs::path& incoming,
                    const fs::path& target, const fs::path& backup,
                    const bool target_was_backed_up, std::string& error) {
  return recover_failed_transaction(runtime_root, incoming, target, backup,
                                    target_was_backed_up, error);
}

} // namespace staging_testing
#endif

} // namespace ht2mp::client
