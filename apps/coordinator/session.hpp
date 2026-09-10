#pragma once

#include "ht2mp/protocol/types.hpp"
#include "ht2mp/protocol/validation.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ht2mp::coordinator {

using ConnectionId = std::uint64_t;

enum class AuthenticationMode : std::uint8_t {
    token,
    public_access,
};

struct SessionConfig {
    std::string profile_id;
    protocol::Token128 token{};
    AuthenticationMode authentication{AuthenticationMode::token};
    std::uint64_t session_id{};
    std::uint8_t max_players{static_cast<std::uint8_t>(protocol::kMaxPlayers)};
    std::uint8_t state_hz{20};
    std::uint8_t max_state_updates_per_second{40};
    // A peer whose latest state is older than this (coordinator receive time)
    // is left out of snapshots; receivers then hide/despawn it by their own
    // receive age.
    std::uint32_t stale_state_ms{3000};
    protocol::ProfileLimits limits{};
    protocol::EnvironmentState environment{};
};

struct Route {
    ConnectionId target{};
    protocol::Message message;
};

struct DispatchResult {
    bool accepted{true};
    protocol::DisconnectReason reason{protocol::DisconnectReason::normal};
    std::string detail;
    std::vector<Route> routes;
};

struct PeerInfo {
    ConnectionId connection_id{};
    std::uint64_t player_id{};
    std::uint64_t incarnation_id{};
    std::uint16_t vehicle_type{};
    std::uint8_t paint_variant{};
    std::string display_name;
};

class Session {
public:
    explicit Session(SessionConfig config);

    [[nodiscard]] DispatchResult accept(ConnectionId connection,
                                        const protocol::ClientHello& hello,
                                        std::uint64_t server_time_ms);
    [[nodiscard]] DispatchResult receive(ConnectionId connection,
                                         const protocol::Message& message,
                                         std::uint64_t server_time_ms);
    [[nodiscard]] std::vector<Route> disconnect(ConnectionId connection,
                                                protocol::DisconnectReason reason);
    [[nodiscard]] std::vector<Route> tick(std::uint64_t server_time_ms);

    [[nodiscard]] bool contains(ConnectionId connection) const noexcept;
    [[nodiscard]] std::optional<PeerInfo> peer(ConnectionId connection) const;
    [[nodiscard]] std::size_t size() const noexcept { return peers_.size(); }
    [[nodiscard]] std::uint64_t session_id() const noexcept { return config_.session_id; }

private:
    struct Peer {
        PeerInfo info;
        std::optional<protocol::PlayerState> latest;
        std::uint64_t latest_received_ms{};
        std::deque<std::uint64_t> accepted_update_times_ms;
    };

    [[nodiscard]] DispatchResult rejected(ConnectionId connection,
                                          protocol::DisconnectReason reason,
                                          std::string detail) const;
    SessionConfig config_;
    std::unordered_map<ConnectionId, Peer> peers_;
    std::uint64_t next_player_id_{1};
    std::uint64_t next_snapshot_ms_{};
    std::optional<std::uint64_t> environment_epoch_ms_;
};

}  // namespace ht2mp::coordinator
