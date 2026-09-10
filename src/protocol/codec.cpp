#include "ht2mp/protocol/codec.hpp"

#include "ht2mp/protocol/validation.hpp"

#include <bit>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace ht2mp::protocol {
namespace {

constexpr std::uint32_t kMagic = 0x504D3248U;  // "H2MP" on the wire.

class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8U));
    }

    void u32(std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value));
        u16(static_cast<std::uint16_t>(value >> 16U));
    }

    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value));
        u32(static_cast<std::uint32_t>(value >> 32U));
    }

    void i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

    void token(const Token128& value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void string8(const std::string& value) {
        u8(static_cast<std::uint8_t>(value.size()));
        for (const unsigned char character : value) {
            u8(character);
        }
    }

    [[nodiscard]] std::vector<std::byte>& bytes() noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool u8(std::uint8_t& value) {
        if (remaining() < 1) {
            return fail();
        }
        value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t& value) {
        std::uint8_t low{};
        std::uint8_t high{};
        if (!u8(low) || !u8(high)) {
            return false;
        }
        value = static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8U));
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& value) {
        std::uint16_t low{};
        std::uint16_t high{};
        if (!u16(low) || !u16(high)) {
            return false;
        }
        value = static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 16U);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& value) {
        std::uint32_t low{};
        std::uint32_t high{};
        if (!u32(low) || !u32(high)) {
            return false;
        }
        value = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32U);
        return true;
    }

    [[nodiscard]] bool i32(std::int32_t& value) {
        std::uint32_t bits{};
        if (!u32(bits)) {
            return false;
        }
        value = std::bit_cast<std::int32_t>(bits);
        return true;
    }

    [[nodiscard]] bool f32(float& value) {
        std::uint32_t bits{};
        if (!u32(bits)) {
            return false;
        }
        value = std::bit_cast<float>(bits);
        return true;
    }

    [[nodiscard]] bool f64(double& value) {
        std::uint64_t bits{};
        if (!u64(bits)) {
            return false;
        }
        value = std::bit_cast<double>(bits);
        return true;
    }

    [[nodiscard]] bool token(Token128& value) {
        if (remaining() < value.size()) {
            return fail();
        }
        for (auto& byte : value) {
            byte = bytes_[offset_++];
        }
        return true;
    }

    [[nodiscard]] bool string8(std::string& value, std::size_t maximum) {
        std::uint8_t size{};
        if (!u8(size) || size > maximum || remaining() < size) {
            return fail();
        }
        value.clear();
        value.reserve(size);
        for (std::size_t index = 0; index < size; ++index) {
            value.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes_[offset_++])));
        }
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    [[nodiscard]] bool fail() noexcept {
        ok_ = false;
        return false;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
    bool ok_{true};
};

void write_vec3d(Writer& writer, const Vec3d& value) {
    writer.f64(value.x);
    writer.f64(value.y);
    writer.f64(value.z);
}

void write_vec3f(Writer& writer, const Vec3f& value) {
    writer.f32(value.x);
    writer.f32(value.y);
    writer.f32(value.z);
}

void write_quat(Writer& writer, const Quatf& value) {
    writer.f32(value.x);
    writer.f32(value.y);
    writer.f32(value.z);
    writer.f32(value.w);
}

void write_location(Writer& writer, const WorldLocationV1& value) {
    writer.i32(value.room_id);
    writer.i32(value.road_id);
    writer.i32(value.node_id);
    writer.f64(value.road_distance);
    writer.i32(value.road_segment_vector_id);
    writer.i32(value.road_segment_id);
    writer.i32(value.aux0);
    writer.i32(value.aux1);
}

void write_vehicle(Writer& writer, const VehicleState& value) {
    writer.u8(value.valid ? 1U : 0U);
    writer.u8(std::bit_cast<std::uint8_t>(value.steering));
    for (auto light : value.lights) writer.u8(light);
    for (auto condition : value.condition) writer.f32(condition);
    writer.u8(value.wheels.count);
    for (auto speed : value.wheels.radians_per_second) writer.f32(speed);
    writer.u8(static_cast<std::uint8_t>((value.horn.active ? 1U : 0U) | (value.horn.tone << 1U)));
    writer.u16(value.horn.press_sequence);
}

bool read_vehicle(Reader& reader, VehicleState& value) {
    std::uint8_t valid{}, steering{};
    if (!reader.u8(valid) || valid > 1U || !reader.u8(steering)) return false;
    value.valid = valid != 0U;
    value.steering = std::bit_cast<std::int8_t>(steering);
    for (auto& light : value.lights) if (!reader.u8(light)) return false;
    for (auto& condition : value.condition) if (!reader.f32(condition)) return false;
    if (!reader.u8(value.wheels.count)) return false;
    for (auto& speed : value.wheels.radians_per_second) if (!reader.f32(speed)) return false;
    std::uint8_t horn_flags{};
    if (!reader.u8(horn_flags) || horn_flags > 5U || !reader.u16(value.horn.press_sequence)) return false;
    value.horn.active = (horn_flags & 1U) != 0;
    value.horn.tone = horn_flags >> 1U;
    return true;
}

void write_environment(Writer& writer, const EnvironmentState& value) {
    writer.u64(value.session_id);
    writer.f64(value.day_hours);
    writer.f64(value.weather_phase);
    writer.f64(value.variation_phase);
    writer.f32(value.day_duration_s);
    writer.f32(value.weather_duration_s);
    writer.f32(value.weather_bias);
    writer.u8(value.weather_preset);
}

bool read_environment(Reader& reader, EnvironmentState& value) {
    return reader.u64(value.session_id) && reader.f64(value.day_hours) &&
           reader.f64(value.weather_phase) && reader.f64(value.variation_phase) &&
           reader.f32(value.day_duration_s) && reader.f32(value.weather_duration_s) &&
           reader.f32(value.weather_bias) && reader.u8(value.weather_preset);
}

void write_state(Writer& writer, const PlayerState& value) {
    writer.u64(value.session_id);
    writer.u64(value.player_id);
    writer.u64(value.incarnation_id);
    writer.u32(value.sequence);
    writer.u64(value.sample_time_ms);
    writer.u8(value.flags);
    write_vec3d(writer, value.position);
    write_quat(writer, value.orientation);
    write_vec3f(writer, value.linear_velocity);
    write_vec3f(writer, value.angular_velocity);
    writer.u16(value.vehicle_type);
    writer.u8(value.paint_variant);
    write_location(writer, value.location);
    write_vehicle(writer, value.vehicle);
}

bool read_vec3d(Reader& reader, Vec3d& value) {
    return reader.f64(value.x) && reader.f64(value.y) && reader.f64(value.z);
}

bool read_vec3f(Reader& reader, Vec3f& value) {
    return reader.f32(value.x) && reader.f32(value.y) && reader.f32(value.z);
}

bool read_quat(Reader& reader, Quatf& value) {
    return reader.f32(value.x) && reader.f32(value.y) && reader.f32(value.z) && reader.f32(value.w);
}

bool read_location(Reader& reader, WorldLocationV1& value) {
    return reader.i32(value.room_id) && reader.i32(value.road_id) && reader.i32(value.node_id) &&
           reader.f64(value.road_distance) && reader.i32(value.road_segment_vector_id) &&
           reader.i32(value.road_segment_id) && reader.i32(value.aux0) && reader.i32(value.aux1);
}

bool read_state(Reader& reader, PlayerState& value) {
    return reader.u64(value.session_id) && reader.u64(value.player_id) && reader.u64(value.incarnation_id) &&
           reader.u32(value.sequence) && reader.u64(value.sample_time_ms) && reader.u8(value.flags) &&
           read_vec3d(reader, value.position) && read_quat(reader, value.orientation) &&
           read_vec3f(reader, value.linear_velocity) && read_vec3f(reader, value.angular_velocity) &&
           reader.u16(value.vehicle_type) && reader.u8(value.paint_variant) && read_location(reader, value.location) &&
           read_vehicle(reader, value.vehicle);
}

bool known_type(std::uint8_t type) noexcept {
    switch (static_cast<MessageType>(type)) {
        case MessageType::client_hello:
        case MessageType::server_welcome:
        case MessageType::peer_joined:
        case MessageType::peer_left:
        case MessageType::spawn_metadata:
        case MessageType::ping:
        case MessageType::pong:
        case MessageType::disconnect:
        case MessageType::player_state:
        case MessageType::world_snapshot:
            return true;
    }
    return false;
}

}  // namespace

MessageType message_type(const Message& message) noexcept {
    return std::visit(
        []<typename T>(const T&) {
            if constexpr (std::is_same_v<T, ClientHello>) {
                return MessageType::client_hello;
            } else if constexpr (std::is_same_v<T, ServerWelcome>) {
                return MessageType::server_welcome;
            } else if constexpr (std::is_same_v<T, PeerJoined>) {
                return MessageType::peer_joined;
            } else if constexpr (std::is_same_v<T, PeerLeft>) {
                return MessageType::peer_left;
            } else if constexpr (std::is_same_v<T, SpawnMetadata>) {
                return MessageType::spawn_metadata;
            } else if constexpr (std::is_same_v<T, Ping>) {
                return MessageType::ping;
            } else if constexpr (std::is_same_v<T, Pong>) {
                return MessageType::pong;
            } else if constexpr (std::is_same_v<T, Disconnect>) {
                return MessageType::disconnect;
            } else if constexpr (std::is_same_v<T, PlayerState>) {
                return MessageType::player_state;
            } else {
                return MessageType::world_snapshot;
            }
        },
        message);
}

std::uint8_t channel_for(MessageType type) noexcept {
    return type == MessageType::player_state || type == MessageType::world_snapshot ? kStateChannel
                                                                                    : kReliableChannel;
}

bool is_reliable(MessageType type) noexcept {
    return channel_for(type) == kReliableChannel;
}

EncodeResult encode_packet(const Message& message) {
    const auto validation = validate(message);
    if (!validation) {
        return {EncodeError::invalid_value, validation.detail, {}};
    }

    Writer writer;
    writer.u32(kMagic);
    writer.u8(kProtocolVersion);
    writer.u8(static_cast<std::uint8_t>(message_type(message)));
    writer.u16(0);  // Header flags are reserved in HT2MP/6.
    writer.u16(0);  // Payload size is patched below.
    writer.u16(0);  // Reserved.

    std::visit(
        [&writer]<typename T>(const T& value) {
            if constexpr (std::is_same_v<T, ClientHello>) {
                writer.u64(value.incarnation_id);
                writer.token(value.token);
                writer.u16(value.vehicle_type);
                writer.u8(value.paint_variant);
                writer.string8(value.profile_id);
                writer.string8(value.display_name);
            } else if constexpr (std::is_same_v<T, ServerWelcome>) {
                writer.u64(value.session_id);
                writer.u64(value.player_id);
                writer.u64(value.server_time_ms);
                writer.u8(value.max_players);
                writer.u8(value.state_hz);
                writer.string8(value.profile_id);
            } else if constexpr (std::is_same_v<T, PeerJoined> || std::is_same_v<T, SpawnMetadata>) {
                writer.u64(value.player_id);
                writer.u64(value.incarnation_id);
                writer.u16(value.vehicle_type);
                writer.u8(value.paint_variant);
                writer.string8(value.display_name);
            } else if constexpr (std::is_same_v<T, PeerLeft>) {
                writer.u64(value.player_id);
                writer.u64(value.incarnation_id);
                writer.u8(static_cast<std::uint8_t>(value.reason));
            } else if constexpr (std::is_same_v<T, Ping>) {
                writer.u64(value.nonce);
                writer.u64(value.client_time_ms);
            } else if constexpr (std::is_same_v<T, Pong>) {
                writer.u64(value.nonce);
                writer.u64(value.client_time_ms);
                writer.u64(value.server_time_ms);
            } else if constexpr (std::is_same_v<T, Disconnect>) {
                writer.u8(static_cast<std::uint8_t>(value.reason));
                writer.string8(value.detail);
            } else if constexpr (std::is_same_v<T, PlayerState>) {
                write_state(writer, value);
            } else if constexpr (std::is_same_v<T, WorldSnapshot>) {
                writer.u64(value.server_time_ms);
                write_environment(writer, value.environment);
                writer.u8(static_cast<std::uint8_t>(value.players.size()));
                for (const auto& state : value.players) {
                    write_state(writer, state);
                }
            }
        },
        message);

    auto bytes = std::move(writer).take();
    if (bytes.size() > kMaxPacketSize || bytes.size() - kPacketHeaderSize > std::numeric_limits<std::uint16_t>::max()) {
        return {EncodeError::packet_too_large, "encoded packet exceeds HT2MP/6 limit", {}};
    }
    const auto payload_size = static_cast<std::uint16_t>(bytes.size() - kPacketHeaderSize);
    bytes[8] = static_cast<std::byte>(payload_size & 0xFFU);
    bytes[9] = static_cast<std::byte>(payload_size >> 8U);
    return {EncodeError::none, {}, std::move(bytes)};
}

DecodeResult decode_packet(std::span<const std::byte> packet) {
    if (packet.size() < kPacketHeaderSize) {
        return {DecodeError::too_short, "packet is shorter than the HT2MP/6 header", Disconnect{}};
    }
    if (packet.size() > kMaxPacketSize) {
        return {DecodeError::too_large, "packet exceeds the HT2MP/6 limit", Disconnect{}};
    }

    Reader header(packet.first(kPacketHeaderSize));
    std::uint32_t magic{};
    std::uint8_t version{};
    std::uint8_t raw_type{};
    std::uint16_t flags{};
    std::uint16_t payload_size{};
    std::uint16_t reserved{};
    if (!header.u32(magic) || !header.u8(version) || !header.u8(raw_type) || !header.u16(flags) ||
        !header.u16(payload_size) || !header.u16(reserved)) {
        return {DecodeError::too_short, "truncated header", Disconnect{}};
    }
    if (magic != kMagic) {
        return {DecodeError::bad_magic, "invalid packet magic", Disconnect{}};
    }
    if (version != kProtocolVersion) {
        return {DecodeError::unsupported_version, "unsupported protocol version", Disconnect{}};
    }
    if (!known_type(raw_type)) {
        return {DecodeError::unknown_message, "unknown message type", Disconnect{}};
    }
    if (flags != 0 || reserved != 0) {
        return {DecodeError::invalid_header, "reserved header bits are non-zero", Disconnect{}};
    }
    if (payload_size != packet.size() - kPacketHeaderSize) {
        return {payload_size > packet.size() - kPacketHeaderSize ? DecodeError::truncated : DecodeError::trailing_data,
                "payload length does not match packet length",
                Disconnect{}};
    }

    Reader reader(packet.subspan(kPacketHeaderSize));
    Message message{Disconnect{}};
    const auto type = static_cast<MessageType>(raw_type);
    bool parsed = false;
    switch (type) {
        case MessageType::client_hello: {
            ClientHello value;
            parsed = reader.u64(value.incarnation_id) && reader.token(value.token) && reader.u16(value.vehicle_type) &&
                     reader.u8(value.paint_variant) &&
                     reader.string8(value.profile_id, kMaxProfileIdBytes) &&
                     reader.string8(value.display_name, kMaxDisplayNameBytes);
            message = std::move(value);
            break;
        }
        case MessageType::server_welcome: {
            ServerWelcome value;
            parsed = reader.u64(value.session_id) && reader.u64(value.player_id) && reader.u64(value.server_time_ms) &&
                     reader.u8(value.max_players) && reader.u8(value.state_hz) &&
                     reader.string8(value.profile_id, kMaxProfileIdBytes);
            message = std::move(value);
            break;
        }
        case MessageType::peer_joined: {
            PeerJoined value;
            parsed = reader.u64(value.player_id) && reader.u64(value.incarnation_id) && reader.u16(value.vehicle_type) &&
                     reader.u8(value.paint_variant) &&
                     reader.string8(value.display_name, kMaxDisplayNameBytes);
            message = std::move(value);
            break;
        }
        case MessageType::peer_left: {
            PeerLeft value;
            std::uint8_t reason{};
            parsed = reader.u64(value.player_id) && reader.u64(value.incarnation_id) && reader.u8(reason);
            value.reason = static_cast<DisconnectReason>(reason);
            message = value;
            break;
        }
        case MessageType::spawn_metadata: {
            SpawnMetadata value;
            parsed = reader.u64(value.player_id) && reader.u64(value.incarnation_id) && reader.u16(value.vehicle_type) &&
                     reader.u8(value.paint_variant) &&
                     reader.string8(value.display_name, kMaxDisplayNameBytes);
            message = std::move(value);
            break;
        }
        case MessageType::ping: {
            Ping value;
            parsed = reader.u64(value.nonce) && reader.u64(value.client_time_ms);
            message = value;
            break;
        }
        case MessageType::pong: {
            Pong value;
            parsed = reader.u64(value.nonce) && reader.u64(value.client_time_ms) && reader.u64(value.server_time_ms);
            message = value;
            break;
        }
        case MessageType::disconnect: {
            Disconnect value;
            std::uint8_t reason{};
            parsed = reader.u8(reason) && reader.string8(value.detail, kMaxDisconnectTextBytes);
            value.reason = static_cast<DisconnectReason>(reason);
            message = std::move(value);
            break;
        }
        case MessageType::player_state: {
            PlayerState value;
            parsed = read_state(reader, value);
            message = value;
            break;
        }
        case MessageType::world_snapshot: {
            WorldSnapshot value;
            std::uint8_t count{};
            parsed = reader.u64(value.server_time_ms) && read_environment(reader, value.environment) &&
                     reader.u8(count) && count <= kMaxSnapshotPlayers;
            if (parsed) {
                value.players.resize(count);
                for (auto& state : value.players) {
                    if (!read_state(reader, state)) {
                        parsed = false;
                        break;
                    }
                }
            }
            message = std::move(value);
            break;
        }
    }

    if (!parsed || !reader.ok()) {
        return {DecodeError::truncated, "message payload is truncated or has an invalid length", Disconnect{}};
    }
    if (reader.remaining() != 0) {
        return {DecodeError::trailing_data, "message payload contains trailing data", Disconnect{}};
    }
    const auto validation = validate(message);
    if (!validation) {
        return {DecodeError::invalid_value, validation.detail, Disconnect{}};
    }
    return {DecodeError::none, {}, std::move(message)};
}

}  // namespace ht2mp::protocol
