#include "test_support.hpp"

#include "session.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace {

ht2mp::protocol::Token128 token() {
    ht2mp::protocol::Token128 result{};
    result[0] = std::byte{0x42};
    return result;
}

ht2mp::coordinator::Session session(
    std::uint8_t maximum =
        static_cast<std::uint8_t>(ht2mp::protocol::kMaxPlayers)) {
    ht2mp::coordinator::SessionConfig config;
    config.profile_id = "gog-05588140";
    config.token = token();
    config.session_id = 77;
    config.max_players = maximum;
    return ht2mp::coordinator::Session(std::move(config));
}

ht2mp::protocol::ClientHello hello(std::string name, std::uint64_t incarnation) {
    return {"gog-05588140", token(), std::move(name), incarnation, 4};
}

ht2mp::protocol::PlayerState state(std::uint64_t player,
                                   std::uint64_t incarnation,
                                   std::uint32_t sequence) {
    ht2mp::protocol::PlayerState value;
    value.session_id = 77;
    value.player_id = player;
    value.incarnation_id = incarnation;
    value.sequence = sequence;
    value.sample_time_ms = 1;
    value.orientation.w = 1.0F;
    value.vehicle_type = 4;
    value.location.room_id = 0;
    value.location.road_id = 0;
    value.location.node_id = 0;
    value.location.road_segment_vector_id = 0;
    value.location.road_segment_id = 0;
    return value;
}

bool admission_and_membership() {
    auto game = session(2);
    const auto first = game.accept(10, hello("Alice", 100), 1000);
    HT2MP_CHECK(first.accepted);
    HT2MP_CHECK(game.size() == 1);
    HT2MP_CHECK(std::holds_alternative<ht2mp::protocol::ServerWelcome>(first.routes.front().message));

    const auto second = game.accept(20, hello("Bob", 200), 1001);
    HT2MP_CHECK(second.accepted);
    HT2MP_CHECK(game.size() == 2);
    HT2MP_CHECK(second.routes.size() == 5);  // welcome + joined/spawn in both directions

    const auto full = game.accept(30, hello("Carol", 300), 1002);
    HT2MP_CHECK(!full.accepted);
    HT2MP_CHECK(full.reason == ht2mp::protocol::DisconnectReason::session_full);

    const auto left = game.disconnect(10, ht2mp::protocol::DisconnectReason::normal);
    HT2MP_CHECK(game.size() == 1);
    HT2MP_CHECK(left.size() == 1);
    HT2MP_CHECK(std::holds_alternative<ht2mp::protocol::PeerLeft>(left.front().message));
    return true;
}

bool admission_rejections() {
    auto game = session();
    auto wrong_profile = hello("Alice", 100);
    wrong_profile.profile_id = "steam-8138acee";
    HT2MP_CHECK(game.accept(1, wrong_profile, 0).reason ==
                ht2mp::protocol::DisconnectReason::profile_mismatch);

    auto wrong_token = hello("Alice", 100);
    wrong_token.token[1] = std::byte{1};
    HT2MP_CHECK(game.accept(2, wrong_token, 0).reason == ht2mp::protocol::DisconnectReason::token_mismatch);
    HT2MP_CHECK(game.accept(3, hello("Alice", 100), 0).accepted);
    HT2MP_CHECK(game.accept(4, hello("Alice", 101), 0).reason ==
                ht2mp::protocol::DisconnectReason::duplicate_name);
    return true;
}

bool public_admission_ignores_tokens() {
    ht2mp::coordinator::SessionConfig config;
    config.profile_id = "gog-05588140";
    config.token = token();
    config.authentication =
        ht2mp::coordinator::AuthenticationMode::public_access;
    config.session_id = 88U;
    auto game = ht2mp::coordinator::Session(std::move(config));

    auto no_token = hello("PublicZero", 1U);
    no_token.token = {};
    HT2MP_CHECK(game.accept(1U, no_token, 100U).accepted);

    auto arbitrary_token = hello("PublicOther", 2U);
    arbitrary_token.token[3] = std::byte{0x99};
    HT2MP_CHECK(game.accept(2U, arbitrary_token, 101U).accepted);
    return true;
}

bool exactly_sixty_four_players_and_sixty_fifth_rejected() {
    auto game = session();
    for (std::uint64_t index = 1;
         index <= ht2mp::protocol::kMaxPlayers; ++index) {
        const auto result = game.accept(100 + index, hello("Player" + std::to_string(index), 1000 + index), 2000 + index);
        HT2MP_CHECK(result.accepted);
        HT2MP_CHECK(game.size() == index);
        HT2MP_CHECK(result.routes.size() == 1 + 4 * (index - 1));
    }
    HT2MP_CHECK(game.size() == ht2mp::protocol::kMaxPlayers);

    const auto overflow = game.accept(999U, hello("Player65", 1065U), 2065U);
    HT2MP_CHECK(!overflow.accepted);
    HT2MP_CHECK(overflow.reason == ht2mp::protocol::DisconnectReason::session_full);
    HT2MP_CHECK(overflow.routes.size() == 1);
    const auto* rejection = std::get_if<ht2mp::protocol::Disconnect>(&overflow.routes.front().message);
    HT2MP_CHECK(rejection != nullptr);
    HT2MP_CHECK(rejection->reason == ht2mp::protocol::DisconnectReason::session_full);
    HT2MP_CHECK(game.size() == ht2mp::protocol::kMaxPlayers);

    const auto left = game.disconnect(101U, ht2mp::protocol::DisconnectReason::normal);
    HT2MP_CHECK(left.size() == ht2mp::protocol::kMaxPlayers - 1U);
    HT2MP_CHECK(game.size() == ht2mp::protocol::kMaxPlayers - 1U);
    const auto replacement = game.accept(1000U, hello("Replacement", 2000U), 3000U);
    HT2MP_CHECK(replacement.accepted);
    HT2MP_CHECK(game.size() == ht2mp::protocol::kMaxPlayers);
    return true;
}

bool leave_reconnect_and_stale_incarnation() {
    auto game = session();
    HT2MP_CHECK(game.accept(10, hello("Observer", 500), 1000).accepted);
    HT2MP_CHECK(game.accept(20, hello("Alice", 100), 1001).accepted);
    const auto old_alice = game.peer(20);
    HT2MP_CHECK(old_alice);
    HT2MP_CHECK(game.receive(20, state(old_alice->player_id, old_alice->incarnation_id, 1), 1010).accepted);

    const auto leave_routes = game.disconnect(20, ht2mp::protocol::DisconnectReason::normal);
    HT2MP_CHECK(leave_routes.size() == 1);
    HT2MP_CHECK(leave_routes.front().target == 10);
    const auto* left = std::get_if<ht2mp::protocol::PeerLeft>(&leave_routes.front().message);
    HT2MP_CHECK(left != nullptr);
    HT2MP_CHECK(left->player_id == old_alice->player_id);
    HT2MP_CHECK(left->incarnation_id == 100);

    const auto reconnect = game.accept(30, hello("Alice", 200), 1100);
    HT2MP_CHECK(reconnect.accepted);
    const auto new_alice = game.peer(30);
    HT2MP_CHECK(new_alice);
    HT2MP_CHECK(new_alice->player_id != old_alice->player_id);
    HT2MP_CHECK(new_alice->incarnation_id == 200);

    bool observer_saw_new_incarnation = false;
    for (const auto& route : reconnect.routes) {
        if (route.target != 10) {
            continue;
        }
        if (const auto* joined = std::get_if<ht2mp::protocol::PeerJoined>(&route.message)) {
            observer_saw_new_incarnation = joined->player_id == new_alice->player_id && joined->incarnation_id == 200;
        }
    }
    HT2MP_CHECK(observer_saw_new_incarnation);

    const auto stale_connection = game.receive(20, state(old_alice->player_id, 100, 2), 1110);
    HT2MP_CHECK(!stale_connection.accepted);
    HT2MP_CHECK(stale_connection.reason == ht2mp::protocol::DisconnectReason::protocol_error);

    const auto stale_incarnation = game.receive(30, state(new_alice->player_id, 100, 2), 1111);
    HT2MP_CHECK(!stale_incarnation.accepted);
    HT2MP_CHECK(stale_incarnation.reason == ht2mp::protocol::DisconnectReason::protocol_error);

    HT2MP_CHECK(game.receive(30, state(new_alice->player_id, 200, 1), 1112).accepted);
    const auto snapshots = game.tick(1112);
    HT2MP_CHECK(snapshots.size() == 2);
    for (const auto& route : snapshots) {
        const auto* snapshot = std::get_if<ht2mp::protocol::WorldSnapshot>(&route.message);
        HT2MP_CHECK(snapshot != nullptr);
        bool contains_old = false;
        bool contains_new = false;
        for (const auto& player : snapshot->players) {
            contains_old = contains_old || player.incarnation_id == 100;
            contains_new = contains_new || player.player_id == new_alice->player_id && player.incarnation_id == 200;
        }
        HT2MP_CHECK(!contains_old);
        HT2MP_CHECK(contains_new);
    }
    return true;
}

bool identity_rate_and_snapshot() {
    auto game = session();
    HT2MP_CHECK(game.accept(1, hello("Alice", 100), 1000).accepted);
    const auto info = game.peer(1);
    HT2MP_CHECK(info);

    auto spoofed = state(info->player_id + 1, info->incarnation_id, 1);
    const auto spoof_result = game.receive(1, spoofed, 1001);
    HT2MP_CHECK(!spoof_result.accepted);

    for (std::uint32_t sequence = 1; sequence <= 41; ++sequence) {
        HT2MP_CHECK(game.receive(1, state(info->player_id, info->incarnation_id, sequence), 1100).accepted);
    }
    const auto routes = game.tick(1100);
    HT2MP_CHECK(routes.size() == 1);
    const auto* snapshot = std::get_if<ht2mp::protocol::WorldSnapshot>(&routes.front().message);
    HT2MP_CHECK(snapshot != nullptr);
    HT2MP_CHECK(snapshot->players.size() == 1);
    HT2MP_CHECK(snapshot->players.front().sequence == 40);  // 41st packet was rate-limited.
    HT2MP_CHECK(snapshot->players.front().sample_time_ms == 1);  // sender clock preserved
    HT2MP_CHECK(game.tick(1120).empty());
    return true;
}

bool state_fans_out_to_other_peers_only() {
    auto game = session();
    HT2MP_CHECK(game.accept(1, hello("Alice", 100), 1000).accepted);
    HT2MP_CHECK(game.accept(2, hello("Bob", 200), 1001).accepted);
    HT2MP_CHECK(game.accept(3, hello("Carol", 300), 1002).accepted);
    const auto alice = game.peer(1);
    HT2MP_CHECK(alice);

    const auto first = game.receive(1, state(alice->player_id, alice->incarnation_id, 1), 1100);
    HT2MP_CHECK(first.accepted);
    HT2MP_CHECK(first.routes.size() == 2);
    for (const auto& route : first.routes) {
        HT2MP_CHECK(route.target == 2 || route.target == 3);
        const auto* relayed = std::get_if<ht2mp::protocol::PlayerState>(&route.message);
        HT2MP_CHECK(relayed != nullptr);
        HT2MP_CHECK(relayed->sequence == 1 && relayed->sample_time_ms == 1);
    }
    // Stale or duplicate sequences are neither stored nor relayed.
    const auto duplicate = game.receive(1, state(alice->player_id, alice->incarnation_id, 1), 1101);
    HT2MP_CHECK(duplicate.accepted && duplicate.routes.empty());
    // Rate-limited states are dropped silently, without fan-out.
    for (std::uint32_t sequence = 2; sequence <= 40; ++sequence) {
        HT2MP_CHECK(game.receive(1, state(alice->player_id, alice->incarnation_id, sequence), 1150).accepted);
    }
    const auto limited = game.receive(1, state(alice->player_id, alice->incarnation_id, 41), 1150);
    HT2MP_CHECK(limited.accepted && limited.routes.empty());
    return true;
}

bool stale_states_leave_snapshots() {
    auto game = session();
    HT2MP_CHECK(game.accept(1, hello("Alice", 100), 1000).accepted);
    HT2MP_CHECK(game.accept(2, hello("Bob", 200), 1001).accepted);
    const auto alice = game.peer(1);
    HT2MP_CHECK(alice);
    HT2MP_CHECK(game.receive(1, state(alice->player_id, alice->incarnation_id, 1), 1100).accepted);
    auto routes = game.tick(1150);
    HT2MP_CHECK(routes.size() == 2);
    const auto* snapshot = std::get_if<ht2mp::protocol::WorldSnapshot>(&routes.front().message);
    HT2MP_CHECK(snapshot != nullptr && snapshot->players.size() == 1);
    // Three seconds without a newer state: the peer is omitted entirely.
    routes = game.tick(4200);
    HT2MP_CHECK(routes.size() == 2);
    const auto& empty = std::get<ht2mp::protocol::WorldSnapshot>(routes.front().message);
    HT2MP_CHECK(empty.players.empty() && empty.environment.session_id == 77);
    return true;
}

bool rate_limit_is_a_rolling_window() {
    auto game = session();
    HT2MP_CHECK(game.accept(1, hello("Burst", 100), 0).accepted);
    const auto info = game.peer(1);
    HT2MP_CHECK(info);

    for (std::uint32_t sequence = 1; sequence <= 40; ++sequence) {
        HT2MP_CHECK(game.receive(
            1, state(info->player_id, info->incarnation_id, sequence), 999)
                        .accepted);
    }
    // Crossing an arbitrary fixed-second boundary must not permit a second
    // burst inside the same rolling one-second interval.
    for (std::uint32_t sequence = 41; sequence <= 80; ++sequence) {
        HT2MP_CHECK(game.receive(
            1, state(info->player_id, info->incarnation_id, sequence), 1000)
                        .accepted);
    }
    auto routes = game.tick(1000);
    HT2MP_CHECK(routes.size() == 1);
    auto* snapshot =
        std::get_if<ht2mp::protocol::WorldSnapshot>(&routes.front().message);
    HT2MP_CHECK(snapshot != nullptr && snapshot->players.size() == 1);
    HT2MP_CHECK(snapshot->players.front().sequence == 40);

    // At exactly one second of age the old burst leaves the window.
    HT2MP_CHECK(game.receive(
        1, state(info->player_id, info->incarnation_id, 81), 1999)
                    .accepted);
    routes = game.tick(1999);
    HT2MP_CHECK(routes.size() == 1);
    snapshot =
        std::get_if<ht2mp::protocol::WorldSnapshot>(&routes.front().message);
    HT2MP_CHECK(snapshot != nullptr && snapshot->players.front().sequence == 81);
    return true;
}

bool steam_appearance_allowlist_and_immutability() {
    ht2mp::coordinator::SessionConfig config;
    config.profile_id = "steam-8138acee";
    config.token = token();
    config.session_id = 91U;
    config.limits = *ht2mp::protocol::limits_for_profile(config.profile_id);
    ht2mp::coordinator::Session game(std::move(config));

    ht2mp::protocol::ClientHello selected{
        "steam-8138acee", token(), "Driver", 501U, 62U, 3U};
    auto tractor = selected;
    tractor.vehicle_type = 88U;
    HT2MP_CHECK(!game.accept(1U, tractor, 100U).accepted);
    auto unknown = selected;
    unknown.vehicle_type = 4096U;
    HT2MP_CHECK(!game.accept(2U, unknown, 100U).accepted);
    HT2MP_CHECK(game.accept(3U, selected, 100U).accepted);
    const auto peer = game.peer(3U);
    HT2MP_CHECK(peer);
    if (!peer) return false;
    HT2MP_CHECK(peer->vehicle_type == 62U && peer->paint_variant == 3U);

    ht2mp::protocol::PlayerState update;
    update.session_id = 91U;
    update.player_id = peer->player_id;
    update.incarnation_id = peer->incarnation_id;
    update.sequence = 1U;
    update.orientation.w = 1.0F;
    update.vehicle_type = 62U;
    update.paint_variant = 3U;
    HT2MP_CHECK(game.receive(3U, update, 110U).accepted);
    update.sequence = 2U;
    update.paint_variant = 2U;
    HT2MP_CHECK(!game.receive(3U, update, 120U).accepted);
    update.paint_variant = 3U;
    update.vehicle_type = 63U;
    HT2MP_CHECK(!game.receive(3U, update, 130U).accepted);
    return true;
}

}  // namespace

int main() {
    int failures = 0;
    failures += run_test("admission and membership", admission_and_membership);
    failures += run_test("admission rejections", admission_rejections);
    failures += run_test("public admission ignores tokens", public_admission_ignores_tokens);
    failures += run_test("exactly 64 players", exactly_sixty_four_players_and_sixty_fifth_rejected);
    failures += run_test("leave, reconnect, stale incarnation", leave_reconnect_and_stale_incarnation);
    failures += run_test("identity, rate, snapshot", identity_rate_and_snapshot);
    failures += run_test("state fan-out", state_fans_out_to_other_peers_only);
    failures += run_test("stale states leave snapshots", stale_states_leave_snapshots);
    failures += run_test("rolling state-rate limit", rate_limit_is_a_rolling_window);
    failures += run_test("Steam appearance allowlist and immutability",
                         steam_appearance_allowlist_and_immutability);
    return failures == 0 ? 0 : 1;
}
