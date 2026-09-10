#include "ht2mp/ipc/codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace ht2mp::ipc {
namespace {

constexpr bool valid_message_type(const std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(MessageType::bridge_hello) &&
         value <= static_cast<std::uint16_t>(MessageType::environment);
}

template <typename T>
constexpr bool value_in_range(const T value, const T first, const T last) noexcept {
  return value >= first && value <= last;
}

CodecError finish_payload(
    const MessageType type,
    const std::span<const std::byte> payload,
    const std::span<std::byte> output,
    std::size_t& encoded_size) noexcept {
  return encode_frame(type, 0U, payload, output, encoded_size);
}

} // namespace

bool PayloadWriter::put_u8(const std::uint8_t value) noexcept {
  if (cursor_ >= output_.size()) {
    return false;
  }
  output_[cursor_++] = static_cast<std::byte>(value);
  return true;
}

bool PayloadWriter::put_u16(const std::uint16_t value) noexcept {
  if (output_.size() - cursor_ < 2U) return false;
  output_[cursor_++] = static_cast<std::byte>(value & 0xFFU);
  output_[cursor_++] = static_cast<std::byte>((value >> 8U) & 0xFFU);
  return true;
}

bool PayloadWriter::put_u32(const std::uint32_t value) noexcept {
  if (output_.size() - cursor_ < 4U) return false;
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    output_[cursor_++] = static_cast<std::byte>((value >> shift) & 0xFFU);
  }
  return true;
}

bool PayloadWriter::put_u64(const std::uint64_t value) noexcept {
  if (output_.size() - cursor_ < 8U) return false;
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    output_[cursor_++] = static_cast<std::byte>((value >> shift) & 0xFFU);
  }
  return true;
}

bool PayloadWriter::put_i32(const std::int32_t value) noexcept {
  return put_u32(std::bit_cast<std::uint32_t>(value));
}

bool PayloadWriter::put_f32(const float value) noexcept {
  return put_u32(std::bit_cast<std::uint32_t>(value));
}

bool PayloadWriter::put_f64(const double value) noexcept {
  return put_u64(std::bit_cast<std::uint64_t>(value));
}

bool PayloadWriter::put_bytes(const std::span<const std::byte> value) noexcept {
  if (value.size() > output_.size() - cursor_) {
    return false;
  }
  if (!value.empty()) {
    std::memcpy(output_.data() + cursor_, value.data(), value.size());
  }
  cursor_ += value.size();
  return true;
}

bool PayloadReader::get_u8(std::uint8_t& value) noexcept {
  if (cursor_ >= input_.size()) {
    return false;
  }
  value = std::to_integer<std::uint8_t>(input_[cursor_++]);
  return true;
}

bool PayloadReader::get_u16(std::uint16_t& value) noexcept {
  if (input_.size() - cursor_ < 2U) return false;
  value = std::to_integer<std::uint8_t>(input_[cursor_]) |
          (static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(input_[cursor_ + 1U])) << 8U);
  cursor_ += 2U;
  return true;
}

bool PayloadReader::get_u32(std::uint32_t& value) noexcept {
  if (input_.size() - cursor_ < 4U) return false;
  value = 0U;
  for (unsigned shift = 0U; shift < 32U; shift += 8U) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(input_[cursor_++])) << shift;
  }
  return true;
}

bool PayloadReader::get_u64(std::uint64_t& value) noexcept {
  if (input_.size() - cursor_ < 8U) return false;
  value = 0U;
  for (unsigned shift = 0U; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(input_[cursor_++])) << shift;
  }
  return true;
}

bool PayloadReader::get_i32(std::int32_t& value) noexcept {
  std::uint32_t bits{};
  if (!get_u32(bits)) {
    return false;
  }
  value = std::bit_cast<std::int32_t>(bits);
  return true;
}

bool PayloadReader::get_f32(float& value) noexcept {
  std::uint32_t bits{};
  if (!get_u32(bits)) {
    return false;
  }
  value = std::bit_cast<float>(bits);
  return true;
}

bool PayloadReader::get_f64(double& value) noexcept {
  std::uint64_t bits{};
  if (!get_u64(bits)) {
    return false;
  }
  value = std::bit_cast<double>(bits);
  return true;
}

bool PayloadReader::get_bytes(const std::span<std::byte> value) noexcept {
  if (value.size() > input_.size() - cursor_) {
    return false;
  }
  if (!value.empty()) {
    std::memcpy(value.data(), input_.data() + cursor_, value.size());
  }
  cursor_ += value.size();
  return true;
}

CodecError encode_frame(
    const MessageType type,
    const std::uint32_t sequence,
    const std::span<const std::byte> payload,
    const std::span<std::byte> output,
    std::size_t& encoded_size) noexcept {
  encoded_size = 0U;
  const auto raw_type = static_cast<std::uint16_t>(type);
  if (!valid_message_type(raw_type)) {
    return CodecError::invalid_message_type;
  }
  if (payload.size() > kMaximumPayloadSize) {
    return CodecError::payload_too_large;
  }
  if (output.size() < kFrameHeaderSize + payload.size()) {
    return CodecError::buffer_too_small;
  }

  PayloadWriter writer(output);
  if (!writer.put_u32(kFrameMagic) || !writer.put_u16(kProtocolVersion) ||
      !writer.put_u16(raw_type) ||
      !writer.put_u32(static_cast<std::uint32_t>(payload.size())) ||
      !writer.put_u32(sequence) || !writer.put_bytes(payload)) {
    return CodecError::buffer_too_small;
  }
  encoded_size = writer.size();
  return CodecError::none;
}

CodecError decode_frame(
    const std::span<const std::byte> frame,
    FrameHeader& header,
    std::span<const std::byte>& payload) noexcept {
  payload = {};
  if (frame.size() < kFrameHeaderSize) {
    return CodecError::truncated;
  }
  PayloadReader reader(frame.first(kFrameHeaderSize));
  std::uint32_t magic{};
  std::uint16_t version{};
  std::uint16_t raw_type{};
  if (!reader.get_u32(magic) || !reader.get_u16(version) ||
      !reader.get_u16(raw_type) || !reader.get_u32(header.payload_size) ||
      !reader.get_u32(header.sequence)) {
    return CodecError::truncated;
  }
  if (magic != kFrameMagic) {
    return CodecError::invalid_magic;
  }
  if (version != kProtocolVersion) {
    return CodecError::unsupported_version;
  }
  if (!valid_message_type(raw_type)) {
    return CodecError::invalid_message_type;
  }
  if (header.payload_size > kMaximumPayloadSize) {
    return CodecError::payload_too_large;
  }
  if (frame.size() < kFrameHeaderSize + header.payload_size) {
    return CodecError::truncated;
  }
  if (frame.size() != kFrameHeaderSize + header.payload_size) {
    return CodecError::trailing_bytes;
  }
  header.type = static_cast<MessageType>(raw_type);
  payload = frame.subspan(kFrameHeaderSize, header.payload_size);
  return CodecError::none;
}

CodecError encode_bridge_hello(
    const BridgeHelloV1& message,
    const std::span<std::byte> output,
    std::size_t& encoded_size) noexcept {
  std::array<std::byte, 60U> payload{};
  PayloadWriter writer(payload);
  if (!writer.put_u32(message.abi_version) || !writer.put_u64(message.run_id) ||
      !writer.put_bytes(std::as_bytes(std::span(message.nonce))) ||
      !writer.put_bytes(std::as_bytes(std::span(message.profile_id)))) {
    return CodecError::buffer_too_small;
  }
  return finish_payload(MessageType::bridge_hello, payload, output, encoded_size);
}

CodecError decode_bridge_hello(
    const std::span<const std::byte> payload,
    BridgeHelloV1& message) noexcept {
  PayloadReader reader(payload);
  if (!reader.get_u32(message.abi_version) || !reader.get_u64(message.run_id) ||
      !reader.get_bytes(std::as_writable_bytes(std::span(message.nonce))) ||
      !reader.get_bytes(std::as_writable_bytes(std::span(message.profile_id)))) {
    return CodecError::truncated;
  }
  return reader.remaining() == 0U ? CodecError::none : CodecError::trailing_bytes;
}

CodecError encode_bridge_ready(
    const BridgeReadyV1& message,
    const std::span<std::byte> output,
    std::size_t& encoded_size) noexcept {
  std::array<std::byte, 12U> payload{};
  PayloadWriter writer(payload);
  if (!writer.put_u32(static_cast<std::uint32_t>(message.result)) ||
      !writer.put_u32(static_cast<std::uint32_t>(message.mode)) ||
      !writer.put_u32(message.capabilities)) {
    return CodecError::buffer_too_small;
  }
  return finish_payload(MessageType::bridge_ready, payload, output, encoded_size);
}

CodecError decode_bridge_ready(
    const std::span<const std::byte> payload,
    BridgeReadyV1& message) noexcept {
  PayloadReader reader(payload);
  std::uint32_t result{};
  std::uint32_t mode{};
  if (!reader.get_u32(result) || !reader.get_u32(mode) ||
      !reader.get_u32(message.capabilities)) {
    return CodecError::truncated;
  }
  if (!value_in_range(result, 0U, static_cast<std::uint32_t>(BootstrapResult::internal_error)) ||
      !value_in_range(mode, static_cast<std::uint32_t>(BridgeMode::observer),
                      static_cast<std::uint32_t>(BridgeMode::safe))) {
    return CodecError::invalid_value;
  }
  message.result = static_cast<BootstrapResult>(result);
  message.mode = static_cast<BridgeMode>(mode);
  return reader.remaining() == 0U ? CodecError::none : CodecError::trailing_bytes;
}

CodecError encode_bridge_status(
    const BridgeStatusV1& message,
    const std::span<std::byte> output,
    std::size_t& encoded_size) noexcept {
  std::array<std::byte, 168U> payload{};
  PayloadWriter writer(payload);
  if (!writer.put_u32(static_cast<std::uint32_t>(message.code)) ||
      !writer.put_u32(static_cast<std::uint32_t>(message.mode)) ||
      !writer.put_bytes(std::as_bytes(std::span(message.detail)))) {
    return CodecError::buffer_too_small;
  }
  return finish_payload(MessageType::bridge_status, payload, output, encoded_size);
}

CodecError decode_bridge_status(
    const std::span<const std::byte> payload,
    BridgeStatusV1& message) noexcept {
  PayloadReader reader(payload);
  std::uint32_t code{};
  std::uint32_t mode{};
  if (!reader.get_u32(code) || !reader.get_u32(mode) ||
      !reader.get_bytes(std::as_writable_bytes(std::span(message.detail)))) {
    return CodecError::truncated;
  }
  if (!value_in_range(code, 0U, static_cast<std::uint32_t>(BridgeStatusCode::background_tick_stalled)) ||
      !value_in_range(mode, static_cast<std::uint32_t>(BridgeMode::observer),
                      static_cast<std::uint32_t>(BridgeMode::safe))) {
    return CodecError::invalid_value;
  }
  message.code = static_cast<BridgeStatusCode>(code);
  message.mode = static_cast<BridgeMode>(mode);
  return reader.remaining() == 0U ? CodecError::none : CodecError::trailing_bytes;
}

CodecError encode_set_observer_mode(
    const SetObserverModeV1& message,
    const std::span<std::byte> output,
    std::size_t& encoded_size) noexcept {
  const std::array payload{message.enabled ? std::byte{1U} : std::byte{0U}};
  return finish_payload(MessageType::set_observer_mode, payload, output, encoded_size);
}

CodecError decode_set_observer_mode(
    const std::span<const std::byte> payload,
    SetObserverModeV1& message) noexcept {
  if (payload.size() != 1U) {
    return payload.empty() ? CodecError::truncated : CodecError::trailing_bytes;
  }
  const auto value = std::to_integer<std::uint8_t>(payload.front());
  if (value > 1U) {
    return CodecError::invalid_value;
  }
  message.enabled = value != 0U;
  return CodecError::none;
}

CodecError encode_enter_safe_mode(
    const EnterSafeModeV1& message,
    const std::span<std::byte> output,
    std::size_t& encoded_size) noexcept {
  std::array<std::byte, 4U> payload{};
  PayloadWriter writer(payload);
  if (!writer.put_u32(message.reason)) {
    return CodecError::buffer_too_small;
  }
  return finish_payload(MessageType::enter_safe_mode, payload, output, encoded_size);
}

CodecError decode_enter_safe_mode(
    const std::span<const std::byte> payload,
    EnterSafeModeV1& message) noexcept {
  PayloadReader reader(payload);
  if (!reader.get_u32(message.reason)) {
    return CodecError::truncated;
  }
  return reader.remaining() == 0U ? CodecError::none : CodecError::trailing_bytes;
}

namespace {

constexpr std::size_t kPlayerSamplePayloadSize = 378U;

bool write_player_sample(PayloadWriter& writer, const PlayerSampleV1& message) noexcept {
  if (!writer.put_u64(message.player_id) ||
      !writer.put_u64(message.incarnation_id) ||
      !writer.put_u64(message.sample_time_ms) ||
      !writer.put_u64(message.receive_time_us) ||
      !writer.put_u32(message.sequence) || !writer.put_u8(message.flags)) {
    return false;
  }
  for (const auto value : message.position) {
    if (!writer.put_f64(value)) return false;
  }
  for (const auto value : message.orientation) {
    if (!writer.put_f32(value)) return false;
  }
  for (const auto value : message.linear_velocity) {
    if (!writer.put_f32(value)) return false;
  }
  for (const auto value : message.angular_velocity) {
    if (!writer.put_f32(value)) return false;
  }
  std::uint8_t valid{message.vehicle.valid ? std::uint8_t{1} : std::uint8_t{0}};
  std::uint8_t steering{std::bit_cast<std::uint8_t>(message.vehicle.steering)};
  if (!writer.put_u8(valid) || valid > 1U || !writer.put_u8(steering)) return false;
  for (const auto& light : message.vehicle.lights) if (!writer.put_u8(light)) return false;
  for (const auto& condition : message.vehicle.condition) if (!writer.put_f32(condition)) return false;
  if (!writer.put_u8(message.vehicle.wheels.count)) return false;
  for (const auto speed : message.vehicle.wheels.radians_per_second) if (!writer.put_f32(speed)) return false;
  if (!writer.put_u8(static_cast<std::uint8_t>((message.vehicle.horn.active ? 1U : 0U) | (message.vehicle.horn.tone << 1U))) ||
      !writer.put_u16(message.vehicle.horn.press_sequence)) return false;
  if (!ht2mp::protocol::valid_vehicle_state(message.vehicle)) return false;
  const auto& location = message.location;
  return writer.put_u16(message.vehicle_type) && writer.put_u8(message.paint_variant) &&
         writer.put_i32(location.room_id) && writer.put_i32(location.road_id) &&
         writer.put_i32(location.node_id) && writer.put_f64(location.road_distance) &&
         writer.put_i32(location.road_segment_vector_id) &&
         writer.put_i32(location.road_segment_id) && writer.put_i32(location.aux0) &&
         writer.put_i32(location.aux1);
}

bool read_player_sample(PayloadReader& reader, PlayerSampleV1& message) noexcept {
  if (!reader.get_u64(message.player_id) ||
      !reader.get_u64(message.incarnation_id) ||
      !reader.get_u64(message.sample_time_ms) ||
      !reader.get_u64(message.receive_time_us) ||
      !reader.get_u32(message.sequence) || !reader.get_u8(message.flags)) {
    return false;
  }
  for (auto& value : message.position) {
    if (!reader.get_f64(value)) return false;
  }
  for (auto& value : message.orientation) {
    if (!reader.get_f32(value)) return false;
  }
  for (auto& value : message.linear_velocity) {
    if (!reader.get_f32(value)) return false;
  }
  for (auto& value : message.angular_velocity) {
    if (!reader.get_f32(value)) return false;
  }
  std::uint8_t valid{};
  std::uint8_t steering{};
  if (!reader.get_u8(valid) || valid > 1U || !reader.get_u8(steering)) return false;
  message.vehicle.valid = valid != 0U;
  message.vehicle.steering = std::bit_cast<std::int8_t>(steering);
  for (auto& light : message.vehicle.lights) if (!reader.get_u8(light)) return false;
  for (auto& condition : message.vehicle.condition) if (!reader.get_f32(condition)) return false;
  if (!reader.get_u8(message.vehicle.wheels.count)) return false;
  for (auto& speed : message.vehicle.wheels.radians_per_second) if (!reader.get_f32(speed)) return false;
  std::uint8_t horn_flags{};
  if (!reader.get_u8(horn_flags) || horn_flags > 5U || !reader.get_u16(message.vehicle.horn.press_sequence)) return false;
  message.vehicle.horn.active = (horn_flags & 1U) != 0;
  message.vehicle.horn.tone = horn_flags >> 1U;
  if (!ht2mp::protocol::valid_vehicle_state(message.vehicle)) return false;
  auto& location = message.location;
  return reader.get_u16(message.vehicle_type) && reader.get_u8(message.paint_variant) &&
         reader.get_i32(location.room_id) && reader.get_i32(location.road_id) &&
         reader.get_i32(location.node_id) && reader.get_f64(location.road_distance) &&
         reader.get_i32(location.road_segment_vector_id) &&
         reader.get_i32(location.road_segment_id) && reader.get_i32(location.aux0) &&
         reader.get_i32(location.aux1);
}

CodecError encode_player_sample_as(const MessageType type,
                                   const PlayerSampleV1& message,
                                   const std::span<std::byte> output,
                                   std::size_t& encoded_size) noexcept {
  std::array<std::byte, kPlayerSamplePayloadSize> payload{};
  PayloadWriter writer(payload);
  if (!write_player_sample(writer, message) || writer.size() != payload.size()) {
    return CodecError::buffer_too_small;
  }
  return finish_payload(type, payload, output, encoded_size);
}

} // namespace

CodecError encode_local_sample(const PlayerSampleV1& message,
                               const std::span<std::byte> output,
                               std::size_t& encoded_size) noexcept {
  return encode_player_sample_as(MessageType::local_sample, message, output,
                                 encoded_size);
}

CodecError encode_remote_sample(const PlayerSampleV1& message,
                                const std::span<std::byte> output,
                                std::size_t& encoded_size) noexcept {
  return encode_player_sample_as(MessageType::remote_sample, message, output,
                                 encoded_size);
}

CodecError decode_player_sample(const std::span<const std::byte> payload,
                                PlayerSampleV1& message) noexcept {
  if (payload.size() < kPlayerSamplePayloadSize) {
    return CodecError::truncated;
  }
  if (payload.size() > kPlayerSamplePayloadSize) {
    return CodecError::trailing_bytes;
  }
  PayloadReader reader(payload);
  if (!read_player_sample(reader, message)) {
    return CodecError::truncated;
  }
  return reader.remaining() == 0U ? CodecError::none : CodecError::trailing_bytes;
}

CodecError encode_spawn_remote(const SpawnRemoteV1& message,
                               const std::span<std::byte> output,
                               std::size_t& encoded_size) noexcept {
  std::array<std::byte, 52U> payload{};
  PayloadWriter writer(payload);
  if (!writer.put_u64(message.player_id) ||
      !writer.put_u64(message.incarnation_id) ||
      !writer.put_u16(message.vehicle_type) ||
      !writer.put_u8(message.paint_variant) ||
      !writer.put_bytes(std::as_bytes(std::span(message.display_name)))) {
    return CodecError::buffer_too_small;
  }
  return finish_payload(MessageType::spawn_remote, payload, output, encoded_size);
}

CodecError decode_spawn_remote(const std::span<const std::byte> payload,
                               SpawnRemoteV1& message) noexcept {
  PayloadReader reader(payload);
  if (!reader.get_u64(message.player_id) ||
      !reader.get_u64(message.incarnation_id) ||
      !reader.get_u16(message.vehicle_type) ||
      !reader.get_u8(message.paint_variant) ||
      !reader.get_bytes(std::as_writable_bytes(std::span(message.display_name)))) {
    return CodecError::truncated;
  }
  if (reader.remaining() != 0U) return CodecError::trailing_bytes;
  if (std::find(message.display_name.begin(), message.display_name.end(), '\0') ==
      message.display_name.end()) {
    return CodecError::invalid_value;
  }
  return CodecError::none;
}

CodecError encode_despawn_remote(const DespawnRemoteV1& message,
                                 const std::span<std::byte> output,
                                 std::size_t& encoded_size) noexcept {
  std::array<std::byte, 16U> payload{};
  PayloadWriter writer(payload);
  if (!writer.put_u64(message.player_id) ||
      !writer.put_u64(message.incarnation_id)) {
    return CodecError::buffer_too_small;
  }
  return finish_payload(MessageType::despawn_remote, payload, output, encoded_size);
}

CodecError decode_despawn_remote(const std::span<const std::byte> payload,
                                 DespawnRemoteV1& message) noexcept {
  PayloadReader reader(payload);
  if (!reader.get_u64(message.player_id) ||
      !reader.get_u64(message.incarnation_id)) {
    return CodecError::truncated;
  }
  return reader.remaining() == 0U ? CodecError::none : CodecError::trailing_bytes;
}

CodecError encode_environment(const EnvironmentV1& message, std::span<std::byte> output,
                              std::size_t& encoded_size) noexcept {
  if (!ht2mp::protocol::valid_environment(message.state)) return CodecError::invalid_value;
  std::array<std::byte, 61> payload{};
  PayloadWriter writer(payload);
  const auto& state = message.state;
  if (!writer.put_u64(message.server_time_ms) || !writer.put_u64(message.receive_time_us) ||
      !writer.put_u64(state.session_id) || !writer.put_f64(state.day_hours) ||
      !writer.put_f64(state.weather_phase) || !writer.put_f64(state.variation_phase) ||
      !writer.put_f32(state.day_duration_s) || !writer.put_f32(state.weather_duration_s) ||
      !writer.put_f32(state.weather_bias) || !writer.put_u8(state.weather_preset)) return CodecError::buffer_too_small;
  return finish_payload(MessageType::environment, payload, output, encoded_size);
}

CodecError decode_environment(std::span<const std::byte> payload, EnvironmentV1& message) noexcept {
  PayloadReader reader(payload);
  auto& state = message.state;
  if (!reader.get_u64(message.server_time_ms) || !reader.get_u64(message.receive_time_us) ||
      !reader.get_u64(state.session_id) || !reader.get_f64(state.day_hours) ||
      !reader.get_f64(state.weather_phase) || !reader.get_f64(state.variation_phase) ||
      !reader.get_f32(state.day_duration_s) || !reader.get_f32(state.weather_duration_s) ||
      !reader.get_f32(state.weather_bias) || !reader.get_u8(state.weather_preset)) return CodecError::truncated;
  if (reader.remaining() != 0U) return CodecError::trailing_bytes;
  return ht2mp::protocol::valid_environment(state) ? CodecError::none : CodecError::invalid_value;
}

} // namespace ht2mp::ipc
