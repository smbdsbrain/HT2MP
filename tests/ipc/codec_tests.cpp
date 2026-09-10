#include "ht2mp/ipc/bootstrap.hpp"
#include "ht2mp/ipc/authentication.hpp"
#include "ht2mp/ipc/codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>

namespace {

int failures{};

void check(const bool condition, const char* expression, const int line) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
  }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void scalar_codec_is_little_endian() {
  std::array<std::byte, 32> bytes{};
  ht2mp::ipc::PayloadWriter writer(bytes);
  CHECK(writer.put_u16(0x1234U));
  CHECK(writer.put_u32(0x89ABCDEFU));
  CHECK(writer.put_u64(0x0123456789ABCDEFULL));
  CHECK(writer.size() == 14U);
  CHECK(bytes[0] == std::byte{0x34U});
  CHECK(bytes[1] == std::byte{0x12U});
  CHECK(bytes[2] == std::byte{0xEFU});
  CHECK(bytes[13] == std::byte{0x01U});

  ht2mp::ipc::PayloadReader reader(std::span(bytes).first(writer.size()));
  std::uint16_t u16{};
  std::uint32_t u32{};
  std::uint64_t u64{};
  CHECK(reader.get_u16(u16));
  CHECK(reader.get_u32(u32));
  CHECK(reader.get_u64(u64));
  CHECK(reader.remaining() == 0U);
  CHECK(u16 == 0x1234U);
  CHECK(u32 == 0x89ABCDEFU);
  CHECK(u64 == 0x0123456789ABCDEFULL);
}

void hello_round_trip() {
  ht2mp::ipc::BridgeHelloV1 source;
  source.run_id = 0xFEDCBA9876543210ULL;
  for (std::size_t i = 0; i < source.nonce.size(); ++i) {
    source.nonce[i] = static_cast<std::uint8_t>(i * 7U);
  }
  constexpr char kProfile[] = "gog-05588140";
  std::copy(std::begin(kProfile), std::end(kProfile), source.profile_id.begin());

  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> encoded{};
  std::size_t size{};
  CHECK(ht2mp::ipc::encode_bridge_hello(source, encoded, size) ==
        ht2mp::ipc::CodecError::none);
  CHECK(size == ht2mp::ipc::kFrameHeaderSize + 60U);

  ht2mp::ipc::FrameHeader header;
  std::span<const std::byte> payload;
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
        ht2mp::ipc::CodecError::none);
  CHECK(header.type == ht2mp::ipc::MessageType::bridge_hello);

  ht2mp::ipc::BridgeHelloV1 decoded;
  CHECK(ht2mp::ipc::decode_bridge_hello(payload, decoded) ==
        ht2mp::ipc::CodecError::none);
  CHECK(decoded.abi_version == source.abi_version);
  CHECK(decoded.run_id == source.run_id);
  CHECK(decoded.nonce == source.nonce);
  CHECK(decoded.profile_id == source.profile_id);
}

void bridge_control_messages_round_trip() {
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> encoded{};
  ht2mp::ipc::FrameHeader header;
  std::span<const std::byte> payload;
  std::size_t size{};

  const ht2mp::ipc::BridgeReadyV1 ready_source{
      ht2mp::ipc::BootstrapResult::pipe_unavailable,
      ht2mp::ipc::BridgeMode::safe,
      static_cast<std::uint32_t>(ht2mp::ipc::Capability::observer_mode) |
          static_cast<std::uint32_t>(ht2mp::ipc::Capability::local_telemetry)};
  CHECK(ht2mp::ipc::encode_bridge_ready(ready_source, encoded, size) ==
        ht2mp::ipc::CodecError::none);
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
        ht2mp::ipc::CodecError::none);
  CHECK(header.type == ht2mp::ipc::MessageType::bridge_ready);
  ht2mp::ipc::BridgeReadyV1 ready_decoded;
  CHECK(ht2mp::ipc::decode_bridge_ready(payload, ready_decoded) ==
        ht2mp::ipc::CodecError::none);
  CHECK(ready_decoded.result == ready_source.result);
  CHECK(ready_decoded.mode == ready_source.mode);
  CHECK(ready_decoded.capabilities == ready_source.capabilities);

  ht2mp::ipc::BridgeStatusV1 status_source;
  status_source.code = ht2mp::ipc::BridgeStatusCode::queue_overflow;
  status_source.mode = ht2mp::ipc::BridgeMode::safe;
  constexpr char kDetail[] = "control queue overflow; entered safe mode";
  std::copy(std::begin(kDetail), std::end(kDetail), status_source.detail.begin());
  CHECK(ht2mp::ipc::encode_bridge_status(status_source, encoded, size) ==
        ht2mp::ipc::CodecError::none);
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
        ht2mp::ipc::CodecError::none);
  CHECK(header.type == ht2mp::ipc::MessageType::bridge_status);
  ht2mp::ipc::BridgeStatusV1 status_decoded;
  CHECK(ht2mp::ipc::decode_bridge_status(payload, status_decoded) ==
        ht2mp::ipc::CodecError::none);
  CHECK(status_decoded.code == status_source.code);
  CHECK(status_decoded.mode == status_source.mode);
  CHECK(status_decoded.detail == status_source.detail);

  for (const bool enabled : {false, true}) {
    const ht2mp::ipc::SetObserverModeV1 observer_source{enabled};
    CHECK(ht2mp::ipc::encode_set_observer_mode(observer_source, encoded, size) ==
          ht2mp::ipc::CodecError::none);
    CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
          ht2mp::ipc::CodecError::none);
    CHECK(header.type == ht2mp::ipc::MessageType::set_observer_mode);
    ht2mp::ipc::SetObserverModeV1 observer_decoded;
    CHECK(ht2mp::ipc::decode_set_observer_mode(payload, observer_decoded) ==
          ht2mp::ipc::CodecError::none);
    CHECK(observer_decoded.enabled == enabled);
  }

  const ht2mp::ipc::EnterSafeModeV1 safe_source{0xA5A55A5AU};
  CHECK(ht2mp::ipc::encode_enter_safe_mode(safe_source, encoded, size) ==
        ht2mp::ipc::CodecError::none);
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
        ht2mp::ipc::CodecError::none);
  CHECK(header.type == ht2mp::ipc::MessageType::enter_safe_mode);
  ht2mp::ipc::EnterSafeModeV1 safe_decoded;
  CHECK(ht2mp::ipc::decode_enter_safe_mode(payload, safe_decoded) ==
        ht2mp::ipc::CodecError::none);
  CHECK(safe_decoded.reason == safe_source.reason);
}

void remote_lifecycle_round_trip() {
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> encoded{};
  ht2mp::ipc::FrameHeader header;
  std::span<const std::byte> payload;
  std::size_t size{};

  ht2mp::ipc::SpawnRemoteV1 spawn_source;
  spawn_source.player_id = 0x0102030405060708ULL;
  spawn_source.incarnation_id = 0x8877665544332211ULL;
  spawn_source.vehicle_type = 7U;
  spawn_source.paint_variant = 3U;
  constexpr char kName[] = "Synthetic peer";
  std::copy(std::begin(kName), std::end(kName), spawn_source.display_name.begin());
  CHECK(ht2mp::ipc::encode_spawn_remote(spawn_source, encoded, size) ==
        ht2mp::ipc::CodecError::none);
  CHECK(size == ht2mp::ipc::kFrameHeaderSize + 52U);
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
        ht2mp::ipc::CodecError::none);
  CHECK(header.type == ht2mp::ipc::MessageType::spawn_remote);
  ht2mp::ipc::SpawnRemoteV1 spawn_decoded;
  CHECK(ht2mp::ipc::decode_spawn_remote(payload, spawn_decoded) ==
        ht2mp::ipc::CodecError::none);
  CHECK(spawn_decoded.player_id == spawn_source.player_id);
  CHECK(spawn_decoded.incarnation_id == spawn_source.incarnation_id);
  CHECK(spawn_decoded.vehicle_type == spawn_source.vehicle_type);
  CHECK(spawn_decoded.paint_variant == spawn_source.paint_variant);
  CHECK(spawn_decoded.display_name == spawn_source.display_name);

  const ht2mp::ipc::DespawnRemoteV1 despawn_source{
      spawn_source.player_id, spawn_source.incarnation_id};
  CHECK(ht2mp::ipc::encode_despawn_remote(despawn_source, encoded, size) ==
        ht2mp::ipc::CodecError::none);
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
        ht2mp::ipc::CodecError::none);
  CHECK(header.type == ht2mp::ipc::MessageType::despawn_remote);
  ht2mp::ipc::DespawnRemoteV1 despawn_decoded;
  CHECK(ht2mp::ipc::decode_despawn_remote(payload, despawn_decoded) ==
        ht2mp::ipc::CodecError::none);
  CHECK(despawn_decoded.player_id == despawn_source.player_id);
  CHECK(despawn_decoded.incarnation_id == despawn_source.incarnation_id);
}

void malformed_frames_are_rejected() {
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> encoded{};
  std::size_t size{};
  const ht2mp::ipc::EnterSafeModeV1 source{42U};
  CHECK(ht2mp::ipc::encode_enter_safe_mode(source, encoded, size) ==
        ht2mp::ipc::CodecError::none);

  ht2mp::ipc::FrameHeader header;
  std::span<const std::byte> payload;
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size - 1U), header, payload) ==
        ht2mp::ipc::CodecError::truncated);
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size + 1U), header, payload) ==
        ht2mp::ipc::CodecError::trailing_bytes);

  encoded[0] ^= std::byte{0xFFU};
  CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
        ht2mp::ipc::CodecError::invalid_magic);

  const std::array invalid_boolean{std::byte{2U}};
  ht2mp::ipc::SetObserverModeV1 observer;
  CHECK(ht2mp::ipc::decode_set_observer_mode(invalid_boolean, observer) ==
        ht2mp::ipc::CodecError::invalid_value);
}

void payload_limit_is_enforced() {
  std::array<std::byte, ht2mp::ipc::kMaximumPayloadSize + 1U> oversized{};
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> output{};
  std::size_t size{123U};
  CHECK(ht2mp::ipc::encode_frame(ht2mp::ipc::MessageType::bridge_status, 0U,
                                 oversized, output, size) ==
        ht2mp::ipc::CodecError::payload_too_large);
  CHECK(size == 0U);
}

void player_sample_round_trip() {
  ht2mp::ipc::PlayerSampleV1 source;
  source.player_id = 17U;
  source.incarnation_id = 23U;
  source.sample_time_ms = 99'123U;
  source.receive_time_us = 0x0123456789ABCDEFULL;
  source.sequence = 0xFFFFFFFEU;
  source.flags = 5U;
  source.position = {1.25, -2.5, 123456.75};
  source.orientation = {0.0F, 0.5F, 0.0F, 0.8660254F};
  source.linear_velocity = {1.0F, 2.0F, 3.0F};
  source.angular_velocity = {-0.1F, 0.2F, -0.3F};
  source.vehicle_type = 42U;
  source.paint_variant = 2U;
  source.vehicle.valid = true;
  source.vehicle.steering = -64;
  source.vehicle.lights = {1, 0, 1, 2};
  source.vehicle.condition[3] = 0.75F;
  source.vehicle.wheels = {8, {-128.0F, 0.0F, 20.0F, 31.0F, -80.0F, 48.0F, 9.0F, 12.0F}};
  source.vehicle.horn = {true, 2, 65535};
  source.location = {3, 4, 5, 6.75, 7, 8, 9, 10};

  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> encoded{};
  std::size_t size{};
  for (const auto type : {ht2mp::ipc::MessageType::local_sample,
                          ht2mp::ipc::MessageType::remote_sample}) {
    const auto encode = type == ht2mp::ipc::MessageType::local_sample
                            ? ht2mp::ipc::encode_local_sample
                            : ht2mp::ipc::encode_remote_sample;
    CHECK(encode(source, encoded, size) == ht2mp::ipc::CodecError::none);
    CHECK(size == ht2mp::ipc::kFrameHeaderSize + 378U);
    ht2mp::ipc::FrameHeader header;
    std::span<const std::byte> payload;
    CHECK(ht2mp::ipc::decode_frame(std::span(encoded).first(size), header, payload) ==
          ht2mp::ipc::CodecError::none);
    CHECK(header.type == type);
    ht2mp::ipc::PlayerSampleV1 decoded;
    CHECK(ht2mp::ipc::decode_player_sample(payload, decoded) ==
          ht2mp::ipc::CodecError::none);
    CHECK(decoded.player_id == source.player_id);
    CHECK(decoded.incarnation_id == source.incarnation_id);
    CHECK(decoded.sample_time_ms == source.sample_time_ms);
    CHECK(decoded.receive_time_us == source.receive_time_us);
    CHECK(decoded.sequence == source.sequence);
    CHECK(decoded.flags == source.flags);
    CHECK(decoded.position == source.position);
    CHECK(decoded.orientation == source.orientation);
    CHECK(decoded.linear_velocity == source.linear_velocity);
    CHECK(decoded.angular_velocity == source.angular_velocity);
    CHECK(decoded.vehicle_type == source.vehicle_type);
    CHECK(decoded.paint_variant == source.paint_variant);
    CHECK(decoded.vehicle == source.vehicle);
    CHECK(decoded.location.room_id == source.location.room_id);
    CHECK(decoded.location.road_distance == source.location.road_distance);
  }
}

void bridge_hello_authentication_rejects_confusion() {
  ht2mp::ipc::BridgeHelloV1 hello;
  hello.run_id = 77U;
  for (std::size_t index = 0; index < hello.nonce.size(); ++index) {
    hello.nonce[index] = static_cast<std::uint8_t>(index + 1U);
  }
  constexpr char kProfile[] = "gog-05588140";
  std::copy(std::begin(kProfile), std::end(kProfile), hello.profile_id.begin());
  const ht2mp::ipc::BridgeExpectation expected{
      hello.run_id, hello.nonce, "gog-05588140"};
  CHECK(ht2mp::ipc::authenticate_bridge_hello(hello, expected) ==
        ht2mp::ipc::AuthenticationError::none);

  auto wrong_nonce = expected;
  wrong_nonce.nonce[4] ^= 0x80U;
  CHECK(ht2mp::ipc::authenticate_bridge_hello(hello, wrong_nonce) ==
        ht2mp::ipc::AuthenticationError::nonce_mismatch);
  auto wrong_run = expected;
  ++wrong_run.run_id;
  CHECK(ht2mp::ipc::authenticate_bridge_hello(hello, wrong_run) ==
        ht2mp::ipc::AuthenticationError::run_id_mismatch);
  auto wrong_profile = expected;
  wrong_profile.profile_id = "steam-8138acee";
  CHECK(ht2mp::ipc::authenticate_bridge_hello(hello, wrong_profile) ==
        ht2mp::ipc::AuthenticationError::profile_mismatch);
  hello.profile_id.fill('x');
  CHECK(ht2mp::ipc::authenticate_bridge_hello(hello, expected) ==
        ht2mp::ipc::AuthenticationError::malformed_profile);
}

} // namespace

int main() {
  scalar_codec_is_little_endian();
  hello_round_trip();
  bridge_control_messages_round_trip();
  remote_lifecycle_round_trip();
  malformed_frames_are_rejected();
  payload_limit_is_enforced();
  player_sample_round_trip();
  bridge_hello_authentication_rejects_confusion();
  if (failures != 0) {
    std::cerr << failures << " IPC codec test(s) failed\n";
    return 1;
  }
  std::cout << "IPC codec tests passed\n";
  return 0;
}
