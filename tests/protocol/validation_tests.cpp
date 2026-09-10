#include "test_support.hpp"

#include "ht2mp/protocol/validation.hpp"

#include <limits>
#include <string>

namespace {

ht2mp::protocol::PlayerState valid_state() {
    ht2mp::protocol::PlayerState state;
    state.session_id = 1;
    state.player_id = 2;
    state.incarnation_id = 3;
    state.orientation.w = 1.0F;
    state.set(ht2mp::protocol::PlayerStateFlag::in_world, true);
    state.location.room_id = 0;
    state.location.road_id = 0;
    state.location.node_id = 0;
    state.location.road_segment_vector_id = 0;
    state.location.road_segment_id = 0;
    return state;
}

bool utf8_validation() {
    HT2MP_CHECK(ht2mp::protocol::is_valid_utf8("Driver"));
    HT2MP_CHECK(ht2mp::protocol::is_valid_utf8("Водитель"));
    HT2MP_CHECK(!ht2mp::protocol::is_valid_utf8(std::string("\xC0\x80", 2)));
    HT2MP_CHECK(!ht2mp::protocol::is_valid_utf8(std::string("\xED\xA0\x80", 3)));
    HT2MP_CHECK(!ht2mp::protocol::is_valid_utf8(std::string("\xF4\x90\x80\x80", 4)));
    return true;
}

bool hello_validation() {
    ht2mp::protocol::ClientHello hello{"gog-05588140", {}, "Driver", 1, 7};
    HT2MP_CHECK(ht2mp::protocol::validate(hello));
    hello.profile_id = "GOG profile";
    HT2MP_CHECK(!ht2mp::protocol::validate(hello));
    hello.profile_id = "gog-05588140";
    hello.display_name.clear();
    HT2MP_CHECK(!ht2mp::protocol::validate(hello));
    hello.display_name = "Driver\n";
    HT2MP_CHECK(!ht2mp::protocol::validate(hello));
    hello.display_name = "Driver";
    hello.incarnation_id = 0;
    HT2MP_CHECK(!ht2mp::protocol::validate(hello));
    return true;
}

bool state_validation() {
    auto state = valid_state();
    HT2MP_CHECK(ht2mp::protocol::validate(state));
    state.flags = 0x80;
    HT2MP_CHECK(!ht2mp::protocol::validate(state));
    state = valid_state();
    state.orientation = {0.0F, 0.0F, 0.0F, 0.0F};
    HT2MP_CHECK(!ht2mp::protocol::validate(state));
    state = valid_state();
    state.position.x = 3'000'000.0;
    HT2MP_CHECK(!ht2mp::protocol::validate(state));
    state = valid_state();
    state.linear_velocity.y = 501.0F;
    HT2MP_CHECK(!ht2mp::protocol::validate(state));
    state = valid_state();
    state.location.road_distance = (std::numeric_limits<double>::infinity)();
    HT2MP_CHECK(!ht2mp::protocol::validate(state));

    ht2mp::protocol::ProfileLimits limits;
    limits.allowed_vehicle_types = {4, 7};
    state = valid_state();
    state.vehicle_type = 7;
    HT2MP_CHECK(ht2mp::protocol::validate(state, limits));
    state.vehicle_type = 8;
    HT2MP_CHECK(!ht2mp::protocol::validate(state, limits));
    state = valid_state();
    state.paint_variant = 4;
    HT2MP_CHECK(!ht2mp::protocol::validate(state));
    return true;
}

bool exact_profile_limits() {
    const auto gog = ht2mp::protocol::limits_for_profile("gog-05588140");
    const auto steam = ht2mp::protocol::limits_for_profile("steam-8138acee");
    HT2MP_CHECK(gog && steam);
    HT2MP_CHECK(!ht2mp::protocol::limits_for_profile("unknown-build"));

    auto hello = ht2mp::protocol::ClientHello{
        "gog-05588140", {}, "Driver", 1, 0};
    HT2MP_CHECK(ht2mp::protocol::validate(hello, *gog));
    hello.vehicle_type = 1;
    HT2MP_CHECK(!ht2mp::protocol::validate(hello, *gog));
    hello.profile_id = "steam-8138acee";
    hello.vehicle_type = 62;
    HT2MP_CHECK(ht2mp::protocol::validate(hello, *steam));
    hello.vehicle_type = 87;
    HT2MP_CHECK(ht2mp::protocol::validate(hello, *steam));
    for (const auto tractor : {88U, 100U, 142U}) {
        hello.vehicle_type = static_cast<std::uint16_t>(tractor);
        HT2MP_CHECK(!ht2mp::protocol::validate(hello, *steam));
    }
    hello.vehicle_type = 4096;
    HT2MP_CHECK(!ht2mp::protocol::validate(hello, *steam));
    hello.vehicle_type = 62;
    hello.paint_variant = 4;
    HT2MP_CHECK(!ht2mp::protocol::validate(hello, *steam));
    return true;
}

}  // namespace

int main() {
    int failures = 0;
    failures += run_test("UTF-8", utf8_validation);
    failures += run_test("ClientHello", hello_validation);
    failures += run_test("PlayerState", state_validation);
    failures += run_test("exact profile limits", exact_profile_limits);
    return failures == 0 ? 0 : 1;
}
