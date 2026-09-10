#include "test_support.hpp"

#include "ht2mp/protocol/codec.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

ht2mp::protocol::PlayerState make_state(std::uint64_t player = 2) {
    ht2mp::protocol::PlayerState state;
    state.session_id = 1;
    state.player_id = player;
    state.incarnation_id = player + 100;
    state.sequence = 42;
    state.sample_time_ms = 123456;
    state.flags = ht2mp::protocol::flag(ht2mp::protocol::PlayerStateFlag::in_world);
    state.position = {1.25, -2.5, 3.75};
    state.orientation = {0.0F, 0.70710677F, 0.0F, 0.70710677F};
    state.linear_velocity = {10.0F, 0.0F, -2.0F};
    state.angular_velocity = {0.0F, 0.1F, 0.0F};
    state.vehicle_type = 7;
    state.paint_variant = 3;
    state.vehicle.valid = true;
    state.vehicle.steering = -98;
    state.vehicle.lights = {1, 1, 0, 2};
    state.vehicle.condition[7] = 0.8F;
    state.vehicle.wheels = {4, {12.5F, -9.0F, 0.0F, 127.0F}};
    state.vehicle.horn = {true, 2, 65535};
    state.location = {8, 9, 10, 11.5, 12, 13, 14, 15};
    return state;
}

template <typename T>
bool round_trip(const T& value) {
    const ht2mp::protocol::Message original = value;
    const auto encoded = ht2mp::protocol::encode_packet(original);
    HT2MP_CHECK(encoded);
    HT2MP_CHECK(encoded.bytes.size() <= ht2mp::protocol::kMaxPacketSize);
    const auto decoded = ht2mp::protocol::decode_packet(encoded.bytes);
    HT2MP_CHECK(decoded);
    HT2MP_CHECK(decoded.message == original);
    return true;
}

bool all_messages_round_trip() {
    ht2mp::protocol::Token128 token{};
    for (std::size_t index = 0; index < token.size(); ++index) {
        token[index] = static_cast<std::byte>(index);
    }
    HT2MP_CHECK(round_trip(ht2mp::protocol::ClientHello{"gog-05588140", token, "Driver", 55, 7, 3}));
    HT2MP_CHECK(round_trip(ht2mp::protocol::ServerWelcome{
        "gog-05588140", 1, 2, 1000,
        static_cast<std::uint8_t>(ht2mp::protocol::kMaxPlayers), 20}));
    HT2MP_CHECK(round_trip(ht2mp::protocol::PeerJoined{2, 55, 7, "Driver", 3}));
    HT2MP_CHECK(round_trip(ht2mp::protocol::PeerLeft{2, 55, ht2mp::protocol::DisconnectReason::normal}));
    HT2MP_CHECK(round_trip(ht2mp::protocol::SpawnMetadata{2, 55, 7, "Driver", 3}));
    HT2MP_CHECK(round_trip(ht2mp::protocol::Ping{0x0102030405060708ULL, 0x1112131415161718ULL}));
    HT2MP_CHECK(round_trip(ht2mp::protocol::Pong{9, 10, 11}));
    HT2MP_CHECK(round_trip(
        ht2mp::protocol::Disconnect{ht2mp::protocol::DisconnectReason::profile_mismatch, "wrong profile"}));
    HT2MP_CHECK(round_trip(make_state()));
    HT2MP_CHECK(round_trip(ht2mp::protocol::WorldSnapshot{999, {make_state(2), make_state(3)}}));
    return true;
}

bool golden_little_endian_ping() {
    constexpr std::array<std::uint8_t, 28> expected{
        0x48, 0x32, 0x4D, 0x50, 0x06, 0x06, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
    };
    const auto encoded = ht2mp::protocol::encode_packet(
        ht2mp::protocol::Ping{0x0102030405060708ULL, 0x1112131415161718ULL});
    HT2MP_CHECK(encoded);
    HT2MP_CHECK(encoded.bytes.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        HT2MP_CHECK(std::to_integer<std::uint8_t>(encoded.bytes[index]) == expected[index]);
    }
    return true;
}

bool malformed_packets_are_rejected() {
    auto packet = ht2mp::protocol::encode_packet(ht2mp::protocol::Ping{1, 2}).bytes;
    HT2MP_CHECK(!ht2mp::protocol::decode_packet(std::span(packet).first(5)));

    auto oversized = std::vector<std::byte>(ht2mp::protocol::kMaxPacketSize + 1);
    HT2MP_CHECK(ht2mp::protocol::decode_packet(oversized).error == ht2mp::protocol::DecodeError::too_large);

    auto changed = packet;
    changed[0] = std::byte{0};
    HT2MP_CHECK(ht2mp::protocol::decode_packet(changed).error == ht2mp::protocol::DecodeError::bad_magic);
    changed = packet;
    changed[4] = std::byte{3};
    HT2MP_CHECK(ht2mp::protocol::decode_packet(changed).error ==
                ht2mp::protocol::DecodeError::unsupported_version);
    changed = packet;
    changed[5] = std::byte{0x7F};
    HT2MP_CHECK(ht2mp::protocol::decode_packet(changed).error == ht2mp::protocol::DecodeError::unknown_message);
    changed = packet;
    changed[6] = std::byte{1};
    HT2MP_CHECK(ht2mp::protocol::decode_packet(changed).error == ht2mp::protocol::DecodeError::invalid_header);
    changed = packet;
    changed.pop_back();
    HT2MP_CHECK(ht2mp::protocol::decode_packet(changed).error == ht2mp::protocol::DecodeError::truncated);
    changed = packet;
    changed.push_back(std::byte{0});
    HT2MP_CHECK(ht2mp::protocol::decode_packet(changed).error == ht2mp::protocol::DecodeError::trailing_data);

    ht2mp::protocol::Token128 token{};
    changed = ht2mp::protocol::encode_packet(
                  ht2mp::protocol::ClientHello{"gog-05588140", token, "x", 1, 0})
                  .bytes;
    changed.back() = std::byte{0xFF};
    HT2MP_CHECK(ht2mp::protocol::decode_packet(changed).error == ht2mp::protocol::DecodeError::invalid_value);
    changed = ht2mp::protocol::encode_packet(make_state()).bytes;
    changed[changed.size() - 3] = std::byte{6}; // invalid horn tone / reserved flags
    HT2MP_CHECK(!ht2mp::protocol::decode_packet(changed));
    return true;
}

bool packet_budget_and_invalid_encode() {
    ht2mp::protocol::WorldSnapshot snapshot;
    snapshot.server_time_ms = 1000;
    for (std::uint64_t player = 1; player <= ht2mp::protocol::kMaxSnapshotPlayers; ++player) {
        snapshot.players.push_back(make_state(player));
    }
    const auto encoded = ht2mp::protocol::encode_packet(snapshot);
    HT2MP_CHECK(encoded);
    HT2MP_CHECK(encoded.bytes.size() == 1200);
    snapshot.players.push_back(make_state(9));
    HT2MP_CHECK(!ht2mp::protocol::encode_packet(snapshot));

    auto invalid = make_state();
    invalid.position.x = (std::numeric_limits<double>::quiet_NaN)();
    HT2MP_CHECK(!ht2mp::protocol::encode_packet(invalid));
    return true;
}

}  // namespace

int main() {
    int failures = 0;
    failures += run_test("all messages round trip", all_messages_round_trip);
    failures += run_test("golden little-endian ping", golden_little_endian_ping);
    failures += run_test("malformed packets", malformed_packets_are_rejected);
    failures += run_test("packet budget", packet_budget_and_invalid_encode);
    return failures == 0 ? 0 : 1;
}
