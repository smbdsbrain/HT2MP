#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace ht2mp::protocol {

inline constexpr std::uint8_t kPaintVariantCount = 4U;

enum class VehicleCategory : std::uint8_t {
    passenger_or_special,
    rigid_truck,
};

struct VehicleCatalogEntry {
    std::string_view key;
    std::string_view display_name;
    // Stable HT2MP/6 wire selector. This is deliberately independent of the
    // game's paint-expanded runtime registry.
    std::uint16_t selector{};
    VehicleCategory category{};
    // Exact-Steam registry selector for paint zero, number of real native
    // variants exposed by this model, and its vehicle.tech record index.
    std::uint16_t steam_native_selector{};
    std::uint8_t steam_native_paint_count{};
    std::uint8_t steam_vehicle_tech_index{};
};

struct SteamNativeVehicleAppearance {
    std::uint16_t selector{};
    std::uint8_t paint_variant{};
    std::uint8_t vehicle_tech_index{};
};

[[nodiscard]] std::span<const VehicleCatalogEntry> steam_vehicle_catalog() noexcept;
[[nodiscard]] const VehicleCatalogEntry* find_steam_vehicle_by_key(std::string_view key) noexcept;
[[nodiscard]] const VehicleCatalogEntry* find_steam_vehicle_by_selector(std::uint16_t selector) noexcept;
[[nodiscard]] bool is_allowed_steam_vehicle(std::uint16_t selector) noexcept;
[[nodiscard]] bool to_steam_native_vehicle(
    std::uint16_t network_selector, std::uint8_t paint_variant,
    SteamNativeVehicleAppearance& native) noexcept;
[[nodiscard]] const VehicleCatalogEntry* find_steam_vehicle_by_native_selector(
    std::uint16_t native_selector, std::uint8_t& paint_variant) noexcept;

}  // namespace ht2mp::protocol
