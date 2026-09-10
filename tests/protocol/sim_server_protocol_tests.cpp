#include "server_protocol.hpp"
#include "test_support.hpp"

#include <cmath>
#include <limits>
#include <string>

namespace {

constexpr std::string_view kProfile = "gog-05588140";

ht2mp::protocol::ServerWelcome welcome(std::string profile = std::string(kProfile),
                                       std::uint64_t session_id = 10,
                                       std::uint64_t player_id = 20) {
    return {std::move(profile), session_id, player_id, 1000,
            static_cast<std::uint8_t>(ht2mp::protocol::kMaxPlayers), 20};
}

ht2mp::protocol::PlayerState state(std::uint64_t session_id = 10,
                                   std::uint64_t player_id = 30) {
    ht2mp::protocol::PlayerState value;
    value.session_id = session_id;
    value.player_id = player_id;
    value.incarnation_id = 40;
    value.sequence = 1;
    value.sample_time_ms = 1000;
    value.orientation.w = 1.0F;
    return value;
}

bool wrong_profile_is_rejected_without_admission() {
    const auto limits = ht2mp::protocol::limits_for_profile(kProfile);
    HT2MP_CHECK(limits);
    ht2mp::sim::ServerProtocolState protocol_state;
    const auto result = ht2mp::sim::accept_server_message(
        welcome("steam-8138acee"), kProfile, *limits, protocol_state);
    HT2MP_CHECK(!result);
    HT2MP_CHECK(!protocol_state.welcomed);
    HT2MP_CHECK(protocol_state.session_id == 0);
    HT2MP_CHECK(protocol_state.player_id == 0);
    return true;
}

bool repeated_welcome_is_rejected_without_rebinding() {
    const auto limits = ht2mp::protocol::limits_for_profile(kProfile);
    HT2MP_CHECK(limits);
    ht2mp::sim::ServerProtocolState protocol_state;
    HT2MP_CHECK(ht2mp::sim::accept_server_message(
        welcome(), kProfile, *limits, protocol_state));
    HT2MP_CHECK(protocol_state.welcomed);
    HT2MP_CHECK(!ht2mp::sim::accept_server_message(
        welcome(std::string(kProfile), 11, 21), kProfile, *limits,
        protocol_state));
    HT2MP_CHECK(protocol_state.session_id == 10);
    HT2MP_CHECK(protocol_state.player_id == 20);
    return true;
}

bool pre_welcome_snapshot_is_rejected() {
    const auto limits = ht2mp::protocol::limits_for_profile(kProfile);
    HT2MP_CHECK(limits);
    ht2mp::sim::ServerProtocolState protocol_state;
    const ht2mp::protocol::WorldSnapshot snapshot{1000, {state()}};
    HT2MP_CHECK(!ht2mp::sim::accept_server_message(
        snapshot, kProfile, *limits, protocol_state));
    HT2MP_CHECK(!protocol_state.welcomed);
    return true;
}

bool cross_session_snapshot_is_rejected() {
    const auto limits = ht2mp::protocol::limits_for_profile(kProfile);
    HT2MP_CHECK(limits);
    ht2mp::sim::ServerProtocolState protocol_state;
    HT2MP_CHECK(!ht2mp::sim::accept_server_message(
        welcome(std::string(kProfile), 0, 20), kProfile, *limits,
        protocol_state));
    HT2MP_CHECK(!protocol_state.welcomed);
    HT2MP_CHECK(ht2mp::sim::accept_server_message(
        welcome(), kProfile, *limits, protocol_state));
    const ht2mp::protocol::WorldSnapshot malformed{1100, {state(11)}};
    HT2MP_CHECK(!ht2mp::sim::accept_server_message(
        malformed, kProfile, *limits, protocol_state));
    HT2MP_CHECK(protocol_state.session_id == 10);
    return true;
}

bool valid_snapshot_and_disconnect_is_terminal() {
    const auto limits = ht2mp::protocol::limits_for_profile(kProfile);
    HT2MP_CHECK(limits);
    ht2mp::sim::ServerProtocolState rejected_state;
    const auto admission_rejection = ht2mp::sim::accept_server_message(
        ht2mp::protocol::Disconnect{ht2mp::protocol::DisconnectReason::profile_mismatch,
                                    "wrong profile"},
        kProfile, *limits, rejected_state);
    HT2MP_CHECK(admission_rejection);
    HT2MP_CHECK(admission_rejection.terminal);
    HT2MP_CHECK(!rejected_state.welcomed);

    ht2mp::sim::ServerProtocolState admitted_state;
    HT2MP_CHECK(ht2mp::sim::accept_server_message(
        welcome(), kProfile, *limits, admitted_state));
    HT2MP_CHECK(ht2mp::sim::accept_server_message(
        ht2mp::protocol::WorldSnapshot{1100, {state()}}, kProfile, *limits,
        admitted_state));
    const auto session_disconnect = ht2mp::sim::accept_server_message(
        ht2mp::protocol::Disconnect{ht2mp::protocol::DisconnectReason::server_shutdown,
                                    "restart"},
        kProfile, *limits, admitted_state);
    HT2MP_CHECK(session_disconnect);
    HT2MP_CHECK(session_disconnect.terminal);
    return true;
}

bool relayed_state_is_accepted_but_invalid_direction_is_rejected() {
    const auto limits = ht2mp::protocol::limits_for_profile(kProfile);
    HT2MP_CHECK(limits);
    ht2mp::sim::ServerProtocolState protocol_state;
    HT2MP_CHECK(ht2mp::sim::accept_server_message(
        welcome(), kProfile, *limits, protocol_state));
    HT2MP_CHECK(ht2mp::sim::accept_server_message(
        state(), kProfile, *limits, protocol_state));
    HT2MP_CHECK(!ht2mp::sim::accept_server_message(
        state(11), kProfile, *limits, protocol_state));
    HT2MP_CHECK(!ht2mp::sim::accept_server_message(
        state(10, 20), kProfile, *limits, protocol_state));
    HT2MP_CHECK(!ht2mp::sim::accept_server_message(
        ht2mp::protocol::ClientHello{}, kProfile, *limits, protocol_state));
    HT2MP_CHECK(!ht2mp::sim::accept_server_message(
        ht2mp::protocol::WorldSnapshot{1100, {state(), state()}}, kProfile,
        *limits, protocol_state));
    return true;
}

bool follower_forward_is_horizontal_and_normalized() {
    const auto identity = ht2mp::sim::ground_forward({0.0F, 0.0F, 0.0F, 1.0F});
    HT2MP_CHECK(std::abs(identity.x) < 1.0e-9);
    HT2MP_CHECK(std::abs(identity.y - 1.0) < 1.0e-9);
    HT2MP_CHECK(identity.z == 0.0);

    constexpr float kHalfSqrtTwo = 0.7071067811865475F;
    const auto quarter_turn =
        ht2mp::sim::ground_forward({0.0F, 0.0F, kHalfSqrtTwo, kHalfSqrtTwo});
    HT2MP_CHECK(std::abs(quarter_turn.x - 1.0) < 1.0e-6);
    HT2MP_CHECK(std::abs(quarter_turn.y) < 1.0e-6);
    HT2MP_CHECK(quarter_turn.z == 0.0);

    const auto tilted = ht2mp::sim::ground_forward({0.3F, -0.2F, 0.1F, 0.9F});
    HT2MP_CHECK(std::abs(std::hypot(tilted.x, tilted.y) - 1.0) < 1.0e-9);
    HT2MP_CHECK(tilted.z == 0.0);
    return true;
}

bool invalid_follower_orientation_uses_safe_forward() {
    const auto zero = ht2mp::sim::ground_forward({});
    HT2MP_CHECK(zero.x == 0.0 && zero.y == 1.0 && zero.z == 0.0);

    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto invalid = ht2mp::sim::ground_forward({nan, 0.0F, 0.0F, 1.0F});
    HT2MP_CHECK(invalid.x == 0.0 && invalid.y == 1.0 && invalid.z == 0.0);
    return true;
}

bool orbit_route_has_expected_pose_and_velocity() {
    constexpr double kRadius = 18.0;
    constexpr double kPeriod = 24.0;
    constexpr double kAngularSpeed = 2.0 * 3.14159265358979323846 / kPeriod;

    const auto start = ht2mp::sim::orbit_pose(0.0, kRadius, kPeriod);
    HT2MP_CHECK(std::abs(start.offset.x - kRadius) < 1.0e-9);
    HT2MP_CHECK(std::abs(start.offset.y) < 1.0e-9);
    HT2MP_CHECK(start.offset.z == 0.0);
    constexpr float kHalfSqrtTwo = 0.7071067811865475F;
    HT2MP_CHECK(std::abs(start.orientation.z + kHalfSqrtTwo) < 1.0e-6F);
    HT2MP_CHECK(std::abs(start.orientation.w - kHalfSqrtTwo) < 1.0e-6F);
    const auto start_forward = ht2mp::sim::ground_forward(start.orientation);
    HT2MP_CHECK(std::abs(start_forward.x + 1.0) < 1.0e-6);
    HT2MP_CHECK(std::abs(start_forward.y) < 1.0e-6);
    HT2MP_CHECK(std::abs(start.linear_velocity.x) < 1.0e-6F);
    HT2MP_CHECK(std::abs(start.linear_velocity.y - kRadius * kAngularSpeed) < 1.0e-5F);
    HT2MP_CHECK(std::abs(start.angular_velocity.z + kAngularSpeed) < 1.0e-6F);

    const auto quarter = ht2mp::sim::orbit_pose(kPeriod / 4.0, kRadius, kPeriod);
    HT2MP_CHECK(std::abs(quarter.offset.x) < 1.0e-9);
    HT2MP_CHECK(std::abs(quarter.offset.y - kRadius) < 1.0e-9);
    HT2MP_CHECK(std::abs(quarter.orientation.z + 1.0F) < 1.0e-6F);
    HT2MP_CHECK(std::abs(quarter.orientation.w) < 1.0e-6F);
    const auto quarter_forward = ht2mp::sim::ground_forward(quarter.orientation);
    HT2MP_CHECK(std::abs(quarter_forward.x) < 1.0e-6);
    HT2MP_CHECK(std::abs(quarter_forward.y + 1.0) < 1.0e-6);
    HT2MP_CHECK(std::abs(quarter.linear_velocity.x + kRadius * kAngularSpeed) < 1.0e-5F);
    HT2MP_CHECK(std::abs(quarter.linear_velocity.y) < 1.0e-5F);
    const double quaternion_norm = std::hypot(quarter.orientation.z, quarter.orientation.w);
    HT2MP_CHECK(std::abs(quaternion_norm - 1.0) < 1.0e-6);
    return true;
}

bool invalid_orbit_route_is_safe() {
    const auto invalid = ht2mp::sim::orbit_pose(
        std::numeric_limits<double>::quiet_NaN(), 18.0, 24.0);
    HT2MP_CHECK(invalid.offset.x == 0.0 && invalid.offset.y == 0.0 &&
                invalid.offset.z == 0.0);
    HT2MP_CHECK(invalid.orientation.x == 0.0F && invalid.orientation.y == 0.0F &&
                invalid.orientation.z == 0.0F && invalid.orientation.w == 1.0F);
    HT2MP_CHECK(invalid.linear_velocity.x == 0.0F &&
                invalid.linear_velocity.y == 0.0F &&
                invalid.linear_velocity.z == 0.0F);
    return true;
}

}  // namespace

int main() {
    int failures{};
    failures += run_test("sim rejects wrong profile",
                         wrong_profile_is_rejected_without_admission);
    failures += run_test("sim rejects repeated welcome",
                         repeated_welcome_is_rejected_without_rebinding);
    failures += run_test("sim rejects snapshot before welcome",
                         pre_welcome_snapshot_is_rejected);
    failures += run_test("sim rejects cross-session snapshot",
                         cross_session_snapshot_is_rejected);
    failures += run_test("sim treats disconnect as terminal",
                         valid_snapshot_and_disconnect_is_terminal);
    failures += run_test("sim accepts relayed state and rejects invalid direction",
                         relayed_state_is_accepted_but_invalid_direction_is_rejected);
    failures += run_test("sim follower forward stays on road plane",
                         follower_forward_is_horizontal_and_normalized);
    failures += run_test("sim follower orientation fallback is safe",
                         invalid_follower_orientation_uses_safe_forward);
    failures += run_test("sim orbit route pose and velocity",
                         orbit_route_has_expected_pose_and_velocity);
    failures += run_test("sim invalid orbit route is safe",
                         invalid_orbit_route_is_safe);
    return failures == 0 ? 0 : 1;
}
