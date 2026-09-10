#include "ht2mp/protocol/vehicle_catalog.hpp"

#include <algorithm>
#include <array>

namespace ht2mp::protocol {
namespace {

constexpr std::array<VehicleCatalogEntry, 26> kSteamVehicles{{
    {"Gazelle", "Gazelle", 62U, VehicleCategory::passenger_or_special, 44U, 4U, 0U},
    {"Gazelle1C", "Gazelle 1C", 63U, VehicleCategory::passenger_or_special, 48U, 1U, 1U},
    {"Offroad", "Offroad", 64U, VehicleCategory::passenger_or_special, 40U, 2U, 2U},
    {"Pickup", "Pickup", 65U, VehicleCategory::passenger_or_special, 42U, 2U, 3U},
    {"Patrol", "Patrol", 66U, VehicleCategory::passenger_or_special, 36U, 4U, 4U},
    {"Cayman", "Cayman", 67U, VehicleCategory::passenger_or_special, 49U, 4U, 5U},
    {"BmwM5", "BMW M5", 68U, VehicleCategory::passenger_or_special, 55U, 4U, 6U},
    {"BmwM5police", "BMW M5 Police", 69U, VehicleCategory::passenger_or_special, 54U, 1U, 7U},
    {"Bus", "Bus", 70U, VehicleCategory::passenger_or_special, 12U, 4U, 8U},
    {"Marera", "Marera", 71U, VehicleCategory::passenger_or_special, 16U, 4U, 9U},
    {"Megan", "Megan", 72U, VehicleCategory::passenger_or_special, 20U, 4U, 10U},
    {"Mini", "Mini", 73U, VehicleCategory::passenger_or_special, 24U, 4U, 11U},
    {"Oka", "Oka", 74U, VehicleCategory::passenger_or_special, 28U, 4U, 12U},
    {"Van", "Van", 75U, VehicleCategory::passenger_or_special, 32U, 4U, 13U},
    {"Avensis", "Avensis", 76U, VehicleCategory::passenger_or_special, 8U, 4U, 14U},
    {"Volga", "Volga", 77U, VehicleCategory::passenger_or_special, 4U, 4U, 15U},
    {"Fiat", "Fiat", 78U, VehicleCategory::passenger_or_special, 0U, 4U, 16U},
    {"Sobol", "Sobol", 79U, VehicleCategory::passenger_or_special, 59U, 4U, 17U},
    {"ScaniaR", "Scania (rigid)", 80U, VehicleCategory::rigid_truck, 67U, 4U, 18U},
    {"KamazR", "KamAZ (rigid)", 81U, VehicleCategory::rigid_truck, 71U, 4U, 19U},
    {"RenaultR", "Renault (rigid)", 82U, VehicleCategory::rigid_truck, 75U, 4U, 20U},
    {"ZilR", "ZIL (rigid)", 83U, VehicleCategory::rigid_truck, 63U, 4U, 21U},
    {"MercedesR", "Mercedes-Benz (rigid)", 84U, VehicleCategory::rigid_truck, 79U, 4U, 22U},
    {"VolvoR", "Volvo (rigid)", 85U, VehicleCategory::rigid_truck, 83U, 4U, 23U},
    {"DafR", "DAF (rigid)", 86U, VehicleCategory::rigid_truck, 87U, 4U, 24U},
    {"StormR", "Storm (rigid)", 87U, VehicleCategory::rigid_truck, 91U, 4U, 25U},
}};

}  // namespace

std::span<const VehicleCatalogEntry> steam_vehicle_catalog() noexcept {
    return kSteamVehicles;
}

const VehicleCatalogEntry* find_steam_vehicle_by_key(const std::string_view key) noexcept {
    const auto found = std::find_if(kSteamVehicles.begin(), kSteamVehicles.end(),
                                    [key](const auto& value) { return value.key == key; });
    return found == kSteamVehicles.end() ? nullptr : &*found;
}

const VehicleCatalogEntry* find_steam_vehicle_by_selector(const std::uint16_t selector) noexcept {
    const auto found = std::find_if(kSteamVehicles.begin(), kSteamVehicles.end(),
                                    [selector](const auto& value) { return value.selector == selector; });
    return found == kSteamVehicles.end() ? nullptr : &*found;
}

bool is_allowed_steam_vehicle(const std::uint16_t selector) noexcept {
    return find_steam_vehicle_by_selector(selector) != nullptr;
}

bool to_steam_native_vehicle(
    const std::uint16_t network_selector, const std::uint8_t paint_variant,
    SteamNativeVehicleAppearance& native) noexcept {
    native = {};
    const auto* vehicle = find_steam_vehicle_by_selector(network_selector);
    if (vehicle == nullptr || paint_variant >= vehicle->steam_native_paint_count) {
        return false;
    }
    native.selector = static_cast<std::uint16_t>(
        vehicle->steam_native_selector + paint_variant);
    native.paint_variant = paint_variant;
    native.vehicle_tech_index = vehicle->steam_vehicle_tech_index;
    return true;
}

const VehicleCatalogEntry* find_steam_vehicle_by_native_selector(
    const std::uint16_t native_selector, std::uint8_t& paint_variant) noexcept {
    paint_variant = 0U;
    const auto found = std::find_if(
        kSteamVehicles.begin(), kSteamVehicles.end(),
        [native_selector](const auto& value) {
            return native_selector >= value.steam_native_selector &&
                   native_selector < static_cast<std::uint32_t>(
                       value.steam_native_selector) + value.steam_native_paint_count;
        });
    if (found == kSteamVehicles.end()) return nullptr;
    paint_variant = static_cast<std::uint8_t>(
        native_selector - found->steam_native_selector);
    return &*found;
}

}  // namespace ht2mp::protocol
