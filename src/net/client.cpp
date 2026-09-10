#include "ht2mp/net/client.hpp"

#include "ht2mp/protocol/codec.hpp"
#include "ht2mp/protocol/validation.hpp"

#include <enet/enet.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <utility>

namespace ht2mp::net {
namespace {

std::mutex lifetime_mutex;
std::size_t lifetime_references{};

bool acquire_enet() {
    const std::scoped_lock lock(lifetime_mutex);
    if (lifetime_references == 0 && enet_initialize() != 0) {
        return false;
    }
    ++lifetime_references;
    return true;
}

void release_enet() {
    const std::scoped_lock lock(lifetime_mutex);
    if (lifetime_references != 0 && --lifetime_references == 0) {
        enet_deinitialize();
    }
}

std::uint64_t monotonic_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

}  // namespace

std::uint64_t qpc_microseconds() noexcept {
#ifdef _WIN32
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? value.QuadPart : 0;
    }();
    LARGE_INTEGER counter{};
    if (frequency <= 0 || !QueryPerformanceCounter(&counter)) {
        return 0U;
    }
    const auto seconds = counter.QuadPart / frequency;
    const auto remainder = counter.QuadPart % frequency;
    return static_cast<std::uint64_t>(seconds) * 1'000'000ULL +
           static_cast<std::uint64_t>(remainder * 1'000'000LL / frequency);
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}

class Client::Impl {
public:
    explicit Impl(ClientConfig value) : config(std::move(value)) {}

    ~Impl() {
        close();
    }

    bool start(std::string& error) {
        if (current_state != ClientState::idle && current_state != ClientState::disconnected &&
            current_state != ClientState::failed && current_state != ClientState::rejected) {
            error = "network client is already active";
            return false;
        }
        protocol::ClientHello hello{config.profile_id,
                                    config.token,
                                    config.display_name,
                                    config.incarnation_id,
                                    config.vehicle_type,
                                    config.paint_variant};
        if (config.server_host.empty() || config.server_port == 0) {
            error = "server endpoint is incomplete";
            return false;
        }
        if (const auto validation = protocol::validate(hello); !validation) {
            error = validation.detail;
            return false;
        }
        close();
        assigned_session_id = 0;
        assigned_player_id = 0;
        server_epoch_ms = 0;
        server_epoch_local_ms = 0;
        if (!acquire_enet()) {
            error = "ENet initialization failed";
            current_state = ClientState::failed;
            return false;
        }
        owns_enet = true;
        host = enet_host_create(nullptr, 1, 2, 0, 0);
        if (host == nullptr) {
            error = "could not create ENet client host";
            close();
            current_state = ClientState::failed;
            return false;
        }
        ENetAddress address{};
        address.port = config.server_port;
        if (enet_address_set_host(&address, config.server_host.c_str()) != 0) {
            error = "could not resolve coordinator host";
            close();
            current_state = ClientState::failed;
            return false;
        }
        peer = enet_host_connect(host, &address, 2, 0);
        if (peer == nullptr) {
            error = "could not allocate ENet peer";
            close();
            current_state = ClientState::failed;
            return false;
        }
        started_at_ms = monotonic_ms();
        current_state = ClientState::connecting;
        return true;
    }

    std::vector<ClientEvent> poll(std::uint32_t timeout_ms) {
        std::vector<ClientEvent> events;
        if (host == nullptr || (current_state != ClientState::connecting && current_state != ClientState::connected)) {
            return events;
        }

        ENetEvent event{};
        const int result = enet_host_service(host, &event, timeout_ms);
        if (result < 0) {
            current_state = ClientState::failed;
            events.push_back({ClientEventKind::error, std::nullopt, protocol::DisconnectReason::protocol_error,
                              "ENet service failed"});
            return events;
        }
        // Stamp arrival once per service wake-up: every event produced by the
        // packets ENet just dequeued inherits the same receive instant.
        const auto stamp_from = [&](const std::size_t first) {
            const auto received_at = qpc_microseconds();
            for (std::size_t index = first; index < events.size(); ++index) {
                events[index].received_at_us = received_at;
            }
        };
        if (result > 0) {
            const auto first = events.size();
            handle_event(event, events);
            stamp_from(first);
        }
        while (host != nullptr && enet_host_check_events(host, &event) > 0) {
            const auto first = events.size();
            handle_event(event, events);
            stamp_from(first);
        }
        if (current_state == ClientState::connecting && monotonic_ms() - started_at_ms >= config.connect_timeout_ms) {
            if (peer != nullptr) {
                enet_peer_reset(peer);
                peer = nullptr;
            }
            current_state = ClientState::failed;
            events.push_back({ClientEventKind::error, std::nullopt, protocol::DisconnectReason::protocol_error,
                              "coordinator connection timed out"});
        }
        return events;
    }

    bool send(const protocol::Message& message, std::string& error) {
        if (current_state != ClientState::connected || peer == nullptr) {
            error = "network client is not connected";
            return false;
        }
        const auto type = protocol::message_type(message);
        if (type != protocol::MessageType::player_state && type != protocol::MessageType::ping &&
            type != protocol::MessageType::disconnect) {
            error = "message type is not valid client traffic";
            return false;
        }
        return send_packet(message, error);
    }

    bool send_state(protocol::PlayerState state_value, std::string& error) {
        state_value.session_id = assigned_session_id;
        state_value.player_id = assigned_player_id;
        state_value.incarnation_id = config.incarnation_id;
        state_value.vehicle_type = config.vehicle_type;
        state_value.paint_variant = config.paint_variant;
        if (state_value.sample_time_ms == 0) {
            state_value.sample_time_ms = estimated_server_time_ms();
        }
        if (!send(state_value, error)) {
            return false;
        }
        // Do not let the state wait for the next service call: push it onto
        // the wire now so sender cadence, not the poll loop, sets the pace.
        enet_host_flush(host);
        return true;
    }

    void disconnect(protocol::DisconnectReason reason, std::string detail) {
        if (peer == nullptr) {
            return;
        }
        if (current_state == ClientState::connected) {
            std::string ignored;
            send_packet(protocol::Disconnect{reason, std::move(detail)}, ignored);
            enet_host_flush(host);
        }
        enet_peer_disconnect_later(peer, static_cast<enet_uint32>(reason));
        current_state = ClientState::disconnected;
    }

    std::uint64_t estimated_server_time_ms() const noexcept {
        if (server_epoch_local_ms == 0) {
            return monotonic_ms();
        }
        return server_epoch_ms + (monotonic_ms() - server_epoch_local_ms);
    }

    ClientConfig config;
    ClientState current_state{ClientState::idle};
    ENetHost* host{};
    ENetPeer* peer{};
    bool owns_enet{};
    std::uint64_t started_at_ms{};
    std::uint64_t assigned_session_id{};
    std::uint64_t assigned_player_id{};
    std::uint64_t server_epoch_ms{};
    std::uint64_t server_epoch_local_ms{};

private:
    void close() {
        if (host != nullptr) {
            if (peer != nullptr) {
                enet_peer_reset(peer);
                peer = nullptr;
            }
            enet_host_destroy(host);
            host = nullptr;
        }
        if (owns_enet) {
            release_enet();
            owns_enet = false;
        }
    }

    bool send_packet(const protocol::Message& message, std::string& error) {
        const auto encoded = protocol::encode_packet(message);
        if (!encoded) {
            error = encoded.detail;
            return false;
        }
        const auto type = protocol::message_type(message);
        const enet_uint32 flags = protocol::is_reliable(type) ? ENET_PACKET_FLAG_RELIABLE : 0;
        ENetPacket* packet = enet_packet_create(encoded.bytes.data(), encoded.bytes.size(), flags);
        if (packet == nullptr) {
            error = "ENet packet allocation failed";
            return false;
        }
        if (enet_peer_send(peer, protocol::channel_for(type), packet) != 0) {
            enet_packet_destroy(packet);
            error = "ENet rejected outgoing packet";
            return false;
        }
        return true;
    }

    void handle_event(ENetEvent& event, std::vector<ClientEvent>& events) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                std::string error;
                const protocol::ClientHello hello{config.profile_id,
                                                  config.token,
                                                  config.display_name,
                                                  config.incarnation_id,
                                                  config.vehicle_type,
                                                  config.paint_variant};
                if (!send_packet(hello, error)) {
                    current_state = ClientState::failed;
                    events.push_back({ClientEventKind::error, std::nullopt,
                                      protocol::DisconnectReason::protocol_error, std::move(error)});
                }
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const auto packet = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(event.packet->data), event.packet->dataLength);
                auto decoded = protocol::decode_packet(packet);
                enet_packet_destroy(event.packet);
                const auto fail_server_protocol =
                    [&](const protocol::DisconnectReason reason,
                        std::string detail) {
                        current_state = ClientState::failed;
                        events.push_back({ClientEventKind::protocol_violation,
                                          std::nullopt,
                                          reason,
                                          std::move(detail)});
                        if (peer != nullptr) {
                            enet_peer_disconnect_later(
                                peer, static_cast<enet_uint32>(reason));
                        }
                    };
                if (!decoded || event.channelID != protocol::channel_for(protocol::message_type(decoded.message))) {
                    fail_server_protocol(
                        protocol::DisconnectReason::protocol_error,
                        decoded ? "server message arrived on the wrong channel"
                                : decoded.detail);
                    break;
                }
                if (const auto* welcome = std::get_if<protocol::ServerWelcome>(&decoded.message)) {
                    if (current_state != ClientState::connecting) {
                        fail_server_protocol(
                            protocol::DisconnectReason::protocol_error,
                            "coordinator sent a repeated ServerWelcome");
                        break;
                    }
                    if (welcome->profile_id != config.profile_id) {
                        fail_server_protocol(
                            protocol::DisconnectReason::profile_mismatch,
                            "coordinator welcomed client into the wrong profile");
                        break;
                    }
                    if (welcome->max_players != protocol::kMaxPlayers ||
                        welcome->state_hz != 20U) {
                        fail_server_protocol(
                            protocol::DisconnectReason::protocol_error,
                            "coordinator advertised unsupported HT2MP/6 limits");
                        break;
                    }
                    assigned_session_id = welcome->session_id;
                    assigned_player_id = welcome->player_id;
                    server_epoch_ms = welcome->server_time_ms;
                    server_epoch_local_ms = monotonic_ms();
                    current_state = ClientState::connected;
                    events.push_back({ClientEventKind::connected, decoded.message});
                } else if (const auto* disconnected = std::get_if<protocol::Disconnect>(&decoded.message)) {
                    const bool admission_rejection = current_state != ClientState::connected;
                    current_state = admission_rejection ? ClientState::rejected : ClientState::disconnected;
                    events.push_back({admission_rejection ? ClientEventKind::rejected : ClientEventKind::disconnected,
                                      decoded.message,
                                      disconnected->reason,
                                      disconnected->detail});
                } else {
                    if (current_state != ClientState::connected) {
                        fail_server_protocol(
                            protocol::DisconnectReason::protocol_error,
                            "coordinator sent gameplay data before ServerWelcome");
                        break;
                    }
                    if (std::holds_alternative<protocol::ClientHello>(
                            decoded.message)) {
                        fail_server_protocol(
                            protocol::DisconnectReason::protocol_error,
                            "coordinator sent a client-only message");
                        break;
                    }
                    events.push_back({ClientEventKind::message, std::move(decoded.message)});
                }
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                peer = nullptr;
                if (current_state != ClientState::rejected && current_state != ClientState::failed) {
                    current_state = ClientState::disconnected;
                    const auto maximum_reason = static_cast<enet_uint32>(protocol::DisconnectReason::server_shutdown);
                    events.push_back({ClientEventKind::disconnected,
                                      std::nullopt,
                                      event.data <= maximum_reason
                                          ? static_cast<protocol::DisconnectReason>(event.data)
                                          : protocol::DisconnectReason::protocol_error,
                                      "coordinator transport disconnected"});
                }
                break;
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
};

Client::Client(ClientConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

bool Client::start(std::string& error) {
    return impl_->start(error);
}

std::vector<ClientEvent> Client::poll(std::uint32_t timeout_ms) {
    return impl_->poll(timeout_ms);
}

bool Client::send(const protocol::Message& message, std::string& error) {
    return impl_->send(message, error);
}

bool Client::send_state(protocol::PlayerState state, std::string& error) {
    return impl_->send_state(std::move(state), error);
}

void Client::disconnect(protocol::DisconnectReason reason, std::string detail) {
    impl_->disconnect(reason, std::move(detail));
}

ClientState Client::state() const noexcept {
    return impl_->current_state;
}

std::uint64_t Client::session_id() const noexcept {
    return impl_->assigned_session_id;
}

std::uint64_t Client::player_id() const noexcept {
    return impl_->assigned_player_id;
}

std::uint64_t Client::incarnation_id() const noexcept {
    return impl_->config.incarnation_id;
}

std::uint64_t Client::estimated_server_time_ms() const noexcept {
    return impl_->estimated_server_time_ms();
}

}  // namespace ht2mp::net
