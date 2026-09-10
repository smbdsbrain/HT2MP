#pragma once

#include "edition.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ht2mp::client {

struct StageResult final {
  std::filesystem::path destination;
  std::uint64_t files_copied{};
  std::uint64_t files_skipped{};
  std::uint64_t bytes_copied{};
};

struct DriverSeedResult final {
  std::filesystem::path destination;
  std::uint64_t bytes_copied{};
};

// Information recovered from, and independently verified against, the
// staging manifest.  In particular, source is the live installation whose
// identity was checked; it is not merely an untrusted string from JSON.
struct StageValidation final {
  std::filesystem::path runtime;
  std::filesystem::path source;
  std::filesystem::path king;
  std::filesystem::path source_king;
};

[[nodiscard]] bool stage_game(
    const EditionDescriptor& edition,
    const std::filesystem::path& source,
    StageResult& result,
    std::string& error,
    std::string_view instance = {});

// Empty denotes the backwards-compatible default runtime. Named instances are
// deliberately restricted to a single safe path component.
[[nodiscard]] bool valid_instance_id(std::string_view instance) noexcept;

[[nodiscard]] std::filesystem::path staged_game_directory(
    const EditionDescriptor& edition,
    std::string& error,
    std::string_view instance = {});

// Fail-closed validation for a runtime immediately before launch.  The
// expected_runtime argument must name exactly the profile-specific directory
// returned by staged_game_directory(); accepting it explicitly lets the
// caller prove that the path it is about to execute is the path validated
// here.  The source installation recorded by schema-1 manifests must still be
// present and contain a distinct, exact-hash king.exe.
[[nodiscard]] bool validate_staged_game(
    const EditionDescriptor& edition,
    const std::filesystem::path& expected_runtime,
    StageValidation& validation,
    std::string& error,
    std::string_view instance = {});

// Copies one explicitly selected driver save into an already validated staged
// runtime. The original filename is retained because TRUCK.INI and the game's
// native Load path use it as the driver identity. Existing staged saves are
// never overwritten; rerun `stage` first when a clean seed is required.
[[nodiscard]] bool seed_staged_driver(
    const EditionDescriptor& edition,
    const std::filesystem::path& source_save,
    DriverSeedResult& result,
    std::string& error,
    std::string_view instance = {});

} // namespace ht2mp::client
