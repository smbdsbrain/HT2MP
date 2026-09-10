#pragma once

#include "ht2mp/protocol/replication.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace ht2mp::protocol {

inline constexpr std::uint8_t kProtocolVersion = 6;
inline constexpr std::size_t kPacketHeaderSize = 12;
inline constexpr std::size_t kMaxPacketSize = 1200;
inline constexpr std::size_t kMaxDisplayNameBytes = 32;
inline constexpr std::size_t kMaxProfileIdBytes = 32;
inline constexpr std::size_t kMaxDisconnectTextBytes = 96;
inline constexpr std::size_t kMaxSessionPlayers = 64;
inline constexpr std::size_t kMaxPlayers = kMaxSessionPlayers;
inline constexpr std::size_t kMaxSnapshotPlayers = 3;
inline constexpr std::uint8_t kReliableChannel = 0;
inline constexpr std::uint8_t kStateChannel = 1;

using Token128 = std::array<std::byte, 16>;

struct Vec3d {
    double x{};
    double y{};
    double z{};

    friend bool operator==(const Vec3d&, const Vec3d&) = default;
};

struct Vec3f {
    float x{};
    float y{};
    float z{};

    friend bool operator==(const Vec3f&, const Vec3f&) = default;
};

struct Quatf {
    float x{};
    float y{};
    float z{};
    float w{1.0F};

    friend bool operator==(const Quatf&, const Quatf&) = default;
};

struct WorldLocationV1 {
    std::int32_t room_id{-1};
    std::int32_t road_id{-1};
    std::int32_t node_id{-1};
    double road_distance{};
    std::int32_t road_segment_vector_id{-1};
    std::int32_t road_segment_id{-1};
    std::int32_t aux0{};
    std::int32_t aux1{};

    friend bool operator==(const WorldLocationV1&, const WorldLocationV1&) = default;
};

enum class PlayerStateFlag : std::uint8_t {
    in_world = 1U << 0U,
    paused = 1U << 1U,
    teleport = 1U << 2U,
};

[[nodiscard]] constexpr std::uint8_t flag(PlayerStateFlag value) noexcept {
    return static_cast<std::uint8_t>(value);
}

struct PlayerState {
    std::uint64_t session_id{};
    std::uint64_t player_id{};
    std::uint64_t incarnation_id{};
    std::uint32_t sequence{};
    std::uint64_t sample_time_ms{};
    std::uint8_t flags{};
    Vec3d position{};
    Quatf orientation{};
    Vec3f linear_velocity{};
    Vec3f angular_velocity{};
    std::uint16_t vehicle_type{};
    std::uint8_t paint_variant{};
    WorldLocationV1 location{};
    VehicleState vehicle{};

    [[nodiscard]] constexpr bool has(PlayerStateFlag value) const noexcept {
        return (flags & flag(value)) != 0;
    }

    void set(PlayerStateFlag value, bool enabled) noexcept {
        if (enabled) {
            flags = static_cast<std::uint8_t>(flags | flag(value));
        } else {
            flags = static_cast<std::uint8_t>(flags & ~flag(value));
        }
    }

    friend bool operator==(const PlayerState&, const PlayerState&) = default;
};

enum class MessageType : std::uint8_t {
    client_hello = 1,
    server_welcome = 2,
    peer_joined = 3,
    peer_left = 4,
    spawn_metadata = 5,
    ping = 6,
    pong = 7,
    disconnect = 8,
    player_state = 32,
    world_snapshot = 33,
};

enum class DisconnectReason : std::uint8_t {
    normal = 0,
    protocol_error = 1,
    profile_mismatch = 2,
    token_mismatch = 3,
    session_full = 4,
    duplicate_name = 5,
    rate_limited = 6,
    server_shutdown = 7,
};

struct ClientHello {
    std::string profile_id;
    Token128 token{};
    std::string display_name;
    std::uint64_t incarnation_id{};
    std::uint16_t vehicle_type{};
    std::uint8_t paint_variant{};

    friend bool operator==(const ClientHello&, const ClientHello&) = default;
};

struct ServerWelcome {
    std::string profile_id;
    std::uint64_t session_id{};
    std::uint64_t player_id{};
    std::uint64_t server_time_ms{};
    std::uint8_t max_players{static_cast<std::uint8_t>(kMaxPlayers)};
    std::uint8_t state_hz{20};

    friend bool operator==(const ServerWelcome&, const ServerWelcome&) = default;
};

struct PeerJoined {
    std::uint64_t player_id{};
    std::uint64_t incarnation_id{};
    std::uint16_t vehicle_type{};
    std::string display_name;
    std::uint8_t paint_variant{};

    friend bool operator==(const PeerJoined&, const PeerJoined&) = default;
};

struct PeerLeft {
    std::uint64_t player_id{};
    std::uint64_t incarnation_id{};
    DisconnectReason reason{DisconnectReason::normal};

    friend bool operator==(const PeerLeft&, const PeerLeft&) = default;
};

struct SpawnMetadata {
    std::uint64_t player_id{};
    std::uint64_t incarnation_id{};
    std::uint16_t vehicle_type{};
    std::string display_name;
    std::uint8_t paint_variant{};

    friend bool operator==(const SpawnMetadata&, const SpawnMetadata&) = default;
};

struct Ping {
    std::uint64_t nonce{};
    std::uint64_t client_time_ms{};

    friend bool operator==(const Ping&, const Ping&) = default;
};

struct Pong {
    std::uint64_t nonce{};
    std::uint64_t client_time_ms{};
    std::uint64_t server_time_ms{};

    friend bool operator==(const Pong&, const Pong&) = default;
};

struct Disconnect {
    DisconnectReason reason{DisconnectReason::normal};
    std::string detail;

    friend bool operator==(const Disconnect&, const Disconnect&) = default;
};

struct WorldSnapshot {
    std::uint64_t server_time_ms{};
    std::vector<PlayerState> players;
    EnvironmentState environment{};

    friend bool operator==(const WorldSnapshot&, const WorldSnapshot&) = default;
};

using Message = std::variant<ClientHello,
                             ServerWelcome,
                             PeerJoined,
                             PeerLeft,
                             SpawnMetadata,
                             Ping,
                             Pong,
                             Disconnect,
                             PlayerState,
                             WorldSnapshot>;

[[nodiscard]] MessageType message_type(const Message& message) noexcept;
[[nodiscard]] std::uint8_t channel_for(MessageType type) noexcept;
[[nodiscard]] bool is_reliable(MessageType type) noexcept;

}  // namespace ht2mp::protocol
