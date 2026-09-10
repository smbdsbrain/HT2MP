#include "server_protocol.hpp"

#include <cmath>
#include <numbers>
#include <unordered_set>
#include <variant>

namespace ht2mp::sim {
namespace {

ServerMessageResult reject(std::string detail) {
    return {std::move(detail), false};
}

}  // namespace

protocol::Vec3d ground_forward(const protocol::Quatf& orientation) noexcept {
    const double x = orientation.x;
    const double y = orientation.y;
    const double z = orientation.z;
    const double w = orientation.w;
    const double norm = std::sqrt(x * x + y * y + z * z + w * w);
    if (!std::isfinite(norm) || norm < 1.0e-6) {
        return {0.0, 1.0, 0.0};
    }

    const double nx = x / norm;
    const double ny = y / norm;
    const double nz = z / norm;
    const double nw = w / norm;

    // Hard Truck 2 consumes the basis as row vectors. Its canonical +Y
    // forward axis is therefore the second row, not the second column of the
    // conventional quaternion matrix. A live forward-drive trace independently
    // confirmed the +X sign below.
    const double forward_x = 2.0 * (nx * ny + nw * nz);
    const double forward_y = 1.0 - 2.0 * (nx * nx + nz * nz);
    const double horizontal_length = std::hypot(forward_x, forward_y);
    if (!std::isfinite(horizontal_length) || horizontal_length < 1.0e-6) {
        return {0.0, 1.0, 0.0};
    }
    return {forward_x / horizontal_length, forward_y / horizontal_length, 0.0};
}

OrbitPose orbit_pose(const double elapsed_seconds,
                     const double radius,
                     const double period_seconds,
                     const double phase_offset) noexcept {
    OrbitPose pose;
    pose.orientation.w = 1.0F;
    if (!std::isfinite(elapsed_seconds) || !std::isfinite(radius) ||
        !std::isfinite(period_seconds) || !std::isfinite(phase_offset) ||
        radius <= 0.0 || period_seconds <= 0.0) {
        return pose;
    }

    const double angular_speed = 2.0 * std::numbers::pi / period_seconds;
    const double phase = std::fmod(elapsed_seconds * angular_speed + phase_offset,
                                   2.0 * std::numbers::pi);
    const double cosine = std::cos(phase);
    const double sine = std::sin(phase);

    pose.offset = {radius * cosine, radius * sine, 0.0};
    // With the game's row-vector basis, yaw -phase-pi/2 maps canonical +Y to
    // the inward radial direction (-cos(phase), -sin(phase), 0), so the remote
    // truck continuously faces the player at the centre of the route.
    const double facing_angle = -phase - std::numbers::pi * 0.5;
    pose.orientation = {
        0.0F, 0.0F, static_cast<float>(std::sin(facing_angle * 0.5)),
        static_cast<float>(std::cos(facing_angle * 0.5))};
    pose.linear_velocity = {
        static_cast<float>(-radius * angular_speed * sine),
        static_cast<float>(radius * angular_speed * cosine), 0.0F};
    pose.angular_velocity = {0.0F, 0.0F, static_cast<float>(-angular_speed)};
    return pose;
}

ServerMessageResult accept_server_message(
    const protocol::Message& message,
    const std::string_view expected_profile,
    const protocol::ProfileLimits& profile_limits,
    ServerProtocolState& state) {
    if (const auto validation = protocol::validate(message, profile_limits);
        !validation) {
        return reject("invalid HT2MP/6 data: " + validation.detail);
    }

    if (std::holds_alternative<protocol::ClientHello>(message)) {
        return reject("coordinator sent a client-only message");
    }

    if (const auto* welcome = std::get_if<protocol::ServerWelcome>(&message)) {
        if (state.welcomed) {
            return reject("coordinator sent a repeated ServerWelcome");
        }
        if (welcome->profile_id != expected_profile) {
            return reject("ServerWelcome selected a different game profile");
        }
        if (welcome->max_players != protocol::kMaxPlayers ||
            welcome->state_hz != 20U) {
            return reject("ServerWelcome selected unsupported session limits");
        }

        state.welcomed = true;
        state.session_id = welcome->session_id;
        state.player_id = welcome->player_id;
        return {};
    }

    // Disconnect is the only valid pre-admission server response: it carries
    // an admission rejection such as profile_mismatch or session_full. It is
    // also a terminal outcome after admission; a simulator must never keep
    // sending state after the coordinator explicitly ended its membership.
    if (std::holds_alternative<protocol::Disconnect>(message)) {
        return {{}, true};
    }
    if (!state.welcomed) {
        return reject("coordinator sent session data before ServerWelcome");
    }

    if (const auto* remote_state = std::get_if<protocol::PlayerState>(&message)) {
        if (remote_state->session_id != state.session_id) {
            return reject("relayed PlayerState crossed session boundaries");
        }
        if (remote_state->player_id == state.player_id) {
            return reject("coordinator reflected the simulator's own PlayerState");
        }
    } else if (const auto* snapshot = std::get_if<protocol::WorldSnapshot>(&message)) {
        std::unordered_set<std::uint64_t> players;
        players.reserve(snapshot->players.size());
        for (const auto& player : snapshot->players) {
            if (player.session_id != state.session_id) {
                return reject("WorldSnapshot crossed session boundaries");
            }
            if (!players.insert(player.player_id).second) {
                return reject("WorldSnapshot contains a duplicate player");
            }
        }
    } else if (const auto* joined = std::get_if<protocol::PeerJoined>(&message)) {
        if (joined->player_id == state.player_id) {
            return reject("PeerJoined advertised the simulator as a remote peer");
        }
    } else if (const auto* spawn = std::get_if<protocol::SpawnMetadata>(&message)) {
        if (spawn->player_id == state.player_id) {
            return reject("SpawnMetadata advertised the simulator as a remote peer");
        }
    } else if (const auto* left = std::get_if<protocol::PeerLeft>(&message)) {
        if (left->player_id == state.player_id) {
            return reject("PeerLeft targeted the admitted simulator");
        }
    }

    return {};
}

}  // namespace ht2mp::sim
