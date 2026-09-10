#pragma once

#include "ht2mp/protocol/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ht2mp::protocol {

enum class ValidationCode {
    ok = 0,
    invalid_utf8,
    invalid_length,
    invalid_flag,
    non_finite,
    out_of_bounds,
    invalid_quaternion,
    invalid_vehicle_type,
    invalid_paint_variant,
    invalid_identifier,
    too_many_players,
};

struct ValidationResult {
    ValidationCode code{ValidationCode::ok};
    std::string detail;

    [[nodiscard]] explicit operator bool() const noexcept { return code == ValidationCode::ok; }
};

struct ProfileLimits {
    double min_coordinate{-2'000'000.0};
    double max_coordinate{2'000'000.0};
    float max_linear_speed{500.0F};
    float max_angular_speed{100.0F};
    std::int32_t min_location_id{-1};
    std::int32_t max_location_id{10'000'000};
    double max_road_distance{10'000'000.0};
    std::uint16_t max_vehicle_type{4095};
    std::vector<std::uint16_t> allowed_vehicle_types;
};

// HT2MP/6 vehicle_type is a bounded network model code, never a pointer. The
// exact Steam profile carries its native vehicle.tech registry selector; the
// bridge still validates that selector against the live loaded registry.
[[nodiscard]] std::optional<ProfileLimits> limits_for_profile(
    std::string_view profile_id);

[[nodiscard]] bool is_valid_utf8(const std::string& value) noexcept;
[[nodiscard]] ValidationResult validate(const ClientHello& hello, const ProfileLimits& limits = {});
[[nodiscard]] ValidationResult validate(const PlayerState& state, const ProfileLimits& limits = {});
[[nodiscard]] ValidationResult validate(const WorldSnapshot& snapshot, const ProfileLimits& limits = {});
[[nodiscard]] ValidationResult validate(const Message& message, const ProfileLimits& limits = {});

}  // namespace ht2mp::protocol
