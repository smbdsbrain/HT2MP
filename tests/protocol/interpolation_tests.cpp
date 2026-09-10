#include "test_support.hpp"

#include "ht2mp/protocol/interpolation.hpp"

#include <cmath>
#include <cstdint>

namespace {

ht2mp::protocol::PlayerState state(std::uint32_t sequence, std::uint64_t time, double x) {
    ht2mp::protocol::PlayerState value;
    value.session_id = 1;
    value.player_id = 2;
    value.incarnation_id = 3;
    value.sequence = sequence;
    value.sample_time_ms = time;
    value.set(ht2mp::protocol::PlayerStateFlag::in_world, true);
    value.position.x = x;
    value.orientation.w = 1.0F;
    value.linear_velocity.x = 100.0F;
    value.location.room_id = 1;
    value.location.road_id = 1;
    value.location.node_id = 1;
    value.location.road_segment_vector_id = 1;
    value.location.road_segment_id = 1;
    return value;
}

bool interpolation_and_extrapolation() {
    ht2mp::protocol::RemoteTimeline timeline;
    HT2MP_CHECK(timeline.push(state(1, 1000, 0.0)));
    auto second = state(2, 1100, 10.0);
    second.location.road_distance = 42.5;
    HT2MP_CHECK(timeline.push(second));
    const auto interpolated = timeline.sample(1150);  // playback target = 1050
    HT2MP_CHECK(interpolated.state == ht2mp::protocol::PlaybackState::interpolated);
    HT2MP_CHECK(interpolated.value);
    HT2MP_CHECK(std::abs(interpolated.value->position.x - 5.0) < 0.001);

    const auto extrapolated = timeline.sample(1250);  // playback target = 1150
    HT2MP_CHECK(extrapolated.state == ht2mp::protocol::PlaybackState::extrapolated);
    HT2MP_CHECK(extrapolated.value);
    HT2MP_CHECK(std::abs(extrapolated.value->position.x - 15.0) < 0.001);
    HT2MP_CHECK(std::abs(extrapolated.value->location.road_distance -
                         second.location.road_distance) < 0.001);
    return true;
}

bool timeout_states() {
    ht2mp::protocol::RemoteTimeline timeline;
    HT2MP_CHECK(timeline.push(state(1, 1000, 0.0)));
    HT2MP_CHECK(timeline.sample(1500).state == ht2mp::protocol::PlaybackState::frozen);
    HT2MP_CHECK(timeline.sample(4000).state == ht2mp::protocol::PlaybackState::hidden);
    HT2MP_CHECK(timeline.sample(6000).state == ht2mp::protocol::PlaybackState::despawn);

    timeline.clear();
    auto hidden = state(1, 1000, 0.0);
    hidden.set(ht2mp::protocol::PlayerStateFlag::in_world, false);
    HT2MP_CHECK(timeline.push(hidden));
    HT2MP_CHECK(timeline.sample(1000).state == ht2mp::protocol::PlaybackState::hidden);
    return true;
}

bool reset_and_sequence_rules() {
    ht2mp::protocol::RemoteTimeline timeline;
    HT2MP_CHECK(timeline.push(state(0xFFFFFFFEU, 1000, 0.0)));
    HT2MP_CHECK(timeline.push(state(1, 1100, 10.0)));
    HT2MP_CHECK(ht2mp::protocol::sequence_newer(1, 0xFFFFFFFEU));
    HT2MP_CHECK(!ht2mp::protocol::sequence_newer(0xFFFFFFFEU, 1));
    HT2MP_CHECK(!ht2mp::protocol::sequence_newer(0x80000001U, 1U));
    HT2MP_CHECK(!timeline.push(state(1, 1200, 20.0)));

    auto room_change = state(2, 1300, 30.0);
    room_change.location.room_id = 2;
    HT2MP_CHECK(timeline.push(room_change));
    HT2MP_CHECK(timeline.size() == 1);

    auto incarnation = state(1, 1400, 40.0);
    incarnation.incarnation_id = 99;
    HT2MP_CHECK(timeline.push(incarnation));
    HT2MP_CHECK(timeline.size() == 1);
    HT2MP_CHECK(timeline.incarnation_id() == 99);
    return true;
}

bool deterministic_loss_reorder_and_jitter() {
    ht2mp::protocol::RemoteTimeline timeline;
    std::uint64_t time = 1000;
    std::uint32_t accepted = 0;
    for (std::uint32_t sequence = 1; sequence <= 200; ++sequence) {
        time += 20 + ((sequence * 37) % 81);  // Deterministic 20..100 ms arrival jitter.
        if (sequence % 20 == 0) {             // Exactly 5% packet loss.
            continue;
        }
        auto value = state(sequence, time, static_cast<double>(sequence));
        HT2MP_CHECK(timeline.push(value));
        ++accepted;
        if (sequence == 51) {
            auto reordered = state(50, time + 1, 50.0);
            HT2MP_CHECK(!timeline.push(reordered));
        }
    }
    HT2MP_CHECK(accepted == 190);
    HT2MP_CHECK(timeline.size() == 64);
    const auto playback = timeline.sample(time + 100);
    HT2MP_CHECK(playback.state == ht2mp::protocol::PlaybackState::interpolated ||
                playback.state == ht2mp::protocol::PlaybackState::extrapolated);
    HT2MP_CHECK(playback.value);
    HT2MP_CHECK(std::isfinite(playback.value->position.x));
    return true;
}

}  // namespace

int main() {
    int failures = 0;
    failures += run_test("interpolation and extrapolation", interpolation_and_extrapolation);
    failures += run_test("timeouts", timeout_states);
    failures += run_test("reset and sequence", reset_and_sequence_rules);
    failures += run_test("loss, reorder, and jitter", deterministic_loss_reorder_and_jitter);
    return failures == 0 ? 0 : 1;
}
