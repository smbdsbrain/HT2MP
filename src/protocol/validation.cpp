#include "ht2mp/protocol/validation.hpp"
#include "ht2mp/protocol/vehicle_catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace ht2mp::protocol {
namespace {

ValidationResult fail(ValidationCode code, std::string detail) {
    return {code, std::move(detail)};
}

bool finite(const Vec3d& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const Vec3f& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool finite(const Quatf& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

bool inside(double value, double minimum, double maximum) noexcept {
    return value >= minimum && value <= maximum;
}

bool magnitude_components(const Vec3f& value, float maximum) noexcept {
    return std::abs(value.x) <= maximum && std::abs(value.y) <= maximum && std::abs(value.z) <= maximum;
}

bool valid_location_id(std::int32_t value, const ProfileLimits& limits) noexcept {
    return value >= limits.min_location_id && value <= limits.max_location_id;
}

bool valid_reason(DisconnectReason value) noexcept {
    return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(DisconnectReason::server_shutdown);
}

ValidationResult validate_name(const std::string& value, std::size_t maximum, const char* field, bool empty_ok) {
    if ((!empty_ok && value.empty()) || value.size() > maximum) {
        return fail(ValidationCode::invalid_length, std::string(field) + " has an invalid byte length");
    }
    if (!is_valid_utf8(value)) {
        return fail(ValidationCode::invalid_utf8, std::string(field) + " is not valid UTF-8");
    }
    for (const unsigned char byte : value) {
        if (byte < 0x20U || byte == 0x7FU) {
            return fail(ValidationCode::invalid_utf8, std::string(field) + " contains a control character");
        }
    }
    return {};
}

ValidationResult validate_profile(const std::string& value) {
    const auto basic = validate_name(value, kMaxProfileIdBytes, "profile_id", false);
    if (!basic) {
        return basic;
    }
    for (const unsigned char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                             character == '-' || character == '_' || character == '.';
        if (!allowed) {
            return fail(ValidationCode::invalid_utf8, "profile_id contains an unsupported character");
        }
    }
    return {};
}

ValidationResult validate_vehicle(std::uint16_t value, const ProfileLimits& limits) {
    if (value > limits.max_vehicle_type) {
        return fail(ValidationCode::invalid_vehicle_type, "vehicle_type exceeds profile bounds");
    }
    if (!limits.allowed_vehicle_types.empty() &&
        std::find(limits.allowed_vehicle_types.begin(), limits.allowed_vehicle_types.end(), value) ==
            limits.allowed_vehicle_types.end()) {
        return fail(ValidationCode::invalid_vehicle_type, "vehicle_type is not in the profile allowlist");
    }
    return {};
}

ValidationResult validate_paint(const std::uint8_t value) {
    if (value >= kPaintVariantCount) {
        return fail(ValidationCode::invalid_paint_variant, "paint_variant must be in range 0..3");
    }
    return {};
}

}  // namespace

std::optional<ProfileLimits> limits_for_profile(
    const std::string_view profile_id) {
    if (profile_id != "gog-05588140" && profile_id != "steam-8138acee") {
        return std::nullopt;
    }
    ProfileLimits limits;
    if (profile_id == "gog-05588140") {
        limits.max_vehicle_type = 0U;
        limits.allowed_vehicle_types = {0U};
    } else {
        // Exact-Steam uses the native vehicle.tech registry selector. The
        // injected bridge additionally proves that the advertised entry is
        // present in the live registry before creating a remote actor.
        limits.max_vehicle_type = 87U;
        for (const auto& vehicle : steam_vehicle_catalog()) {
            limits.allowed_vehicle_types.push_back(vehicle.selector);
        }
    }
    return limits;
}

bool is_valid_utf8(const std::string& value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char first = bytes[index++];
        if (first <= 0x7FU) {
            continue;
        }

        std::uint32_t codepoint{};
        std::size_t continuation{};
        std::uint32_t minimum{};
        if ((first & 0xE0U) == 0xC0U) {
            codepoint = first & 0x1FU;
            continuation = 1;
            minimum = 0x80U;
        } else if ((first & 0xF0U) == 0xE0U) {
            codepoint = first & 0x0FU;
            continuation = 2;
            minimum = 0x800U;
        } else if ((first & 0xF8U) == 0xF0U) {
            codepoint = first & 0x07U;
            continuation = 3;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (index + continuation > value.size()) {
            return false;
        }
        for (std::size_t count = 0; count < continuation; ++count) {
            const unsigned char next = bytes[index++];
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3FU);
        }
        if (codepoint < minimum || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
            return false;
        }
    }
    return true;
}

ValidationResult validate(const ClientHello& hello, const ProfileLimits& limits) {
    if (hello.incarnation_id == 0) {
        return fail(ValidationCode::invalid_identifier, "incarnation_id must be non-zero");
    }
    if (const auto profile = validate_profile(hello.profile_id); !profile) {
        return profile;
    }
    if (const auto name = validate_name(hello.display_name, kMaxDisplayNameBytes, "display_name", false); !name) {
        return name;
    }
    if (const auto vehicle = validate_vehicle(hello.vehicle_type, limits); !vehicle) {
        return vehicle;
    }
    return validate_paint(hello.paint_variant);
}

ValidationResult validate(const PlayerState& state, const ProfileLimits& limits) {
    if (state.session_id == 0 || state.player_id == 0 || state.incarnation_id == 0) {
        return fail(ValidationCode::invalid_identifier, "player state identifiers must be non-zero");
    }
    constexpr std::uint8_t known_flags = flag(PlayerStateFlag::in_world) | flag(PlayerStateFlag::paused) |
                                         flag(PlayerStateFlag::teleport);
    if ((state.flags & ~known_flags) != 0) {
        return fail(ValidationCode::invalid_flag, "player state contains unknown flags");
    }
    if (!finite(state.position) || !finite(state.orientation) || !finite(state.linear_velocity) ||
        !finite(state.angular_velocity) || !std::isfinite(state.location.road_distance)) {
        return fail(ValidationCode::non_finite, "player state contains NaN or infinity");
    }
    if (!inside(state.position.x, limits.min_coordinate, limits.max_coordinate) ||
        !inside(state.position.y, limits.min_coordinate, limits.max_coordinate) ||
        !inside(state.position.z, limits.min_coordinate, limits.max_coordinate)) {
        return fail(ValidationCode::out_of_bounds, "position exceeds profile bounds");
    }
    if (!magnitude_components(state.linear_velocity, limits.max_linear_speed) ||
        !magnitude_components(state.angular_velocity, limits.max_angular_speed)) {
        return fail(ValidationCode::out_of_bounds, "velocity exceeds profile bounds");
    }
    const double norm_squared = static_cast<double>(state.orientation.x) * state.orientation.x +
                                static_cast<double>(state.orientation.y) * state.orientation.y +
                                static_cast<double>(state.orientation.z) * state.orientation.z +
                                static_cast<double>(state.orientation.w) * state.orientation.w;
    if (std::abs(norm_squared - 1.0) > 0.05) {
        return fail(ValidationCode::invalid_quaternion, "orientation is not normalized");
    }
    if (!valid_location_id(state.location.room_id, limits) ||
        !valid_location_id(state.location.road_id, limits) ||
        !valid_location_id(state.location.node_id, limits) ||
        !valid_location_id(state.location.road_segment_vector_id, limits) ||
        !valid_location_id(state.location.road_segment_id, limits) ||
        std::abs(state.location.road_distance) > limits.max_road_distance) {
        return fail(ValidationCode::out_of_bounds, "world location exceeds profile bounds");
    }
    if (!valid_vehicle_state(state.vehicle)) {
        return fail(ValidationCode::out_of_bounds, "invalid vehicle condition or lamp selector");
    }
    if (const auto vehicle = validate_vehicle(state.vehicle_type, limits); !vehicle) {
        return vehicle;
    }
    return validate_paint(state.paint_variant);
}

ValidationResult validate(const WorldSnapshot& snapshot, const ProfileLimits& limits) {
    if (snapshot.players.size() > kMaxSnapshotPlayers) {
        return fail(ValidationCode::too_many_players, "snapshot exceeds per-packet player limit");
    }
    if (!valid_environment(snapshot.environment)) {
        return fail(ValidationCode::out_of_bounds, "invalid server environment");
    }
    for (const auto& player : snapshot.players) {
        if (const auto result = validate(player, limits); !result) {
            return result;
        }
    }
    return {};
}

ValidationResult validate(const Message& message, const ProfileLimits& limits) {
    return std::visit(
        [&limits]<typename T>(const T& value) -> ValidationResult {
            if constexpr (std::is_same_v<T, ClientHello>) {
                return validate(value, limits);
            } else if constexpr (std::is_same_v<T, PlayerState>) {
                return validate(value, limits);
            } else if constexpr (std::is_same_v<T, WorldSnapshot>) {
                return validate(value, limits);
            } else if constexpr (std::is_same_v<T, ServerWelcome>) {
                if (value.session_id == 0 || value.player_id == 0 || value.max_players == 0 ||
                    value.max_players > kMaxPlayers || value.state_hz == 0 || value.state_hz > 40) {
                    return fail(ValidationCode::invalid_identifier, "server welcome contains invalid limits or IDs");
                }
                return validate_profile(value.profile_id);
            } else if constexpr (std::is_same_v<T, PeerJoined> || std::is_same_v<T, SpawnMetadata>) {
                if (value.player_id == 0 || value.incarnation_id == 0) {
                    return fail(ValidationCode::invalid_identifier, "peer identifiers must be non-zero");
                }
                if (const auto name = validate_name(value.display_name, kMaxDisplayNameBytes, "display_name", false);
                    !name) {
                    return name;
                }
                if (const auto vehicle = validate_vehicle(value.vehicle_type, limits); !vehicle) {
                    return vehicle;
                }
                return validate_paint(value.paint_variant);
            } else if constexpr (std::is_same_v<T, PeerLeft>) {
                if (value.player_id == 0 || value.incarnation_id == 0 || !valid_reason(value.reason)) {
                    return fail(ValidationCode::invalid_identifier, "peer-left contains invalid IDs or reason");
                }
                return {};
            } else if constexpr (std::is_same_v<T, Disconnect>) {
                if (!valid_reason(value.reason)) {
                    return fail(ValidationCode::invalid_identifier, "disconnect contains an invalid reason");
                }
                return validate_name(value.detail, kMaxDisconnectTextBytes, "disconnect detail", true);
            } else {
                return {};
            }
        },
        message);
}

}  // namespace ht2mp::protocol
