#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ht2mp::client {

struct ProtectedModuleDescriptor final {
  std::wstring_view filename;
  std::string_view sha256;
};

struct EditionDescriptor final {
  std::string_view edition;
  std::string_view profile_id;
  std::string_view king_sha256;
  std::span<const ProtectedModuleDescriptor> protected_modules{};
};

[[nodiscard]] std::optional<EditionDescriptor> find_edition(std::string_view value) noexcept;
[[nodiscard]] const std::array<EditionDescriptor, 2>& known_editions() noexcept;

} // namespace ht2mp::client
