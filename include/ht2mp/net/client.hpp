#pragma once

#include "ht2mp/protocol/types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ht2mp::net {

struct ClientConfig {
    std::string server_host;
    std::uint16_t server_port{28020};
    std::string profile_id;
    protocol::Token128 token{};
    std::string display_name;
    std::uint64_t incarnation_id{};
    std::uint16_t vehicle_type{};
    std::uint8_t paint_variant{};
    std::uint32_t connect_timeout_ms{5000};
};

enum class ClientState {
    idle,
    connecting,
    connected,
    rejected,
    disconnected,
    failed,
};

enum class ClientEventKind {
    connected,
    message,
    rejected,
    disconnected,
    error,
    protocol_violation,
};

// A reconnect can race the coordinator's ENet timeout for the previous
// transport. Only capacity/name conflicts after a prior successful admission
// are retried; authentication and protocol failures remain terminal.
[[nodiscard]] constexpr bool admission_rejection_is_retryable(
    protocol::DisconnectReason reason,
    bool previously_connected) noexcept {
    return previously_connected &&
           (reason == protocol::DisconnectReason::duplicate_name ||
            reason == protocol::DisconnectReason::session_full);
}

// Transport failures and an orderly coordinator restart are reconnectable.
// A peer that explicitly reports an identity/protocol failure must instead
// move the bridge to safe mode; retrying the same invalid session forever
// would become fail-open once actor writes are available.
[[nodiscard]] constexpr bool disconnect_is_terminal(
    protocol::DisconnectReason reason) noexcept {
    return reason == protocol::DisconnectReason::protocol_error ||
           reason == protocol::DisconnectReason::profile_mismatch ||
           reason == protocol::DisconnectReason::token_mismatch ||
           reason == protocol::DisconnectReason::rate_limited;
}

struct ClientEvent {
    ClientEventKind kind{ClientEventKind::message};
    std::optional<protocol::Message> message;
    protocol::DisconnectReason reason{protocol::DisconnectReason::normal};
    std::string detail;
    // QueryPerformanceCounter microseconds when the datagram left the socket
    // layer. Zero for events that did not originate from a received packet.
    std::uint64_t received_at_us{};
};

// QueryPerformanceCounter in microseconds: the same counter the bridge stamps
// its samples with, so one PC shares a single time domain end to end.
[[nodiscard]] std::uint64_t qpc_microseconds() noexcept;

// One Client owns one ENet host and one coordinator connection. All methods
// must be called from the same sidecar thread; no game memory is referenced.
class Client {
public:
    explicit Client(ClientConfig config);
    ~Client();

    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] bool start(std::string& error);
    [[nodiscard]] std::vector<ClientEvent> poll(std::uint32_t timeout_ms = 0);
    [[nodiscard]] bool send(const protocol::Message& message, std::string& error);
    [[nodiscard]] bool send_state(protocol::PlayerState state, std::string& error);
    void disconnect(protocol::DisconnectReason reason = protocol::DisconnectReason::normal,
                    std::string detail = {});

    [[nodiscard]] ClientState state() const noexcept;
    [[nodiscard]] std::uint64_t session_id() const noexcept;
    [[nodiscard]] std::uint64_t player_id() const noexcept;
    [[nodiscard]] std::uint64_t incarnation_id() const noexcept;
    [[nodiscard]] std::uint64_t estimated_server_time_ms() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ht2mp::net
