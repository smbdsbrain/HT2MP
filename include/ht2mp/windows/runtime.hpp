#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace ht2mp::windows {

using Sha256 = std::array<std::uint8_t, 32>;

[[nodiscard]] bool sha256_file(
    const std::filesystem::path& path,
    Sha256& digest,
    std::string& error);
[[nodiscard]] std::string sha256_hex(const Sha256& digest);
[[nodiscard]] bool parse_sha256(std::string_view text, Sha256& digest) noexcept;
[[nodiscard]] bool secure_random(std::span<std::uint8_t> destination, std::string& error);

[[nodiscard]] bool is_i386_pe(const std::filesystem::path& path, std::string& error);
[[nodiscard]] bool same_existing_file(
    const std::filesystem::path& lhs,
    const std::filesystem::path& rhs,
    std::string& error);
[[nodiscard]] bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate,
    std::string& error);

[[nodiscard]] std::filesystem::path local_app_data(std::string& error);
[[nodiscard]] std::filesystem::path executable_path(std::string& error);
[[nodiscard]] std::wstring quote_command_line_argument(std::wstring_view argument);
[[nodiscard]] std::wstring widen_utf8(std::string_view input, std::string& error);
[[nodiscard]] std::string narrow_utf8(std::wstring_view input, std::string& error);
[[nodiscard]] std::string win32_error(std::uint32_t code);

} // namespace ht2mp::windows
