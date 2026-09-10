#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace ht2mp::launcher {

struct ProfileEditResult final {
  std::filesystem::path destination;
  std::filesystem::path backup;
  std::uint32_t completed_slots{};
};

// Edits a private copy only. Appearance records are stored in every complete
// game-slot storage and are consumed by the exact-build bridge before the
// native player acquisition path materializes its VehicleInstance.
[[nodiscard]] bool prepare_online_profile(
    const std::filesystem::path& template_profile,
    const std::filesystem::path& runtime_directory,
    std::uint16_t vehicle_selector,
    std::uint8_t paint_variant,
    ProfileEditResult& result,
    std::string& error);

[[nodiscard]] bool write_last_player(const std::filesystem::path& truck_ini,
                                     std::string& error);

}  // namespace ht2mp::launcher
