#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace ht2mp::protocol {

inline constexpr std::size_t kMaxVehicleWheels = 8;
struct WheelSpeeds {
    std::uint8_t count{}; // zero: sender has no wheel telemetry
    std::array<float, kMaxVehicleWheels> radians_per_second{};
    friend bool operator==(const WheelSpeeds&, const WheelSpeeds&) = default;
};

inline bool valid_wheel_speeds(const WheelSpeeds& value) noexcept {
    if (value.count > kMaxVehicleWheels || value.count == 1) return false;
    for (std::size_t i = 0; i < kMaxVehicleWheels; ++i) {
        const auto speed = value.radians_per_second[i];
        if (!std::isfinite(speed) || std::abs(speed) > 2000.0F ||
            (i >= value.count && speed != 0.0F)) return false;
    }
    return true;
}

// Semantic values only; no native pointers/layout cross either transport.
// Exact Steam's get_state/set_state schema: 9 deformation channels, 5
// vehicle systems, 27 chassis systems, an accumulator and 7 condition values.
struct HornState {
    bool active{};
    std::uint8_t tone{}; // 0 car, 1 truck, 2 upgraded native horn
    std::uint16_t press_sequence{}; // rising edges, including taps between samples
    friend bool operator==(const HornState&, const HornState&) = default;
};

struct VehicleState {
    bool valid{};
    std::int8_t steering{}; // signed native visual steering quantum
    std::array<std::uint8_t, 4> lights{}; // native lamp selectors, each 0..2
    std::array<float, 49> condition{};
    WheelSpeeds wheels{};
    HornState horn{};
    friend bool operator==(const VehicleState&, const VehicleState&) = default;
};

inline bool valid_vehicle_state(const VehicleState& value) noexcept {
    if (!valid_wheel_speeds(value.wheels) || value.horn.tone > 2U) return false;
    for (const auto light : value.lights) if (light > 2U) return false;
    for (std::size_t i = 0; i < value.condition.size(); ++i) {
        const float field = value.condition[i];
        if (!std::isfinite(field) || field < 0.0F ||
            field > (i == 41U ? 1.0e9F : 1.0F)) return false;
    }
    return true;
}

// Server-generated phases. Clients advance from a received anchor using QPC;
// their save, pause state and local weather clock never become authority.
struct EnvironmentState {
    std::uint64_t session_id{}; // zero means no environment authority
    double day_hours{12.0};
    double weather_phase{};
    double variation_phase{};
    float day_duration_s{1440.0F};
    float weather_duration_s{600.0F};
    float weather_bias{0.5F};
    std::uint8_t weather_preset{};
    friend bool operator==(const EnvironmentState&, const EnvironmentState&) = default;
};

inline bool valid_environment(const EnvironmentState& value) noexcept {
    constexpr double tau = 6.283185307179586;
    return std::isfinite(value.day_hours) && value.day_hours >= 0.0 && value.day_hours < 24.0 &&
           std::isfinite(value.weather_phase) && value.weather_phase >= 0.0 && value.weather_phase < tau &&
           std::isfinite(value.variation_phase) && value.variation_phase >= 0.0 && value.variation_phase < tau &&
           std::isfinite(value.day_duration_s) && value.day_duration_s >= 60.0F && value.day_duration_s <= 86400.0F &&
           std::isfinite(value.weather_duration_s) && value.weather_duration_s >= 60.0F && value.weather_duration_s <= 86400.0F &&
           std::isfinite(value.weather_bias) && value.weather_bias >= 0.0F && value.weather_bias <= 1.0F &&
           value.weather_preset < 8U;
}

inline EnvironmentState advance_environment(EnvironmentState value, double elapsed_s) noexcept {
    constexpr double tau = 6.283185307179586;
    if (!valid_environment(value) || !std::isfinite(elapsed_s) || elapsed_s < 0.0) return value;
    value.day_hours = std::fmod(value.day_hours + std::fmod(elapsed_s, value.day_duration_s) * 24.0 / value.day_duration_s, 24.0);
    value.weather_phase = std::fmod(value.weather_phase + std::fmod(elapsed_s, value.weather_duration_s) * tau / value.weather_duration_s, tau);
    value.variation_phase = std::fmod(value.variation_phase + std::fmod(elapsed_s, value.weather_duration_s * 3.0) * tau / (value.weather_duration_s * 3.0), tau);
    return value;
}

} // namespace ht2mp::protocol
