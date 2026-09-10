#pragma once

#include "ht2mp/protocol/types.hpp"
#include "ht2mp/protocol/validation.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace ht2mp::sim {

struct ServerProtocolState {
    bool welcomed{};
    std::uint64_t session_id{};
    std::uint64_t player_id{};
};

struct ServerMessageResult {
    std::string detail;
    // A syntactically valid Disconnect is not a protocol violation, but it
    // still requires the simulator process to stop with a failed run result.
    bool terminal{};

    [[nodiscard]] explicit operator bool() const noexcept { return detail.empty(); }
};

// Hard Truck 2 uses a Z-up world and a row-vector transform basis. Rotate the
// vehicle's canonical +Y axis, then project it onto the road plane so follower
// spacing never changes altitude on slopes or because of vehicle pitch/roll.
[[nodiscard]] protocol::Vec3d ground_forward(
    const protocol::Quatf& orientation) noexcept;

// Produces a Z-up circular demonstration route around an arbitrary origin.
// The vehicle's canonical +Y forward axis always points inward at the origin,
// making both position and orientation updates directly observable.
struct OrbitPose {
    protocol::Vec3d offset;
    protocol::Quatf orientation;
    protocol::Vec3f linear_velocity;
    protocol::Vec3f angular_velocity;
};

[[nodiscard]] OrbitPose orbit_pose(double elapsed_seconds,
                                   double radius,
                                   double period_seconds,
                                   double phase_offset = 0.0) noexcept;

// Validates both the wire values and the server-to-simulator protocol phase.
// State changes only when a valid, first ServerWelcome is accepted.
[[nodiscard]] ServerMessageResult accept_server_message(
    const protocol::Message& message,
    std::string_view expected_profile,
    const protocol::ProfileLimits& profile_limits,
    ServerProtocolState& state);

}  // namespace ht2mp::sim
