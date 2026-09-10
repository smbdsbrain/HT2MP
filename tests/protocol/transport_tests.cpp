#include "test_support.hpp"

#include "ht2mp/net/client.hpp"
#include "ht2mp/protocol/codec.hpp"

#include <enet/enet.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace {

bool send_server_message(ENetPeer* peer, const ht2mp::protocol::Message& message) {
    const auto encoded = ht2mp::protocol::encode_packet(message);
    HT2MP_CHECK(encoded);
    const auto type = ht2mp::protocol::message_type(message);
    ENetPacket* packet = enet_packet_create(encoded.bytes.data(),
                                             encoded.bytes.size(),
                                             ht2mp::protocol::is_reliable(type) ? ENET_PACKET_FLAG_RELIABLE : 0);
    HT2MP_CHECK(packet != nullptr);
    if (enet_peer_send(peer, ht2mp::protocol::channel_for(type), packet) != 0) {
        enet_packet_destroy(packet);
        HT2MP_CHECK(false);
    }
    return true;
}

bool loopback_handshake_and_state() {
    HT2MP_CHECK(enet_initialize() == 0);
    ENetAddress address{};
    address.port = 0;
    HT2MP_CHECK(enet_address_set_host(&address, "127.0.0.1") == 0);
    ENetHost* server = enet_host_create(&address, 2, 2, 0, 0);
    HT2MP_CHECK(server != nullptr);

    bool received_hello = false;
    bool client_connected = false;
    bool received_state = false;
    bool client_received_snapshot = false;
    ENetPeer* server_peer = nullptr;

    {
        ht2mp::protocol::Token128 token{};
        token[0] = std::byte{0x7A};
        ht2mp::net::ClientConfig config;
        config.server_host = "127.0.0.1";
        config.server_port = server->address.port;
        config.profile_id = "gog-05588140";
        config.token = token;
        config.display_name = "Loopback";
        config.incarnation_id = 99;
        config.vehicle_type = 4;
        config.connect_timeout_ms = 2000;
        ht2mp::net::Client client(std::move(config));
        std::string error;
        HT2MP_CHECK(client.start(error));

        bool sent_state = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline && !client_received_snapshot) {
            for (const auto& client_event : client.poll(0)) {
                if (client_event.kind == ht2mp::net::ClientEventKind::connected) {
                    client_connected = true;
                }
                if (client_event.message &&
                    std::holds_alternative<ht2mp::protocol::WorldSnapshot>(*client_event.message)) {
                    client_received_snapshot = true;
                }
                HT2MP_CHECK(client_event.kind != ht2mp::net::ClientEventKind::error);
                HT2MP_CHECK(client_event.kind != ht2mp::net::ClientEventKind::rejected);
            }

            ENetEvent event{};
            while (enet_host_service(server, &event, 1) > 0) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    server_peer = event.peer;
                } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    const auto bytes = std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(event.packet->data), event.packet->dataLength);
                    const auto decoded = ht2mp::protocol::decode_packet(bytes);
                    enet_packet_destroy(event.packet);
                    HT2MP_CHECK(decoded);
                    HT2MP_CHECK(event.channelID ==
                                ht2mp::protocol::channel_for(ht2mp::protocol::message_type(decoded.message)));
                    if (std::holds_alternative<ht2mp::protocol::ClientHello>(decoded.message)) {
                        received_hello = true;
                        HT2MP_CHECK(send_server_message(event.peer,
                                                        ht2mp::protocol::ServerWelcome{
                                                            "gog-05588140", 10, 20, 1000,
                                                            static_cast<std::uint8_t>(
                                                                ht2mp::protocol::kMaxPlayers),
                                                            20}));
                    } else if (const auto* state = std::get_if<ht2mp::protocol::PlayerState>(&decoded.message)) {
                        received_state = state->session_id == 10 && state->player_id == 20 &&
                                         state->incarnation_id == 99 && state->vehicle_type == 4;
                        HT2MP_CHECK(send_server_message(
                            event.peer, ht2mp::protocol::WorldSnapshot{1050, {*state}}));
                    }
                }
            }

            if (client_connected && !sent_state) {
                ht2mp::protocol::PlayerState state;
                state.sequence = 1;
                state.orientation.w = 1.0F;
                state.location.room_id = 0;
                state.location.road_id = 0;
                state.location.node_id = 0;
                state.location.road_segment_vector_id = 0;
                state.location.road_segment_id = 0;
                HT2MP_CHECK(client.send_state(state, error));
                sent_state = true;
            }
            std::this_thread::yield();
        }
        client.disconnect();
    }

    if (server_peer != nullptr) {
        enet_peer_reset(server_peer);
    }
    enet_host_destroy(server);
    enet_deinitialize();

    HT2MP_CHECK(received_hello);
    HT2MP_CHECK(client_connected);
    HT2MP_CHECK(received_state);
    HT2MP_CHECK(client_received_snapshot);
    return true;
}

ht2mp::net::ClientConfig client_config(std::uint16_t port,
                                       std::uint64_t incarnation,
                                       std::string name = "Restartable") {
    ht2mp::protocol::Token128 token{};
    token[0] = std::byte{0x7A};
    ht2mp::net::ClientConfig config;
    config.server_host = "127.0.0.1";
    config.server_port = port;
    config.profile_id = "gog-05588140";
    config.token = token;
    config.display_name = std::move(name);
    config.incarnation_id = incarnation;
    config.vehicle_type = 4;
    config.connect_timeout_ms = 2000;
    return config;
}

bool complete_mock_handshake(ht2mp::net::Client& client,
                             ENetHost* server,
                             std::uint64_t expected_incarnation,
                             std::uint64_t session_id,
                             std::uint64_t player_id,
                             ENetPeer*& server_peer) {
    bool received_hello = false;
    bool connected = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !connected) {
        for (const auto& client_event : client.poll(0)) {
            HT2MP_CHECK(client_event.kind != ht2mp::net::ClientEventKind::error);
            HT2MP_CHECK(client_event.kind != ht2mp::net::ClientEventKind::rejected);
            connected = connected || client_event.kind == ht2mp::net::ClientEventKind::connected;
        }
        ENetEvent event{};
        while (enet_host_service(server, &event, 1) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                server_peer = event.peer;
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                const auto bytes = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(event.packet->data), event.packet->dataLength);
                const auto decoded = ht2mp::protocol::decode_packet(bytes);
                enet_packet_destroy(event.packet);
                HT2MP_CHECK(decoded);
                const auto* hello = std::get_if<ht2mp::protocol::ClientHello>(&decoded.message);
                HT2MP_CHECK(hello != nullptr);
                HT2MP_CHECK(hello->incarnation_id == expected_incarnation);
                received_hello = true;
                HT2MP_CHECK(send_server_message(
                    event.peer,
                    ht2mp::protocol::ServerWelcome{
                        "gog-05588140", session_id, player_id, 1000,
                        static_cast<std::uint8_t>(ht2mp::protocol::kMaxPlayers), 20}));
            }
        }
        std::this_thread::yield();
    }
    HT2MP_CHECK(received_hello);
    HT2MP_CHECK(connected);
    HT2MP_CHECK(client.session_id() == session_id);
    HT2MP_CHECK(client.player_id() == player_id);
    return true;
}

bool graceful_coordinator_restart() {
    HT2MP_CHECK(enet_initialize() == 0);
    ENetAddress address{};
    address.port = 0;
    HT2MP_CHECK(enet_address_set_host(&address, "127.0.0.1") == 0);
    ENetHost* first_server = enet_host_create(&address, 2, 2, 0, 0);
    HT2MP_CHECK(first_server != nullptr);
    const auto restart_port = first_server->address.port;
    ENetPeer* first_server_peer = nullptr;

    {
        ht2mp::net::Client first_client(client_config(restart_port, 100));
        std::string error;
        HT2MP_CHECK(first_client.start(error));
        HT2MP_CHECK(complete_mock_handshake(first_client, first_server, 100, 10, 20, first_server_peer));
        HT2MP_CHECK(first_server_peer != nullptr);
        HT2MP_CHECK(send_server_message(
            first_server_peer,
            ht2mp::protocol::Disconnect{ht2mp::protocol::DisconnectReason::server_shutdown, "restart"}));
        enet_host_flush(first_server);

        bool observed_shutdown = false;
        const auto shutdown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < shutdown_deadline && !observed_shutdown) {
            for (const auto& client_event : first_client.poll(10)) {
                if (client_event.kind == ht2mp::net::ClientEventKind::disconnected) {
                    observed_shutdown = client_event.reason == ht2mp::protocol::DisconnectReason::server_shutdown;
                }
            }
        }
        HT2MP_CHECK(observed_shutdown);
        HT2MP_CHECK(first_client.state() == ht2mp::net::ClientState::disconnected);
    }

    if (first_server_peer != nullptr) {
        enet_peer_reset(first_server_peer);
    }
    enet_host_destroy(first_server);

    ENetAddress restart_address{};
    restart_address.port = restart_port;
    HT2MP_CHECK(enet_address_set_host(&restart_address, "127.0.0.1") == 0);
    ENetHost* restarted_server = enet_host_create(&restart_address, 2, 2, 0, 0);
    HT2MP_CHECK(restarted_server != nullptr);
    ENetPeer* restarted_server_peer = nullptr;
    bool received_new_incarnation_state = false;

    {
        ht2mp::net::Client reconnected_client(client_config(restart_port, 200));
        std::string error;
        HT2MP_CHECK(reconnected_client.start(error));
        HT2MP_CHECK(
            complete_mock_handshake(reconnected_client, restarted_server, 200, 11, 21, restarted_server_peer));

        ht2mp::protocol::PlayerState state;
        state.sequence = 1;
        state.orientation.w = 1.0F;
        state.location.room_id = 0;
        state.location.road_id = 0;
        state.location.node_id = 0;
        state.location.road_segment_vector_id = 0;
        state.location.road_segment_id = 0;
        HT2MP_CHECK(reconnected_client.send_state(state, error));

        const auto state_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < state_deadline && !received_new_incarnation_state) {
            static_cast<void>(reconnected_client.poll(0));
            ENetEvent event{};
            while (enet_host_service(restarted_server, &event, 1) > 0) {
                if (event.type != ENET_EVENT_TYPE_RECEIVE) {
                    continue;
                }
                const auto bytes = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(event.packet->data), event.packet->dataLength);
                const auto decoded = ht2mp::protocol::decode_packet(bytes);
                enet_packet_destroy(event.packet);
                HT2MP_CHECK(decoded);
                if (const auto* received = std::get_if<ht2mp::protocol::PlayerState>(&decoded.message)) {
                    received_new_incarnation_state = received->session_id == 11 && received->player_id == 21 &&
                                                     received->incarnation_id == 200;
                }
            }
            std::this_thread::yield();
        }
        HT2MP_CHECK(received_new_incarnation_state);
        reconnected_client.disconnect();
    }

    if (restarted_server_peer != nullptr) {
        enet_peer_reset(restarted_server_peer);
    }
    enet_host_destroy(restarted_server);
    enet_deinitialize();
    return true;
}

bool gameplay_before_welcome_is_rejected() {
    HT2MP_CHECK(enet_initialize() == 0);
    ENetAddress address{};
    address.port = 0;
    HT2MP_CHECK(enet_address_set_host(&address, "127.0.0.1") == 0);
    ENetHost* server = enet_host_create(&address, 1, 2, 0, 0);
    HT2MP_CHECK(server != nullptr);

    bool saw_error = false;
    ENetPeer* server_peer = nullptr;
    {
        ht2mp::net::Client client(client_config(server->address.port, 300,
                                                "EarlyData"));
        std::string error;
        HT2MP_CHECK(client.start(error));
        bool sent_early_data = false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline && !saw_error) {
            for (const auto& client_event : client.poll(0)) {
                saw_error = saw_error ||
                            client_event.kind ==
                                ht2mp::net::ClientEventKind::protocol_violation;
            }
            ENetEvent event{};
            while (enet_host_service(server, &event, 1) > 0) {
                if (event.type == ENET_EVENT_TYPE_CONNECT) {
                    server_peer = event.peer;
                } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                    if (!sent_early_data) {
                        ht2mp::protocol::PlayerState state;
                        state.session_id = 10;
                        state.player_id = 20;
                        state.incarnation_id = 400;
                        state.sequence = 1;
                        state.sample_time_ms = 1000;
                        state.orientation.w = 1.0F;
                        HT2MP_CHECK(send_server_message(
                            event.peer,
                            ht2mp::protocol::WorldSnapshot{1000, {state}}));
                        enet_host_flush(server);
                        sent_early_data = true;
                    }
                }
            }
            std::this_thread::yield();
        }
        HT2MP_CHECK(sent_early_data);
        HT2MP_CHECK(saw_error);
        HT2MP_CHECK(client.state() == ht2mp::net::ClientState::failed);
    }
    if (server_peer != nullptr) enet_peer_reset(server_peer);
    enet_host_destroy(server);
    enet_deinitialize();
    return true;
}

bool repeated_welcome_is_rejected() {
    HT2MP_CHECK(enet_initialize() == 0);
    ENetAddress address{};
    address.port = 0;
    HT2MP_CHECK(enet_address_set_host(&address, "127.0.0.1") == 0);
    ENetHost* server = enet_host_create(&address, 1, 2, 0, 0);
    HT2MP_CHECK(server != nullptr);
    ENetPeer* server_peer = nullptr;

    {
        ht2mp::net::Client client(client_config(server->address.port, 301,
                                                "RepeatWelcome"));
        std::string error;
        HT2MP_CHECK(client.start(error));
        HT2MP_CHECK(complete_mock_handshake(client, server, 301, 10, 20,
                                            server_peer));
        HT2MP_CHECK(server_peer != nullptr);
        HT2MP_CHECK(send_server_message(
            server_peer,
            ht2mp::protocol::ServerWelcome{
                "gog-05588140", 11, 21, 1100,
                static_cast<std::uint8_t>(ht2mp::protocol::kMaxPlayers), 20}));
        enet_host_flush(server);

        bool saw_error = false;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline && !saw_error) {
            for (const auto& client_event : client.poll(1)) {
                saw_error = saw_error ||
                            client_event.kind ==
                                ht2mp::net::ClientEventKind::protocol_violation;
            }
            ENetEvent event{};
            while (enet_host_service(server, &event, 1) > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                }
            }
        }
        HT2MP_CHECK(saw_error);
        HT2MP_CHECK(client.state() == ht2mp::net::ClientState::failed);
    }
    if (server_peer != nullptr) enet_peer_reset(server_peer);
    enet_host_destroy(server);
    enet_deinitialize();
    return true;
}

bool reconnect_admission_policy() {
    using ht2mp::protocol::DisconnectReason;
    HT2MP_CHECK(!ht2mp::net::admission_rejection_is_retryable(
        DisconnectReason::duplicate_name, false));
    HT2MP_CHECK(ht2mp::net::admission_rejection_is_retryable(
        DisconnectReason::duplicate_name, true));
    HT2MP_CHECK(ht2mp::net::admission_rejection_is_retryable(
        DisconnectReason::session_full, true));
    HT2MP_CHECK(!ht2mp::net::admission_rejection_is_retryable(
        DisconnectReason::token_mismatch, true));
    HT2MP_CHECK(!ht2mp::net::admission_rejection_is_retryable(
        DisconnectReason::profile_mismatch, true));
    HT2MP_CHECK(!ht2mp::net::admission_rejection_is_retryable(
        DisconnectReason::protocol_error, true));
    HT2MP_CHECK(ht2mp::net::disconnect_is_terminal(
        DisconnectReason::protocol_error));
    HT2MP_CHECK(ht2mp::net::disconnect_is_terminal(
        DisconnectReason::profile_mismatch));
    HT2MP_CHECK(ht2mp::net::disconnect_is_terminal(
        DisconnectReason::token_mismatch));
    HT2MP_CHECK(ht2mp::net::disconnect_is_terminal(
        DisconnectReason::rate_limited));
    HT2MP_CHECK(!ht2mp::net::disconnect_is_terminal(
        DisconnectReason::server_shutdown));
    HT2MP_CHECK(!ht2mp::net::disconnect_is_terminal(
        DisconnectReason::normal));
    return true;
}

}  // namespace

int main() {
    int failures = 0;
    failures += run_test("ENet loopback handshake and state", loopback_handshake_and_state);
    failures += run_test("graceful coordinator restart", graceful_coordinator_restart);
    failures += run_test("gameplay before welcome rejected",
                         gameplay_before_welcome_is_rejected);
    failures += run_test("repeated welcome rejected",
                         repeated_welcome_is_rejected);
    failures += run_test("reconnect admission policy",
                         reconnect_admission_policy);
    return failures == 0 ? 0 : 1;
}
