#pragma once

#include "ht2mp/ipc/codec.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ht2mp::client {

struct ReceivedFrame final {
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> storage{};
  std::size_t size{};
  ht2mp::ipc::FrameHeader header{};

  [[nodiscard]] std::span<const std::byte> payload() const noexcept {
    return std::span(storage).subspan(ht2mp::ipc::kFrameHeaderSize,
                                      header.payload_size);
  }
};

class PipeServer final {
public:
  PipeServer() noexcept = default;
  ~PipeServer();
  PipeServer(const PipeServer&) = delete;
  PipeServer& operator=(const PipeServer&) = delete;
  PipeServer(PipeServer&& other) noexcept;
  PipeServer& operator=(PipeServer&& other) noexcept;

  [[nodiscard]] bool create(std::wstring_view name, std::string& error);
  [[nodiscard]] bool connect(std::string& error);
  [[nodiscard]] bool read(ReceivedFrame& frame, std::string& error);
  [[nodiscard]] bool write(std::span<const std::byte> frame, std::string& error);
  [[nodiscard]] bool available(std::uint32_t& bytes, std::string& error) const;
  void close() noexcept;

private:
  HANDLE handle_{INVALID_HANDLE_VALUE};
};

} // namespace ht2mp::client
