#pragma once

#include "ht2mp/protocol/types.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ht2mp::protocol {

enum class EncodeError {
    none = 0,
    invalid_value,
    packet_too_large,
};

struct EncodeResult {
    EncodeError error{EncodeError::none};
    std::string detail;
    std::vector<std::byte> bytes;

    [[nodiscard]] explicit operator bool() const noexcept { return error == EncodeError::none; }
};

enum class DecodeError {
    none = 0,
    too_short,
    too_large,
    bad_magic,
    unsupported_version,
    unknown_message,
    invalid_header,
    truncated,
    trailing_data,
    invalid_value,
};

struct DecodeResult {
    DecodeError error{DecodeError::none};
    std::string detail;
    Message message{Disconnect{}};

    [[nodiscard]] explicit operator bool() const noexcept { return error == DecodeError::none; }
};

[[nodiscard]] EncodeResult encode_packet(const Message& message);
[[nodiscard]] DecodeResult decode_packet(std::span<const std::byte> packet);

}  // namespace ht2mp::protocol
