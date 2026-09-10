#include "ht2mp/protocol/interpolation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace ht2mp::protocol {
namespace {

Vec3d hermite(const Vec3d& first,
              const Vec3f& first_velocity,
              const Vec3d& second,
              const Vec3f& second_velocity,
              double alpha,
              double duration_seconds) noexcept {
    const double a2 = alpha * alpha;
    const double a3 = a2 * alpha;
    const double h00 = 2.0 * a3 - 3.0 * a2 + 1.0;
    const double h10 = a3 - 2.0 * a2 + alpha;
    const double h01 = -2.0 * a3 + 3.0 * a2;
    const double h11 = a3 - a2;
    return {
        h00 * first.x + h10 * duration_seconds * first_velocity.x + h01 * second.x +
            h11 * duration_seconds * second_velocity.x,
        h00 * first.y + h10 * duration_seconds * first_velocity.y + h01 * second.y +
            h11 * duration_seconds * second_velocity.y,
        h00 * first.z + h10 * duration_seconds * first_velocity.z + h01 * second.z +
            h11 * duration_seconds * second_velocity.z,
    };
}

Vec3f lerp(const Vec3f& first, const Vec3f& second, float alpha) noexcept {
    return {
        first.x + (second.x - first.x) * alpha,
        first.y + (second.y - first.y) * alpha,
        first.z + (second.z - first.z) * alpha,
    };
}

Quatf normalized(Quatf value) noexcept {
    const float norm = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
    if (norm <= std::numeric_limits<float>::epsilon()) {
        return {};
    }
    value.x /= norm;
    value.y /= norm;
    value.z /= norm;
    value.w /= norm;
    return value;
}

Quatf slerp(Quatf first, Quatf second, float alpha) noexcept {
    float dot = first.x * second.x + first.y * second.y + first.z * second.z + first.w * second.w;
    if (dot < 0.0F) {
        second.x = -second.x;
        second.y = -second.y;
        second.z = -second.z;
        second.w = -second.w;
        dot = -dot;
    }
    dot = std::clamp(dot, -1.0F, 1.0F);
    if (dot > 0.9995F) {
        return normalized({first.x + alpha * (second.x - first.x),
                           first.y + alpha * (second.y - first.y),
                           first.z + alpha * (second.z - first.z),
                           first.w + alpha * (second.w - first.w)});
    }
    const float angle = std::acos(dot);
    const float sine = std::sin(angle);
    const float first_weight = std::sin((1.0F - alpha) * angle) / sine;
    const float second_weight = std::sin(alpha * angle) / sine;
    return normalized({first.x * first_weight + second.x * second_weight,
                       first.y * first_weight + second.y * second_weight,
                       first.z * first_weight + second.z * second_weight,
                       first.w * first_weight + second.w * second_weight});
}

Quatf multiply(const Quatf& first, const Quatf& second) noexcept {
    return normalized({first.w * second.x + first.x * second.w + first.y * second.z - first.z * second.y,
                       first.w * second.y - first.x * second.z + first.y * second.w + first.z * second.x,
                       first.w * second.z + first.x * second.y - first.y * second.x + first.z * second.w,
                       first.w * second.w - first.x * second.x - first.y * second.y - first.z * second.z});
}

Quatf extrapolate_orientation(const Quatf& orientation, const Vec3f& angular_velocity, double seconds) noexcept {
    const double speed = std::sqrt(static_cast<double>(angular_velocity.x) * angular_velocity.x +
                                   static_cast<double>(angular_velocity.y) * angular_velocity.y +
                                   static_cast<double>(angular_velocity.z) * angular_velocity.z);
    if (speed < 1.0e-7) {
        return orientation;
    }
    const double half_angle = speed * seconds * 0.5;
    const double scale = std::sin(half_angle) / speed;
    const Quatf delta{static_cast<float>(angular_velocity.x * scale),
                      static_cast<float>(angular_velocity.y * scale),
                      static_cast<float>(angular_velocity.z * scale),
                      static_cast<float>(std::cos(half_angle))};
    return multiply(orientation, delta);
}

PlayerState interpolate(const PlayerState& first, const PlayerState& second, std::uint64_t target_time) {
    const auto duration_ms = second.sample_time_ms - first.sample_time_ms;
    const double alpha = duration_ms == 0 ? 1.0 : static_cast<double>(target_time - first.sample_time_ms) / duration_ms;
    const auto clamped = std::clamp(alpha, 0.0, 1.0);
    PlayerState result = clamped < 0.5 ? first : second;
    result.position = hermite(first.position,
                              first.linear_velocity,
                              second.position,
                              second.linear_velocity,
                              clamped,
                              static_cast<double>(duration_ms) / 1000.0);
    result.orientation = slerp(first.orientation, second.orientation, static_cast<float>(clamped));
    result.linear_velocity = lerp(first.linear_velocity, second.linear_velocity, static_cast<float>(clamped));
    result.angular_velocity = lerp(first.angular_velocity, second.angular_velocity, static_cast<float>(clamped));
    result.location.road_distance = first.location.road_distance +
                                    (second.location.road_distance - first.location.road_distance) * clamped;
    result.sample_time_ms = target_time;
    result.sequence = second.sequence;
    result.set(PlayerStateFlag::teleport, false);
    return result;
}

}  // namespace

RemoteTimeline::RemoteTimeline(TimelineOptions options, ProfileLimits limits)
    : options_(options), limits_(std::move(limits)) {
    options_.max_samples = std::max<std::size_t>(options_.max_samples, 2);
    options_.max_extrapolation_ms = std::min(options_.max_extrapolation_ms, options_.freeze_after_ms);
    options_.freeze_after_ms = std::min(options_.freeze_after_ms, options_.hide_after_ms);
    options_.hide_after_ms = std::min(options_.hide_after_ms, options_.despawn_after_ms);
}

ValidationResult RemoteTimeline::push(PlayerState incoming) {
    if (const auto result = validate(incoming, limits_); !result) {
        return result;
    }
    if (samples_.empty()) {
        player_id_ = incoming.player_id;
        incarnation_id_ = incoming.incarnation_id;
    } else if (incoming.player_id != player_id_) {
        return {ValidationCode::invalid_identifier, "timeline cannot mix player IDs"};
    } else if (incoming.incarnation_id != incarnation_id_) {
        clear();
        player_id_ = incoming.player_id;
        incarnation_id_ = incoming.incarnation_id;
    } else {
        const auto& latest = samples_.back();
        if (!sequence_newer(incoming.sequence, latest.sequence)) {
            return {ValidationCode::out_of_bounds, "duplicate or stale sequence"};
        }
        if (incoming.sample_time_ms <= latest.sample_time_ms) {
            return {ValidationCode::out_of_bounds, "sample time did not advance"};
        }
        const bool changed_room = incoming.location.room_id != latest.location.room_id;
        if (changed_room || incoming.has(PlayerStateFlag::teleport)) {
            samples_.clear();
        }
    }

    samples_.push_back(std::move(incoming));
    while (samples_.size() > options_.max_samples) {
        samples_.pop_front();
    }
    return {};
}

PlaybackSample RemoteTimeline::sample(std::uint64_t server_time_ms) const {
    if (samples_.empty()) {
        return {};
    }
    const auto& latest = samples_.back();
    const auto age = server_time_ms > latest.sample_time_ms ? server_time_ms - latest.sample_time_ms : 0;
    if (age >= options_.despawn_after_ms) {
        return {PlaybackState::despawn, std::nullopt};
    }
    if (!latest.has(PlayerStateFlag::in_world) || age >= options_.hide_after_ms) {
        return {PlaybackState::hidden, std::nullopt};
    }
    if (latest.has(PlayerStateFlag::paused) || age >= options_.freeze_after_ms) {
        return {PlaybackState::frozen, latest};
    }

    const auto target_time = server_time_ms > options_.interpolation_delay_ms
                                 ? server_time_ms - options_.interpolation_delay_ms
                                 : 0;
    if (target_time <= samples_.front().sample_time_ms) {
        return {PlaybackState::interpolated, samples_.front()};
    }
    for (std::size_t index = 1; index < samples_.size(); ++index) {
        if (target_time <= samples_[index].sample_time_ms) {
            return {PlaybackState::interpolated, interpolate(samples_[index - 1], samples_[index], target_time)};
        }
    }

    const auto extrapolation_ms = target_time - latest.sample_time_ms;
    if (extrapolation_ms <= options_.max_extrapolation_ms) {
        PlayerState result = latest;
        const double seconds = static_cast<double>(extrapolation_ms) / 1000.0;
        result.position.x += result.linear_velocity.x * seconds;
        result.position.y += result.linear_velocity.y * seconds;
        result.position.z += result.linear_velocity.z * seconds;
        result.orientation = extrapolate_orientation(result.orientation, result.angular_velocity, seconds);
        // World velocity cannot be projected onto the game's curved road
        // coordinate without build-local road geometry. Keep the last exact
        // PositionId during the bounded extrapolation and move only the visual
        // world transform; using world Z here treated vertical speed as road
        // progress and could manufacture an invalid PositionId.
        result.sample_time_ms = target_time;
        result.set(PlayerStateFlag::teleport, false);
        return {PlaybackState::extrapolated, result};
    }
    return {PlaybackState::frozen, latest};
}

void RemoteTimeline::clear() noexcept {
    samples_.clear();
    player_id_ = 0;
    incarnation_id_ = 0;
}

bool sequence_newer(std::uint32_t candidate, std::uint32_t baseline) noexcept {
    const auto delta = candidate - baseline;
    // RFC-1982-style half-range comparison without an implementation-defined
    // unsigned-to-signed conversion. Exactly half the sequence space is
    // intentionally ambiguous and therefore not considered newer.
    return delta != 0U && delta < 0x80000000U;
}

}  // namespace ht2mp::protocol
