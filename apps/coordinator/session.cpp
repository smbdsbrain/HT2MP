#include "session.hpp"

#include "ht2mp/protocol/interpolation.hpp"

#include <algorithm>
#include <utility>

namespace ht2mp::coordinator {
namespace {

protocol::PeerJoined joined(const PeerInfo& info) {
    return {info.player_id, info.incarnation_id, info.vehicle_type, info.display_name, info.paint_variant};
}

protocol::SpawnMetadata spawn(const PeerInfo& info) {
    return {info.player_id, info.incarnation_id, info.vehicle_type, info.display_name, info.paint_variant};
}

}  // namespace

Session::Session(SessionConfig config) : config_(std::move(config)) {
    config_.max_players = std::clamp<std::uint8_t>(config_.max_players, 1, protocol::kMaxPlayers);
    config_.state_hz = std::clamp<std::uint8_t>(config_.state_hz, 1, 40);
    config_.max_state_updates_per_second = std::max<std::uint8_t>(config_.max_state_updates_per_second, 1);
    if (config_.session_id == 0) {
        config_.session_id = 1;
    }
    if (!protocol::valid_environment(config_.environment)) config_.environment = {};
    config_.environment.session_id = config_.session_id;
}

DispatchResult Session::accept(ConnectionId connection,
                               const protocol::ClientHello& hello,
                               std::uint64_t server_time_ms) {
    if (connection == 0 || contains(connection)) {
        return rejected(connection, protocol::DisconnectReason::protocol_error, "connection is already registered");
    }
    if (const auto result = protocol::validate(hello, config_.limits); !result) {
        return rejected(connection, protocol::DisconnectReason::protocol_error, result.detail);
    }
    if (hello.profile_id != config_.profile_id) {
        return rejected(connection, protocol::DisconnectReason::profile_mismatch, "game profile does not match session");
    }
    if (config_.authentication == AuthenticationMode::token &&
        hello.token != config_.token) {
        return rejected(connection, protocol::DisconnectReason::token_mismatch, "session token does not match");
    }
    if (peers_.size() >= config_.max_players) {
        return rejected(connection, protocol::DisconnectReason::session_full,
                        "session already has " + std::to_string(config_.max_players) + " players");
    }
    const auto duplicate = std::find_if(peers_.begin(), peers_.end(), [&hello](const auto& item) {
        return item.second.info.display_name == hello.display_name;
    });
    if (duplicate != peers_.end()) {
        return rejected(connection, protocol::DisconnectReason::duplicate_name, "display name is already in use");
    }

    PeerInfo info{connection, next_player_id_++, hello.incarnation_id, hello.vehicle_type,
                  hello.paint_variant, hello.display_name};
    DispatchResult result;
    result.routes.push_back({connection,
                             protocol::ServerWelcome{config_.profile_id,
                                                     config_.session_id,
                                                     info.player_id,
                                                     server_time_ms,
                                                     config_.max_players,
                                                     config_.state_hz}});
    for (const auto& [existing_connection, existing] : peers_) {
        result.routes.push_back({connection, joined(existing.info)});
        result.routes.push_back({connection, spawn(existing.info)});
        result.routes.push_back({existing_connection, joined(info)});
        result.routes.push_back({existing_connection, spawn(info)});
    }
    Peer peer;
    peer.info = info;
    peers_.emplace(connection, std::move(peer));
    if (!environment_epoch_ms_) environment_epoch_ms_ = server_time_ms;
    return result;
}

DispatchResult Session::receive(ConnectionId connection,
                                const protocol::Message& message,
                                std::uint64_t server_time_ms) {
    auto iterator = peers_.find(connection);
    if (iterator == peers_.end()) {
        return rejected(connection, protocol::DisconnectReason::protocol_error, "ClientHello is required first");
    }
    auto& peer = iterator->second;

    if (const auto* ping = std::get_if<protocol::Ping>(&message)) {
        DispatchResult result;
        result.routes.push_back(
            {connection, protocol::Pong{ping->nonce, ping->client_time_ms, server_time_ms}});
        return result;
    }
    if (std::holds_alternative<protocol::Disconnect>(message)) {
        DispatchResult result;
        result.accepted = false;
        result.reason = protocol::DisconnectReason::normal;
        result.detail = "client requested disconnect";
        return result;
    }
    const auto* state = std::get_if<protocol::PlayerState>(&message);
    if (state == nullptr) {
        return rejected(connection, protocol::DisconnectReason::protocol_error, "message is not valid client traffic");
    }
    if (const auto validation = protocol::validate(*state, config_.limits); !validation) {
        return rejected(connection, protocol::DisconnectReason::protocol_error, validation.detail);
    }
    if (state->session_id != config_.session_id || state->player_id != peer.info.player_id ||
        state->incarnation_id != peer.info.incarnation_id || state->vehicle_type != peer.info.vehicle_type ||
        state->paint_variant != peer.info.paint_variant) {
        return rejected(connection, protocol::DisconnectReason::protocol_error, "player state attempts to spoof identity");
    }

    while (!peer.accepted_update_times_ms.empty() &&
           server_time_ms >= peer.accepted_update_times_ms.front() &&
           server_time_ms - peer.accepted_update_times_ms.front() >= 1000U) {
        peer.accepted_update_times_ms.pop_front();
    }
    if (peer.accepted_update_times_ms.size() >=
        config_.max_state_updates_per_second) {
        return {};  // Drop excess state without amplifying traffic.
    }
    if (peer.latest && !protocol::sequence_newer(state->sequence, peer.latest->sequence)) {
        return {};  // Duplicate, reordered, or stale datagram.
    }

    peer.accepted_update_times_ms.push_back(server_time_ms);
    // sample_time_ms stays on the sender's clock: receivers estimate the
    // offset themselves, which is what makes playback timing independent of
    // relay jitter. The coordinator only records when it saw the state.
    peer.latest = *state;
    peer.latest_received_ms = server_time_ms;
    // Fan the accepted state out immediately on the unreliable state channel;
    // the periodic snapshot below remains the repair/baseline path.
    DispatchResult result;
    for (const auto& [other_connection, other] : peers_) {
        static_cast<void>(other);
        if (other_connection != connection) {
            result.routes.push_back({other_connection, *state});
        }
    }
    return result;
}

std::vector<Route> Session::disconnect(ConnectionId connection, protocol::DisconnectReason reason) {
    const auto iterator = peers_.find(connection);
    if (iterator == peers_.end()) {
        return {};
    }
    const protocol::PeerLeft left{iterator->second.info.player_id, iterator->second.info.incarnation_id, reason};
    peers_.erase(iterator);

    std::vector<Route> routes;
    routes.reserve(peers_.size());
    for (const auto& [target, unused] : peers_) {
        static_cast<void>(unused);
        routes.push_back({target, left});
    }
    return routes;
}

std::vector<Route> Session::tick(std::uint64_t server_time_ms) {
    const std::uint64_t interval = 1000U / config_.state_hz;
    if (next_snapshot_ms_ != 0 && server_time_ms < next_snapshot_ms_) {
        return {};
    }
    next_snapshot_ms_ = server_time_ms + interval;

    protocol::WorldSnapshot snapshot;
    snapshot.server_time_ms = server_time_ms;
    if (!environment_epoch_ms_) environment_epoch_ms_ = server_time_ms;
    const auto elapsed = server_time_ms >= *environment_epoch_ms_
                             ? server_time_ms - *environment_epoch_ms_ : 0U;
    snapshot.environment = protocol::advance_environment(config_.environment,
                                                         static_cast<double>(elapsed) / 1000.0);
    snapshot.players.reserve(peers_.size());
    for (const auto& [unused, peer] : peers_) {
        static_cast<void>(unused);
        if (peer.latest && server_time_ms >= peer.latest_received_ms &&
            server_time_ms - peer.latest_received_ms < config_.stale_state_ms) {
            snapshot.players.push_back(*peer.latest);
        }
    }
    std::sort(snapshot.players.begin(), snapshot.players.end(), [](const auto& first, const auto& second) {
        return first.player_id < second.player_id;
    });

    std::vector<Route> routes;
    // Extended vehicle condition fits at most three states in a 1200-byte packet.
    // Each chunk is independently useful; clients never interpret an omitted
    // player as a despawn. Empty rooms still receive the environment clock.
    for (std::size_t begin = 0;; begin += protocol::kMaxSnapshotPlayers) {
        protocol::WorldSnapshot chunk;
        chunk.server_time_ms = snapshot.server_time_ms;
        chunk.environment = snapshot.environment;
        const auto end = std::min(begin + protocol::kMaxSnapshotPlayers, snapshot.players.size());
        chunk.players.assign(snapshot.players.begin() + begin, snapshot.players.begin() + end);
        for (const auto& [connection, unused] : peers_) {
            static_cast<void>(unused);
            routes.push_back({connection, chunk});
        }
        if (end == snapshot.players.size()) break;
    }
    return routes;
}

bool Session::contains(ConnectionId connection) const noexcept {
    return peers_.contains(connection);
}

std::optional<PeerInfo> Session::peer(ConnectionId connection) const {
    const auto iterator = peers_.find(connection);
    if (iterator == peers_.end()) {
        return std::nullopt;
    }
    return iterator->second.info;
}

DispatchResult Session::rejected(ConnectionId connection,
                                 protocol::DisconnectReason reason,
                                 std::string detail) const {
    DispatchResult result;
    result.accepted = false;
    result.reason = reason;
    result.detail = std::move(detail);
    result.routes.push_back({connection, protocol::Disconnect{reason, result.detail}});
    return result;
}

}  // namespace ht2mp::coordinator
