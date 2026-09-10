#include "session.hpp"

#include "ht2mp/protocol/codec.hpp"
#include "ht2mp/protocol/validation.hpp"

#include <enet/enet.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using ht2mp::coordinator::ConnectionId;
using ht2mp::protocol::DisconnectReason;

std::atomic_bool running{true};
constexpr std::size_t kMaximumTransportPeers = 128U;

void on_signal(int) {
    running.store(false);
}

struct Options {
    std::string host{"0.0.0.0"};
    std::uint16_t port{28020};
    std::string profile;
    ht2mp::protocol::Token128 token{};
    bool token_was_supplied{};
    bool token_file_was_supplied{};
    bool token_was_generated{};
    bool public_access{};
    std::string token_file;
    ht2mp::protocol::EnvironmentState environment{};
};

void usage() {
    std::cout << "Usage: ht2mp-coordinator --profile <id> [--listen 0.0.0.0:28020] "
                 "[--public | --token <32 hex digits> | --token-file <path>] "
                 "[--start-hour 12] [--day-seconds 1440] "
                 "[--weather-seconds 600] [--weather-bias 0.5] [--weather-preset 0]\n";
}

bool parse_endpoint(std::string_view text, std::string& host, std::uint16_t& port) {
    const auto separator = text.rfind(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
        return false;
    }
    std::uint32_t parsed_port{};
    const auto port_text = text.substr(separator + 1);
    const auto conversion = std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed_port);
    if (conversion.ec != std::errc{} || conversion.ptr != port_text.data() + port_text.size() || parsed_port == 0 ||
        parsed_port > 65535) {
        return false;
    }
    host.assign(text.substr(0, separator));
    port = static_cast<std::uint16_t>(parsed_port);
    return true;
}

std::optional<ht2mp::protocol::Token128> parse_token(std::string_view value) {
    if (value.size() != 32) {
        return std::nullopt;
    }
    ht2mp::protocol::Token128 token{};
    for (std::size_t index = 0; index < token.size(); ++index) {
        const auto pair = value.substr(index * 2, 2);
        unsigned parsed{};
        const auto conversion = std::from_chars(pair.data(), pair.data() + pair.size(), parsed, 16);
        if (conversion.ec != std::errc{} || conversion.ptr != pair.data() + pair.size() || parsed > 0xFFU) {
            return std::nullopt;
        }
        token[index] = static_cast<std::byte>(parsed);
    }
    return token;
}

std::optional<ht2mp::protocol::Token128> read_token_file(
    const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Could not open --token-file: " << path << '\n';
        return std::nullopt;
    }
    std::string value{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
    if (input.bad()) {
        std::cerr << "Could not read --token-file: " << path << '\n';
        return std::nullopt;
    }
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        std::cerr << "--token-file must contain exactly 32 hexadecimal digits\n";
        return std::nullopt;
    }
    value = value.substr(first, last - first + 1U);
    const auto token = parse_token(value);
    if (!token) {
        std::cerr << "--token-file must contain exactly 32 hexadecimal digits\n";
    }
    return token;
}

ht2mp::protocol::Token128 random_token() {
    ht2mp::protocol::Token128 token{};
    std::random_device source;
    for (auto& byte : token) {
        byte = static_cast<std::byte>(source() & 0xFFU);
    }
    return token;
}

std::uint64_t random_nonzero_u64() {
    std::random_device source;
    const auto value = (static_cast<std::uint64_t>(source()) << 32U) ^ source();
    return value == 0 ? 1 : value;
}

std::string token_text(const ht2mp::protocol::Token128& token) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : token) {
        output << std::setw(2) << std::to_integer<unsigned>(byte);
    }
    return output.str();
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            usage();
            return std::nullopt;
        }
        if (argument == "--public") {
            options.public_access = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return std::nullopt;
        }
        const std::string_view value = argv[++index];
        if (argument == "--listen") {
            if (!parse_endpoint(value, options.host, options.port)) {
                std::cerr << "Invalid --listen endpoint\n";
                return std::nullopt;
            }
        } else if (argument == "--profile") {
            options.profile.assign(value);
        } else if (argument == "--token") {
            const auto token = parse_token(value);
            if (!token) {
                std::cerr << "--token must contain exactly 32 hexadecimal digits\n";
                return std::nullopt;
            }
            options.token = *token;
            options.token_was_supplied = true;
        } else if (argument == "--token-file") {
            options.token_file.assign(value);
            options.token_file_was_supplied = true;
        } else if (argument == "--start-hour" || argument == "--day-seconds" ||
                   argument == "--weather-seconds" || argument == "--weather-bias" ||
                   argument == "--weather-preset") {
            double parsed{};
            const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() || !std::isfinite(parsed)) {
                std::cerr << "Invalid environment option: " << argument << '\n';
                return std::nullopt;
            }
            if (argument == "--start-hour") options.environment.day_hours = parsed;
            else if (argument == "--day-seconds") options.environment.day_duration_s = static_cast<float>(parsed);
            else if (argument == "--weather-seconds") options.environment.weather_duration_s = static_cast<float>(parsed);
            else if (argument == "--weather-bias") options.environment.weather_bias = static_cast<float>(parsed);
            else {
                if (parsed < 0 || parsed >= 8 || std::floor(parsed) != parsed) return std::nullopt;
                options.environment.weather_preset = static_cast<std::uint8_t>(parsed);
            }
            if (!ht2mp::protocol::valid_environment(options.environment)) {
                std::cerr << "Environment option outside supported bounds: " << argument << '\n';
                return std::nullopt;
            }
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            return std::nullopt;
        }
    }
    if (options.profile.empty()) {
        std::cerr << "--profile is required\n";
        return std::nullopt;
    }
    const auto authentication_modes =
        static_cast<unsigned>(options.public_access) +
        static_cast<unsigned>(options.token_was_supplied) +
        static_cast<unsigned>(options.token_file_was_supplied);
    if (authentication_modes > 1U) {
        std::cerr << "--public, --token, and --token-file are mutually exclusive\n";
        return std::nullopt;
    }
    if (options.token_file_was_supplied) {
        const auto token = read_token_file(options.token_file);
        if (!token) {
            return std::nullopt;
        }
        options.token = *token;
    } else if (!options.public_access && !options.token_was_supplied) {
        options.token = random_token();
        options.token_was_generated = true;
    }
    const ht2mp::protocol::ClientHello probe{
        options.profile, options.token, "coordinator-probe", 1, 0};
    if (const auto validation = ht2mp::protocol::validate(probe); !validation) {
        std::cerr << "Invalid --profile: " << validation.detail << '\n';
        return std::nullopt;
    }
    return options;
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool send_message(ENetPeer* peer, const ht2mp::protocol::Message& message) {
    const auto encoded = ht2mp::protocol::encode_packet(message);
    if (!encoded) {
        std::cerr << "Internal encode error: " << encoded.detail << '\n';
        return false;
    }
    const auto type = ht2mp::protocol::message_type(message);
    const enet_uint32 flags = ht2mp::protocol::is_reliable(type) ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(encoded.bytes.data(), encoded.bytes.size(), flags);
    if (packet == nullptr) {
        return false;
    }
    if (enet_peer_send(peer, ht2mp::protocol::channel_for(type), packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

bool dispatch(const std::vector<ht2mp::coordinator::Route>& routes,
              const std::unordered_map<ConnectionId, ENetPeer*>& peers) {
    for (const auto& route : routes) {
        const auto iterator = peers.find(route.target);
        if (iterator != peers.end()) {
            if (!send_message(iterator->second, route.message)) {
                const auto type = ht2mp::protocol::message_type(route.message);
                if (ht2mp::protocol::is_reliable(type)) {
                    std::cerr << "Failed to queue reliable HT2MP/6 control message for connection "
                              << route.target << '\n';
                    return false;
                }
                // World snapshots are deliberately unreliable-sequenced; a
                // later 20 Hz snapshot repairs a transient queue failure.
            }
        }
    }
    return true;
}

bool reject_transport(ENetPeer* peer, DisconnectReason reason, const std::string& detail) {
    const bool queued =
        send_message(peer, ht2mp::protocol::Disconnect{reason, detail});
    enet_peer_disconnect_later(peer, static_cast<enet_uint32>(reason));
    return queued;
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    if (!options) {
        return argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") ? 0 : 2;
    }
    const auto profile_limits =
        ht2mp::protocol::limits_for_profile(options->profile);
    if (!profile_limits) {
        std::cerr << "Unknown HT2MP game profile: " << options->profile << '\n';
        return 2;
    }
    if (enet_initialize() != 0) {
        std::cerr << "ENet initialization failed\n";
        return 3;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = options->port;
    if (options->host != "0.0.0.0" && enet_address_set_host(&address, options->host.c_str()) != 0) {
        std::cerr << "Could not resolve listen address\n";
        enet_deinitialize();
        return 4;
    }
    ENetHost* host = enet_host_create(&address, kMaximumTransportPeers, 2, 0, 0);
    if (host == nullptr) {
        std::cerr << "Could not bind ENet host\n";
        enet_deinitialize();
        return 5;
    }

    ht2mp::coordinator::SessionConfig config;
    config.profile_id = options->profile;
    config.environment = options->environment;
    config.limits = *profile_limits;
    config.token = options->token;
    config.authentication = options->public_access
        ? ht2mp::coordinator::AuthenticationMode::public_access
        : ht2mp::coordinator::AuthenticationMode::token;
    config.session_id = random_nonzero_u64();
    ht2mp::coordinator::Session session(std::move(config));

    std::unordered_map<ENetPeer*, ConnectionId> connection_ids;
    std::unordered_map<ConnectionId, ENetPeer*> peers;
    ConnectionId next_connection = 1;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "HT2MP/" << static_cast<unsigned>(ht2mp::protocol::kProtocolVersion)
              << " coordinator listening on " << options->host << ':' << options->port << '\n'
              << "profile=" << options->profile << '\n'
              << "max_players=" << ht2mp::protocol::kMaxPlayers << '\n'
              << "auth=" << (options->public_access ? "public" : "token") << '\n';
    if (options->token_was_generated) {
        std::cout << "token=" << token_text(options->token) << '\n';
    } else if (!options->public_access) {
        std::cout << "token=configured\n";
    }
    std::cout << std::flush;

    bool service_failed = false;
    while (running.load()) {
        ENetEvent event{};
        const int event_result = enet_host_service(host, &event, 10);
        if (event_result < 0) {
            std::cerr << "ENet service failure\n";
            service_failed = true;
            break;
        }
        if (event_result > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    const ConnectionId id = next_connection++;
                    connection_ids.emplace(event.peer, id);
                    peers.emplace(id, event.peer);
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    const auto id_iterator = connection_ids.find(event.peer);
                    if (id_iterator == connection_ids.end()) {
                        enet_packet_destroy(event.packet);
                        if (!reject_transport(event.peer,
                                              DisconnectReason::protocol_error,
                                              "unknown transport peer")) {
                            service_failed = true;
                            running.store(false);
                        }
                        break;
                    }
                    const ConnectionId connection_id = id_iterator->second;
                    const auto reject_registered_transport =
                        [&](const DisconnectReason reason,
                            const std::string& detail) {
                            if (!dispatch(session.disconnect(connection_id, reason),
                                          peers)) {
                                service_failed = true;
                                running.store(false);
                            }
                            if (!reject_transport(event.peer, reason, detail)) {
                                service_failed = true;
                                running.store(false);
                            }
                            // The disconnect packet is already queued on the
                            // ENetPeer. Removing routing/membership now keeps a
                            // closing transport from re-admitting itself or
                            // receiving fresh snapshots before its event fires.
                            peers.erase(connection_id);
                            connection_ids.erase(event.peer);
                        };
                    const auto bytes = std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(event.packet->data), event.packet->dataLength);
                    const auto decoded = ht2mp::protocol::decode_packet(bytes);
                    enet_packet_destroy(event.packet);
                    if (!decoded) {
                        reject_registered_transport(
                            DisconnectReason::protocol_error, decoded.detail);
                        break;
                    }
                    const auto type = ht2mp::protocol::message_type(decoded.message);
                    if (event.channelID != ht2mp::protocol::channel_for(type)) {
                        reject_registered_transport(
                            DisconnectReason::protocol_error,
                            "message arrived on the wrong channel");
                        break;
                    }

                    ht2mp::coordinator::DispatchResult result;
                    if (!session.contains(connection_id)) {
                        const auto* hello = std::get_if<ht2mp::protocol::ClientHello>(&decoded.message);
                        if (hello == nullptr) {
                            reject_registered_transport(
                                DisconnectReason::protocol_error,
                                "ClientHello is required first");
                            break;
                        }
                        result = session.accept(connection_id, *hello, now_ms());
                    } else {
                        result = session.receive(connection_id, decoded.message, now_ms());
                    }
                    if (!dispatch(result.routes, peers)) {
                        service_failed = true;
                        running.store(false);
                        break;
                    }
                    if (!result.routes.empty()) {
                        enet_host_flush(host);
                    }
                    if (!result.accepted) {
                        // Stop this connection contributing snapshots or
                        // consuming a session slot immediately. Waiting for
                        // ENet's disconnect event while continuing to queue
                        // traffic can indefinitely postpone disconnect_later.
                        if (!dispatch(session.disconnect(connection_id,
                                                         result.reason),
                                      peers)) {
                            service_failed = true;
                            running.store(false);
                        }
                        enet_peer_disconnect_later(event.peer, static_cast<enet_uint32>(result.reason));
                        peers.erase(connection_id);
                        connection_ids.erase(event.peer);
                    }
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    const auto id_iterator = connection_ids.find(event.peer);
                    if (id_iterator != connection_ids.end()) {
                        if (!dispatch(session.disconnect(id_iterator->second,
                                                         DisconnectReason::normal),
                                      peers)) {
                            service_failed = true;
                            running.store(false);
                        }
                        peers.erase(id_iterator->second);
                        connection_ids.erase(id_iterator);
                    }
                    event.peer->data = nullptr;
                    break;
                }
                case ENET_EVENT_TYPE_NONE:
                    break;
            }
        }
        if (!dispatch(session.tick(now_ms()), peers)) {
            service_failed = true;
            break;
        }
    }

    for (const auto& [unused, peer] : peers) {
        static_cast<void>(unused);
        send_message(peer, ht2mp::protocol::Disconnect{DisconnectReason::server_shutdown, "server is shutting down"});
        enet_peer_disconnect_later(peer, static_cast<enet_uint32>(DisconnectReason::server_shutdown));
    }
    enet_host_flush(host);
    enet_host_destroy(host);
    enet_deinitialize();
    return service_failed ? 6 : 0;
}
