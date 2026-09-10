#include "edition.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace ht2mp::client {
namespace {

constexpr std::string_view kDdrawSha256 =
    "986aee2a50b373bd6a5272ae8f2e88ab3545ce08b4b7480a36ba1caeccddbdd3";
constexpr std::array kGogProtectedModules{
    ProtectedModuleDescriptor{L"ddraw.dll", kDdrawSha256},
};
constexpr std::array kSteamProtectedModules{
    ProtectedModuleDescriptor{L"ddraw.dll", kDdrawSha256},
    ProtectedModuleDescriptor{
        L"dinput.dll",
        "b3d36591d760710501b37e9827ba5cf0c0daa86d78d0aa0ea5b2dd50c2311f71"},
    ProtectedModuleDescriptor{
        L"steam_api.dll",
        "fdafb7c65ec7d4182b42bf92cc8671c68e4dc547cc6e613899d2618a47eb65da"},
};

constexpr std::array<EditionDescriptor, 2> kEditions{{
    {"gog", "gog-05588140",
     "4412a5f695dd016c9f92185b7d2d0be8ad3be787bfe2d77908f5b4d171eedd86",
     kGogProtectedModules},
    {"steam", "steam-8138acee",
     "8138aceebfd67b9ed3d8e1d209a34ded92075c5627a19dc6ccbb32fb0e4c3d36",
     kSteamProtectedModules},
}};

std::string lowercase(std::string_view input) {
  std::string result(input);
  std::transform(result.begin(), result.end(), result.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

} // namespace

std::optional<EditionDescriptor> find_edition(const std::string_view value) noexcept {
  const auto normalized = lowercase(value);
  for (const auto& edition : kEditions) {
    if (normalized == edition.edition || normalized == edition.profile_id) {
      return edition;
    }
  }
  return std::nullopt;
}

const std::array<EditionDescriptor, 2>& known_editions() noexcept {
  return kEditions;
}

} // namespace ht2mp::client
