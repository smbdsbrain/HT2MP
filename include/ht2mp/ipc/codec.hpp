#pragma once

#include "ht2mp/ipc/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ht2mp::ipc {

enum class CodecError {
  none,
  buffer_too_small,
  invalid_magic,
  unsupported_version,
  invalid_message_type,
  payload_too_large,
  truncated,
  trailing_bytes,
  invalid_value,
};

class PayloadWriter final {
public:
  explicit PayloadWriter(std::span<std::byte> output) noexcept : output_(output) {}

  [[nodiscard]] bool put_u8(std::uint8_t value) noexcept;
  [[nodiscard]] bool put_u16(std::uint16_t value) noexcept;
  [[nodiscard]] bool put_u32(std::uint32_t value) noexcept;
  [[nodiscard]] bool put_u64(std::uint64_t value) noexcept;
  [[nodiscard]] bool put_i32(std::int32_t value) noexcept;
  [[nodiscard]] bool put_f32(float value) noexcept;
  [[nodiscard]] bool put_f64(double value) noexcept;
  [[nodiscard]] bool put_bytes(std::span<const std::byte> value) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return cursor_; }

private:
  std::span<std::byte> output_;
  std::size_t cursor_{};
};

class PayloadReader final {
public:
  explicit PayloadReader(std::span<const std::byte> input) noexcept : input_(input) {}

  [[nodiscard]] bool get_u8(std::uint8_t& value) noexcept;
  [[nodiscard]] bool get_u16(std::uint16_t& value) noexcept;
  [[nodiscard]] bool get_u32(std::uint32_t& value) noexcept;
  [[nodiscard]] bool get_u64(std::uint64_t& value) noexcept;
  [[nodiscard]] bool get_i32(std::int32_t& value) noexcept;
  [[nodiscard]] bool get_f32(float& value) noexcept;
  [[nodiscard]] bool get_f64(double& value) noexcept;
  [[nodiscard]] bool get_bytes(std::span<std::byte> value) noexcept;
  [[nodiscard]] std::size_t remaining() const noexcept { return input_.size() - cursor_; }

private:
  std::span<const std::byte> input_;
  std::size_t cursor_{};
};

[[nodiscard]] CodecError encode_frame(
    MessageType type,
    std::uint32_t sequence,
    std::span<const std::byte> payload,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;

[[nodiscard]] CodecError decode_frame(
    std::span<const std::byte> frame,
    FrameHeader& header,
    std::span<const std::byte>& payload) noexcept;

[[nodiscard]] CodecError encode_bridge_hello(
    const BridgeHelloV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError decode_bridge_hello(
    std::span<const std::byte> payload,
    BridgeHelloV1& message) noexcept;

[[nodiscard]] CodecError encode_bridge_ready(
    const BridgeReadyV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError decode_bridge_ready(
    std::span<const std::byte> payload,
    BridgeReadyV1& message) noexcept;

[[nodiscard]] CodecError encode_bridge_status(
    const BridgeStatusV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError decode_bridge_status(
    std::span<const std::byte> payload,
    BridgeStatusV1& message) noexcept;

[[nodiscard]] CodecError encode_set_observer_mode(
    const SetObserverModeV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError decode_set_observer_mode(
    std::span<const std::byte> payload,
    SetObserverModeV1& message) noexcept;

[[nodiscard]] CodecError encode_enter_safe_mode(
    const EnterSafeModeV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError decode_enter_safe_mode(
    std::span<const std::byte> payload,
    EnterSafeModeV1& message) noexcept;

[[nodiscard]] CodecError encode_local_sample(
    const PlayerSampleV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError encode_remote_sample(
    const PlayerSampleV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError decode_player_sample(
    std::span<const std::byte> payload,
    PlayerSampleV1& message) noexcept;

[[nodiscard]] CodecError encode_spawn_remote(
    const SpawnRemoteV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError decode_spawn_remote(
    std::span<const std::byte> payload,
    SpawnRemoteV1& message) noexcept;

[[nodiscard]] CodecError encode_despawn_remote(
    const DespawnRemoteV1& message,
    std::span<std::byte> output,
    std::size_t& encoded_size) noexcept;
[[nodiscard]] CodecError decode_despawn_remote(
    std::span<const std::byte> payload,
    DespawnRemoteV1& message) noexcept;

[[nodiscard]] CodecError encode_environment(const EnvironmentV1&, std::span<std::byte>, std::size_t&) noexcept;
[[nodiscard]] CodecError decode_environment(std::span<const std::byte>, EnvironmentV1&) noexcept;

} // namespace ht2mp::ipc
