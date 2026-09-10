#include "test_support.hpp"

#include "network_adapter.hpp"

#include "ht2mp/ipc/codec.hpp"
#include "ht2mp/protocol/codec.hpp"

#include <enet/enet.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

bool send_server_message(ENetPeer* peer,
                         const ht2mp::protocol::Message& message) {
    const auto encoded = ht2mp::protocol::encode_packet(message);
    HT2MP_CHECK(encoded);
    const auto type = ht2mp::protocol::message_type(message);
    ENetPacket* packet = enet_packet_create(
        encoded.bytes.data(), encoded.bytes.size(),
        ht2mp::protocol::is_reliable(type) ? ENET_PACKET_FLAG_RELIABLE : 0U);
    HT2MP_CHECK(packet != nullptr);
    if (enet_peer_send(peer, ht2mp::protocol::channel_for(type), packet) != 0) {
        enet_packet_destroy(packet);
        HT2MP_CHECK(false);
    }
    return true;
}

std::uint32_t load_u32_le(const std::span<const std::byte> bytes,
                          const std::size_t offset) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[offset + 1U]))
            << 8U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[offset + 2U]))
            << 16U) |
           (static_cast<std::uint32_t>(
                std::to_integer<std::uint8_t>(bytes[offset + 3U]))
            << 24U);
}

bool drain_bridge_frames(HANDLE bridge,
                         std::vector<std::byte>& pending,
                         std::vector<ht2mp::ipc::MessageType>& types) {
    DWORD available{};
    if (!PeekNamedPipe(bridge, nullptr, 0U, nullptr, &available, nullptr)) {
        return false;
    }
    if (available != 0U) {
        const auto old_size = pending.size();
        pending.resize(old_size + available);
        DWORD read{};
        if (!ReadFile(bridge, pending.data() + old_size, available, &read,
                      nullptr) ||
            read != available) {
            return false;
        }
    }

    std::size_t consumed{};
    while (pending.size() - consumed >= ht2mp::ipc::kFrameHeaderSize) {
        const auto bytes = std::span<const std::byte>(pending).subspan(consumed);
        const auto payload_size = load_u32_le(bytes, 8U);
        if (payload_size > ht2mp::ipc::kMaximumPayloadSize) {
            return false;
        }
        const auto frame_size = ht2mp::ipc::kFrameHeaderSize + payload_size;
        if (bytes.size() < frame_size) {
            break;
        }
        ht2mp::ipc::FrameHeader header{};
        std::span<const std::byte> payload{};
        if (ht2mp::ipc::decode_frame(bytes.first(frame_size), header, payload) !=
            ht2mp::ipc::CodecError::none) {
            return false;
        }
        types.push_back(header.type);
        consumed += frame_size;
    }
    if (consumed != 0U) {
        pending.erase(pending.begin(), pending.begin() +
                                           static_cast<std::ptrdiff_t>(consumed));
    }
    return true;
}

bool same_session_welcome_replaces_remote_roster() {
    HT2MP_CHECK(enet_initialize() == 0);
    ENetAddress address{};
    address.port = 0U;
    HT2MP_CHECK(enet_address_set_host(&address, "127.0.0.1") == 0);
    ENetHost* server = enet_host_create(&address, 2U, 2U, 0U, 0U);
    HT2MP_CHECK(server != nullptr);

    const auto pipe_name =
        std::wstring(L"\\\\.\\pipe\\HT2MP-network-test-") +
        std::to_wstring(GetCurrentProcessId()) + L'-' +
        std::to_wstring(GetTickCount64());
    ht2mp::client::PipeServer pipe;
    std::string error;
    HT2MP_CHECK(pipe.create(pipe_name, error));

    HANDLE bridge = INVALID_HANDLE_VALUE;
    std::thread bridge_connector([&] {
        bridge = CreateFileW(pipe_name.c_str(), GENERIC_READ, 0U, nullptr,
                             OPEN_EXISTING, 0U, nullptr);
    });
    const bool pipe_connected = pipe.connect(error);
    bridge_connector.join();
    HT2MP_CHECK(pipe_connected);
    HT2MP_CHECK(bridge != INVALID_HANDLE_VALUE);

    ht2mp::client::NetworkOptions options;
    options.edition = {
        "gog", "gog-05588140",
        "4412a5f695dd016c9f92185b7d2d0be8ad3be787bfe2d77908f5b4d171eedd86"};
    options.endpoint = "127.0.0.1:" + std::to_string(server->address.port);
    options.token = "00000000000000000000000000000000";
    options.player_name = "Local";
    options.vehicle_type = 0U;
    ht2mp::client::NetworkAdapter adapter(
        std::move(options),
        static_cast<std::uint32_t>(ht2mp::ipc::Capability::remote_actors),
        ht2mp::ipc::BridgeMode::active, true);
    HT2MP_CHECK(adapter.start(error));

    // Online admission is intentionally deferred until the bridge proves that
    // the selected appearance is the one actually loaded by the game.
    ht2mp::ipc::PlayerSampleV1 local_sample;
    local_sample.flags = 1U;
    local_sample.orientation[3] = 1.0F;
    local_sample.vehicle_type = 0U;
    ht2mp::client::ReceivedFrame local_frame;
    HT2MP_CHECK(ht2mp::ipc::encode_local_sample(
                    local_sample, local_frame.storage, local_frame.size) ==
                ht2mp::ipc::CodecError::none);
    std::span<const std::byte> local_payload;
    HT2MP_CHECK(ht2mp::ipc::decode_frame(
                    std::span(local_frame.storage).first(local_frame.size),
                    local_frame.header, local_payload) ==
                ht2mp::ipc::CodecError::none);
    HT2MP_CHECK(adapter.handle_bridge_frame(local_frame, error));

    std::size_t hello_count{};
    bool first_membership_sent{};
    bool first_state_sent{};
    bool shutdown_sent{};
    bool second_membership_sent{};
    bool eighth_membership_sent{};
    bool pump_failed{};
    std::vector<std::byte> pending_pipe_bytes;
    std::vector<ht2mp::ipc::MessageType> bridge_messages;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(7);

    while (std::chrono::steady_clock::now() < deadline) {
        if (!adapter.pump(pipe, error)) {
            pump_failed = true;
            HT2MP_CHECK(drain_bridge_frames(
                bridge, pending_pipe_bytes, bridge_messages));
            break;
        }
        HT2MP_CHECK(
            drain_bridge_frames(bridge, pending_pipe_bytes, bridge_messages));

        const bool saw_spawn = std::ranges::find(
                                   bridge_messages,
                                   ht2mp::ipc::MessageType::spawn_remote) !=
                               bridge_messages.end();
        if (saw_spawn && !shutdown_sent) {
            // No PeerLeft is sent. The next welcome deliberately uses the same
            // session ID so only an authoritative welcome reset can remove it.
            for (auto* peer = server->peers;
                 peer != server->peers + server->peerCount; ++peer) {
                if (peer->state == ENET_PEER_STATE_CONNECTED) {
                    HT2MP_CHECK(send_server_message(
                        peer, ht2mp::protocol::Disconnect{
                                  ht2mp::protocol::DisconnectReason::server_shutdown,
                                  "restart"}));
                    enet_host_flush(server);
                    shutdown_sent = true;
                    break;
                }
            }
        }
        ENetEvent event{};
        while (enet_host_service(server, &event, 1U) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                const auto bytes = std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(event.packet->data),
                    event.packet->dataLength);
                const auto decoded = ht2mp::protocol::decode_packet(bytes);
                enet_packet_destroy(event.packet);
                HT2MP_CHECK(decoded);
                if (std::holds_alternative<ht2mp::protocol::ClientHello>(
                        decoded.message)) {
                    ++hello_count;
                    const auto local_player_id = hello_count == 1U ? 100U : 101U;
                    HT2MP_CHECK(send_server_message(
                        event.peer,
                        ht2mp::protocol::ServerWelcome{
                            "gog-05588140", 42U, local_player_id, 1000U,
                            static_cast<std::uint8_t>(
                                ht2mp::protocol::kMaxPlayers),
                            20U}));
                    if (hello_count == 1U) {
                        HT2MP_CHECK(send_server_message(
                            event.peer,
                            ht2mp::protocol::PeerJoined{200U, 300U, 0U,
                                                         "Stale"}));
                        first_membership_sent = true;
                    } else if (hello_count == 2U) {
                        for (std::uint64_t index = 0U; index < 8U; ++index) {
                            HT2MP_CHECK(send_server_message(
                                event.peer,
                                ht2mp::protocol::PeerJoined{
                                    201U + index, 401U + index, 0U,
                                    "Peer" + std::to_string(index + 1U)}));
                        }
                        second_membership_sent = true;
                        eighth_membership_sent = true;
                    }
                    enet_host_flush(server);
                }
            }
        }

        if (first_membership_sent && !first_state_sent) {
            for (auto* peer = server->peers;
                 peer != server->peers + server->peerCount; ++peer) {
                if (peer->state != ENET_PEER_STATE_CONNECTED) continue;
                ht2mp::protocol::PlayerState remote;
                remote.session_id = 42U;
                remote.player_id = 200U;
                remote.incarnation_id = 300U;
                remote.sequence = 1U;
                remote.sample_time_ms = 1000U;
                remote.set(ht2mp::protocol::PlayerStateFlag::in_world, true);
                remote.orientation.w = 1.0F;
                remote.vehicle_type = 0U;
                HT2MP_CHECK(send_server_message(
                    peer,
                    ht2mp::protocol::WorldSnapshot{1000U, {remote}}));
                enet_host_flush(server);
                first_state_sent = true;
                break;
            }
        }
        std::this_thread::yield();
    }

    const bool saw_spawn =
        std::ranges::find(bridge_messages,
                          ht2mp::ipc::MessageType::spawn_remote) !=
        bridge_messages.end();
    const bool saw_despawn =
        std::ranges::find(bridge_messages,
                          ht2mp::ipc::MessageType::despawn_remote) !=
        bridge_messages.end();

    adapter.stop(pipe);
    CloseHandle(bridge);
    pipe.close();
    enet_host_destroy(server);
    enet_deinitialize();

    HT2MP_CHECK(pump_failed);
    HT2MP_CHECK(error == "Coordinator advertised more than seven remote peers");
    HT2MP_CHECK(hello_count >= 2U);
    HT2MP_CHECK(second_membership_sent);
    HT2MP_CHECK(eighth_membership_sent);
    HT2MP_CHECK(saw_spawn);
    HT2MP_CHECK(saw_despawn);
    return true;
}

}  // namespace

int main() {
    return run_test("same-session roster reset and eighth peer fail closed",
                    same_session_welcome_replaces_remote_roster);
}
