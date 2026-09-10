#include "ht2mp/protocol/codec.hpp"
#include "ht2mp/protocol/types.hpp"
#include "ht2mp/protocol/validation.hpp"
#include "server_protocol.hpp"

#include <enet/enet.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

std::atomic_bool running{true};

enum class RouteMode {
    follow,
    orbit,
};

void on_signal(int) {
    running.store(false);
}

struct Options {
    std::string host;
    std::uint16_t port{};
    std::string profile;
    ht2mp::protocol::Token128 token{};
    std::string follow;
    std::string name{"sim"};
    std::uint8_t count{1};
    std::uint16_t vehicle_type{};
    double distance{15.0};
    RouteMode route{RouteMode::follow};
    double radius{18.0};
    double period_seconds{24.0};
    std::uint64_t duration_ms{};
};

struct SimPeer {
    ENetPeer* transport{};
    std::string name;
    std::uint64_t incarnation_id{};
    ht2mp::sim::ServerProtocolState server;
    std::uint32_t sequence{};
};

void usage() {
    std::cout << "Usage: ht2mp-sim --server <host:port> --profile <id> [--token <32 hex digits>] "
                 "--follow <name-or-id> [--count 1..64] [--name sim] [--vehicle-type 0] [--distance 15] "
                 "[--route follow|orbit] [--radius 18] [--period 24] [--duration-ms N]\n";
}

bool parse_endpoint(std::string_view text, std::string& host, std::uint16_t& port) {
    const auto separator = text.rfind(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= text.size()) {
        return false;
    }
    std::uint32_t parsed{};
    const auto port_text = text.substr(separator + 1);
    const auto conversion = std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != port_text.data() + port_text.size() || parsed == 0 ||
        parsed > 65535) {
        return false;
    }
    host.assign(text.substr(0, separator));
    port = static_cast<std::uint16_t>(parsed);
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
        if (conversion.ec != std::errc{} || conversion.ptr != pair.data() + pair.size() || parsed > 255) {
            return std::nullopt;
        }
        token[index] = static_cast<std::byte>(parsed);
    }
    return token;
}

template <typename Integer>
bool parse_integer(std::string_view value, Integer& output) {
    unsigned long long parsed{};
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() ||
        parsed > static_cast<unsigned long long>((std::numeric_limits<Integer>::max)())) {
        return false;
    }
    output = static_cast<Integer>(parsed);
    return true;
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            usage();
            return std::nullopt;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return std::nullopt;
        }
        const std::string_view value = argv[++index];
        if (argument == "--server") {
            if (!parse_endpoint(value, options.host, options.port)) {
                std::cerr << "Invalid --server endpoint\n";
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
        } else if (argument == "--follow") {
            options.follow.assign(value);
        } else if (argument == "--name") {
            options.name.assign(value);
        } else if (argument == "--count") {
            if (!parse_integer(value, options.count) || options.count < 1 || options.count > 64) {
                std::cerr << "--count must be in the range 1..64\n";
                return std::nullopt;
            }
        } else if (argument == "--vehicle-type") {
            if (!parse_integer(value, options.vehicle_type)) {
                std::cerr << "Invalid --vehicle-type\n";
                return std::nullopt;
            }
        } else if (argument == "--distance") {
            try {
                std::size_t consumed{};
                options.distance = std::stod(std::string(value), &consumed);
                if (consumed != value.size() || !std::isfinite(options.distance) || options.distance < 1.0 ||
                    options.distance > 100.0) {
                    throw std::invalid_argument("distance");
                }
            } catch (...) {
                std::cerr << "--distance must be a finite number in the range 1..100\n";
                return std::nullopt;
            }
        } else if (argument == "--route") {
            if (value == "follow") {
                options.route = RouteMode::follow;
            } else if (value == "orbit") {
                options.route = RouteMode::orbit;
            } else {
                std::cerr << "--route must be follow or orbit\n";
                return std::nullopt;
            }
        } else if (argument == "--radius" || argument == "--period") {
            try {
                std::size_t consumed{};
                const double parsed = std::stod(std::string(value), &consumed);
                const double minimum = argument == "--radius" ? 5.0 : 5.0;
                const double maximum = argument == "--radius" ? 100.0 : 300.0;
                if (consumed != value.size() || !std::isfinite(parsed) ||
                    parsed < minimum || parsed > maximum) {
                    throw std::invalid_argument("route parameter");
                }
                if (argument == "--radius") {
                    options.radius = parsed;
                } else {
                    options.period_seconds = parsed;
                }
            } catch (...) {
                std::cerr << argument << " must be a finite number in the range 5.."
                          << (argument == "--radius" ? 100 : 300) << '\n';
                return std::nullopt;
            }
        } else if (argument == "--duration-ms") {
            if (!parse_integer(value, options.duration_ms) || options.duration_ms < 250) {
                std::cerr << "--duration-ms must be zero (omitted) or at least 250\n";
                return std::nullopt;
            }
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            return std::nullopt;
        }
    }
    if (options.host.empty() || options.profile.empty() || options.follow.empty()) {
        std::cerr << "--server, --profile, and --follow are required\n";
        return std::nullopt;
    }
    const auto suffix_bytes = options.count >= 10U ? 3U : 2U;
    if (options.name.empty() ||
        options.name.size() > ht2mp::protocol::kMaxDisplayNameBytes - suffix_bytes) {
        std::cerr << "--name is too long for the requested peer count\n";
        return std::nullopt;
    }
    return options;
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t random_nonzero_u64() {
    std::random_device source;
    const auto value = (static_cast<std::uint64_t>(source()) << 32U) ^ source();
    return value == 0 ? 1 : value;
}

bool send_message(ENetPeer* peer, const ht2mp::protocol::Message& message) {
    const auto encoded = ht2mp::protocol::encode_packet(message);
    if (!encoded) {
        std::cerr << "Encode failure: " << encoded.detail << '\n';
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

std::optional<std::uint64_t> numeric_target(std::string_view value) {
    std::uint64_t parsed{};
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec == std::errc{} && conversion.ptr == value.data() + value.size() && parsed != 0) {
        return parsed;
    }
    return std::nullopt;
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
    for (std::uint8_t index = 0; index < options->count; ++index) {
        const auto peer_name = options->name + '-' + std::to_string(index + 1U);
        const ht2mp::protocol::ClientHello probe{
            options->profile, options->token, peer_name,
            static_cast<std::uint64_t>(index) + 1U, options->vehicle_type};
        if (const auto validation =
                ht2mp::protocol::validate(probe, *profile_limits);
            !validation) {
            std::cerr << "Invalid simulator options: " << validation.detail << '\n';
            return 2;
        }
    }
    if (enet_initialize() != 0) {
        std::cerr << "ENet initialization failed\n";
        return 3;
    }
    ENetAddress address{};
    address.port = options->port;
    if (enet_address_set_host(&address, options->host.c_str()) != 0) {
        std::cerr << "Could not resolve server host\n";
        enet_deinitialize();
        return 4;
    }
    ENetHost* host = enet_host_create(nullptr, options->count, 2, 0, 0);
    if (host == nullptr) {
        std::cerr << "Could not create ENet client host\n";
        enet_deinitialize();
        return 5;
    }

    std::vector<SimPeer> simulations;
    simulations.reserve(options->count);
    std::unordered_map<ENetPeer*, std::size_t> simulation_by_transport;
    for (std::uint8_t index = 0; index < options->count; ++index) {
        ENetPeer* transport = enet_host_connect(host, &address, 2, 0);
        if (transport == nullptr) {
            std::cerr << "Could not allocate ENet peer " << static_cast<unsigned>(index + 1) << '\n';
            continue;
        }
        SimPeer simulation;
        simulation.transport = transport;
        simulation.name = options->name + '-' + std::to_string(index + 1);
        simulation.incarnation_id = random_nonzero_u64();
        simulation_by_transport.emplace(transport, simulations.size());
        simulations.push_back(std::move(simulation));
    }
    if (simulations.size() != options->count) {
        std::cerr << "Allocated only " << simulations.size() << '/'
                  << static_cast<unsigned>(options->count)
                  << " requested simulator peers\n";
        enet_host_destroy(host);
        enet_deinitialize();
        return 6;
    }

    std::unordered_map<std::uint64_t, ht2mp::protocol::PeerJoined> roster;
    std::optional<ht2mp::protocol::PlayerState> target_state;
    const auto fixed_target = numeric_target(options->follow);
    std::uint64_t next_send_ms = now_ms();
    const std::uint64_t route_epoch_ms = next_send_ms;
    std::uint64_t next_report_ms = next_send_ms + 5000U;
    const std::uint64_t connect_deadline = next_send_ms + 5000;
    const std::uint64_t stop_deadline =
        options->duration_ms == 0 ? 0 : next_send_ms + options->duration_ms;
    std::uint64_t visible_follow_states{};
    std::uint64_t hidden_states{};
    std::optional<ht2mp::protocol::Vec3d> last_emitted_position;
    bool failed{};
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    while (running.load()) {
        ENetEvent event{};
        const int event_result = enet_host_service(host, &event, 10);
        if (event_result < 0) {
            std::cerr << "ENet service failure\n";
            failed = true;
            break;
        }
        if (event_result > 0) {
            const auto simulation_iterator = simulation_by_transport.find(event.peer);
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    if (simulation_iterator != simulation_by_transport.end()) {
                        auto& simulation = simulations[simulation_iterator->second];
                        send_message(event.peer,
                                     ht2mp::protocol::ClientHello{options->profile,
                                                                   options->token,
                                                                   simulation.name,
                                                                   simulation.incarnation_id,
                                                                   options->vehicle_type});
                    }
                    break;
                case ENET_EVENT_TYPE_RECEIVE: {
                    const auto bytes = std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(event.packet->data), event.packet->dataLength);
                    const auto decoded = ht2mp::protocol::decode_packet(bytes);
                    enet_packet_destroy(event.packet);
                    if (!decoded) {
                        std::cerr << "Rejected server packet: " << decoded.detail << '\n';
                        failed = true;
                        break;
                    }
                    if (event.channelID != ht2mp::protocol::channel_for(ht2mp::protocol::message_type(decoded.message))) {
                        std::cerr << "Server used the wrong ENet channel\n";
                        failed = true;
                        break;
                    }
                    if (simulation_iterator == simulation_by_transport.end()) {
                        std::cerr << "Server packet came from an unknown transport\n";
                        failed = true;
                        break;
                    }
                    auto& simulation = simulations[simulation_iterator->second];
                    if (const auto validation = ht2mp::sim::accept_server_message(
                            decoded.message, options->profile, *profile_limits,
                            simulation.server);
                        !validation) {
                        std::cerr << "Rejected server message: " << validation.detail << '\n';
                        failed = true;
                        break;
                    } else if (validation.terminal) {
                        const auto& disconnected =
                            std::get<ht2mp::protocol::Disconnect>(decoded.message);
                        std::cerr << "Server disconnected simulator: "
                                  << disconnected.detail << '\n';
                        simulation.server = {};
                        failed = true;
                        break;
                    }
                    if (const auto* welcome = std::get_if<ht2mp::protocol::ServerWelcome>(&decoded.message)) {
                        std::cout << simulation.name << " joined as player "
                                  << simulation.server.player_id << '\n'
                                  << std::flush;
                    } else if (const auto* joined = std::get_if<ht2mp::protocol::PeerJoined>(&decoded.message)) {
                        roster[joined->player_id] = *joined;
                    } else if (const auto* spawn = std::get_if<ht2mp::protocol::SpawnMetadata>(&decoded.message)) {
                        roster[spawn->player_id] = {spawn->player_id,
                                                    spawn->incarnation_id,
                                                    spawn->vehicle_type,
                                                    spawn->display_name,
                                                    spawn->paint_variant};
                    } else if (const auto* left = std::get_if<ht2mp::protocol::PeerLeft>(&decoded.message)) {
                        roster.erase(left->player_id);
                        if (target_state && target_state->player_id == left->player_id) {
                            target_state.reset();
                        }
                    } else if (const auto* snapshot = std::get_if<ht2mp::protocol::WorldSnapshot>(&decoded.message)) {
                        std::optional<std::uint64_t> target_id = fixed_target;
                        if (!target_id) {
                            for (const auto& [player_id, info] : roster) {
                                if (info.display_name == options->follow) {
                                    target_id = player_id;
                                    break;
                                }
                            }
                        }
                        if (target_id) {
                            const auto state = std::find_if(snapshot->players.begin(), snapshot->players.end(),
                                                            [target_id](const auto& candidate) {
                                                                return candidate.player_id == *target_id;
                                                            });
                            // The coordinator already drops peers whose state is
                            // older than its stale window; sample_time_ms is on the
                            // sender's clock and must not be compared with ours.
                            if (state != snapshot->players.end()) {
                                target_state = *state;
                            } else {
                                target_state.reset();
                            }
                        }
                    }
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                    if (simulation_iterator != simulation_by_transport.end()) {
                        simulations[simulation_iterator->second].server = {};
                    }
                    break;
                case ENET_EVENT_TYPE_NONE:
                    break;
            }
        }

        if (failed) {
            break;
        }

        const auto current_time = now_ms();
        if (stop_deadline != 0 && current_time >= stop_deadline) {
            break;
        }
        if (current_time >= connect_deadline) {
            const bool all_welcomed = std::all_of(simulations.begin(), simulations.end(), [](const auto& simulation) {
                return simulation.server.welcomed;
            });
            if (!all_welcomed) {
                const auto welcomed = static_cast<std::size_t>(std::count_if(
                    simulations.begin(), simulations.end(), [](const auto& simulation) {
                        return simulation.server.welcomed;
                    }));
                std::cerr << "Only " << welcomed << '/' << simulations.size()
                          << " simulator peers were welcomed within five seconds\n";
                failed = true;
                break;
            }
        }
        if (current_time < next_send_ms) {
            continue;
        }
        next_send_ms = current_time + 50;

        const bool target_visible =
            target_state && target_state->has(
                                ht2mp::protocol::PlayerStateFlag::in_world);
        for (std::size_t index = 0; index < simulations.size(); ++index) {
            auto& simulation = simulations[index];
            if (!simulation.server.welcomed) {
                continue;
            }
            ht2mp::protocol::PlayerState state;
            state.session_id = simulation.server.session_id;
            state.player_id = simulation.server.player_id;
            state.incarnation_id = simulation.incarnation_id;
            state.sequence = ++simulation.sequence;
            state.sample_time_ms = current_time;
            state.vehicle_type = options->vehicle_type;
            state.paint_variant = 0U;
            state.orientation.w = 1.0F;
            if (target_visible) {
                state = *target_state;
                state.session_id = simulation.server.session_id;
                state.player_id = simulation.server.player_id;
                state.incarnation_id = simulation.incarnation_id;
                state.sequence = simulation.sequence;
                state.sample_time_ms = current_time;
                state.vehicle_type = options->vehicle_type;
                state.paint_variant = 0U;
                if (options->route == RouteMode::orbit) {
                    const double elapsed_seconds =
                        static_cast<double>(current_time - route_epoch_ms) / 1000.0;
                    const double phase_offset =
                        2.0 * std::numbers::pi * static_cast<double>(index) /
                        static_cast<double>(simulations.size());
                    const auto pose = ht2mp::sim::orbit_pose(
                        elapsed_seconds, options->radius, options->period_seconds,
                        phase_offset);
                    state.position.x += pose.offset.x;
                    state.position.y += pose.offset.y;
                    state.position.z += pose.offset.z;
                    state.orientation = pose.orientation;
                    state.linear_velocity = pose.linear_velocity;
                    state.angular_velocity = pose.angular_velocity;
                } else {
                    const auto forward = ht2mp::sim::ground_forward(target_state->orientation);
                    const double separation = options->distance * static_cast<double>(index + 1);
                    state.position.x -= forward.x * separation;
                    state.position.y -= forward.y * separation;
                }
                // Keep the target's last exact PositionId.  Subtracting a
                // Euclidean separation from road_distance is invalid at a
                // segment boundary (and can produce a negative PositionId).
                // Each game build resolves the logical location locally; the
                // bridge applies the visual world-space offset afterwards.
                ++visible_follow_states;
                if (index == 0) {
                    last_emitted_position = state.position;
                }
            } else {
                ++hidden_states;
            }
            if (!send_message(simulation.transport, state)) {
                std::cerr << "Could not send state for " << simulation.name << '\n';
                failed = true;
                break;
            }
        }
        if (failed) {
            break;
        }
        if (current_time >= next_report_ms) {
            std::cout << "target=" << options->follow
                      << " route=" << (options->route == RouteMode::orbit ? "orbit" : "follow")
                      << " visible=" << (target_visible ? 1 : 0)
                      << " follow_states=" << visible_follow_states
                      << " hidden_states=" << hidden_states;
            if (target_state) {
                std::cout << " player=" << target_state->player_id
                          << " room=" << target_state->location.room_id
                          << " road=" << target_state->location.road_id
                          << " node=" << target_state->location.node_id
                          << " distance="
                          << target_state->location.road_distance
                          << " segment_vector="
                          << target_state->location.road_segment_vector_id
                          << " segment="
                          << target_state->location.road_segment_id
                          << " pos=(" << target_state->position.x << ','
                          << target_state->position.y << ','
                          << target_state->position.z << ')';
            }
            if (last_emitted_position) {
                std::cout << " remote_pos=(" << last_emitted_position->x << ','
                          << last_emitted_position->y << ','
                          << last_emitted_position->z << ')';
            }
            std::cout << '\n' << std::flush;
            next_report_ms = current_time + 5000U;
        }
    }

    for (auto& simulation : simulations) {
        if (simulation.server.welcomed) {
            send_message(simulation.transport,
                         ht2mp::protocol::Disconnect{ht2mp::protocol::DisconnectReason::normal, "simulator stopped"});
        }
        enet_peer_disconnect_later(simulation.transport, 0);
    }
    enet_host_flush(host);
    enet_host_destroy(host);
    enet_deinitialize();
    return failed ? 7 : 0;
}
