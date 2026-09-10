#include "pipe_server.hpp"

#include "ht2mp/windows/runtime.hpp"

#include <sddl.h>

#include <algorithm>
#include <array>

namespace ht2mp::client {
namespace {

bool read_exact(HANDLE pipe, const std::span<std::byte> destination, std::string& error) {
  std::size_t cursor{};
  while (cursor < destination.size()) {
    DWORD read{};
    const auto remaining = destination.size() - cursor;
    if (!ReadFile(pipe, destination.data() + cursor,
                  static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD)),
                  &read, nullptr)) {
      error = "Named-pipe read failed: " +
              ht2mp::windows::win32_error(GetLastError());
      return false;
    }
    if (read == 0U) {
      error = "Named pipe closed during a frame";
      return false;
    }
    cursor += read;
  }
  return true;
}

} // namespace

PipeServer::~PipeServer() {
  close();
}

PipeServer::PipeServer(PipeServer&& other) noexcept : handle_(other.handle_) {
  other.handle_ = INVALID_HANDLE_VALUE;
}

PipeServer& PipeServer::operator=(PipeServer&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = INVALID_HANDLE_VALUE;
  }
  return *this;
}

bool PipeServer::create(const std::wstring_view name, std::string& error) {
  close();
  error.clear();
  if (!name.starts_with(L"\\\\.\\pipe\\HT2MP-") ||
      name.size() >= ht2mp::ipc::kPipeNameCapacity) {
    error = "Invalid HT2MP named-pipe name";
    return false;
  }

  // OWNER RIGHTS means the object creator (the current user), while SYSTEM is
  // retained for process-management tooling. Remote clients are rejected by
  // the pipe mode below.
  PSECURITY_DESCRIPTOR descriptor{};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr)) {
    error = "Cannot build named-pipe ACL: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;

  const std::wstring owned_name(name);
  handle_ = CreateNamedPipeW(
      owned_name.c_str(),
      PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1U, static_cast<DWORD>(ht2mp::ipc::kMaximumFrameSize * 4U),
      static_cast<DWORD>(ht2mp::ipc::kMaximumFrameSize * 4U), 10'000U,
      &attributes);
  const auto create_error = GetLastError();
  LocalFree(descriptor);
  if (handle_ == INVALID_HANDLE_VALUE) {
    error = "Cannot create named pipe: " +
            ht2mp::windows::win32_error(create_error);
    return false;
  }
  return true;
}

bool PipeServer::connect(std::string& error) {
  error.clear();
  if (handle_ == INVALID_HANDLE_VALUE) {
    error = "Named pipe has not been created";
    return false;
  }
  if (ConnectNamedPipe(handle_, nullptr)) {
    return true;
  }
  const auto code = GetLastError();
  if (code == ERROR_PIPE_CONNECTED) {
    return true;
  }
  error = "Cannot accept bridge connection: " +
          ht2mp::windows::win32_error(code);
  return false;
}

bool PipeServer::read(ReceivedFrame& frame, std::string& error) {
  frame = {};
  error.clear();
  if (handle_ == INVALID_HANDLE_VALUE) {
    error = "Named pipe is closed";
    return false;
  }
  auto header_bytes = std::span(frame.storage).first(ht2mp::ipc::kFrameHeaderSize);
  if (!read_exact(handle_, header_bytes, error)) {
    return false;
  }

  ht2mp::ipc::PayloadReader reader(header_bytes);
  std::uint32_t magic{};
  std::uint16_t version{};
  std::uint16_t type{};
  if (!reader.get_u32(magic) || !reader.get_u16(version) || !reader.get_u16(type) ||
      !reader.get_u32(frame.header.payload_size) ||
      !reader.get_u32(frame.header.sequence)) {
    error = "Truncated IPC frame header";
    return false;
  }
  if (magic != ht2mp::ipc::kFrameMagic || version != ht2mp::ipc::kProtocolVersion ||
      type < static_cast<std::uint16_t>(ht2mp::ipc::MessageType::bridge_hello) ||
      type > static_cast<std::uint16_t>(ht2mp::ipc::MessageType::enter_safe_mode) ||
      frame.header.payload_size > ht2mp::ipc::kMaximumPayloadSize) {
    error = "Invalid IPC frame header";
    return false;
  }
  frame.header.type = static_cast<ht2mp::ipc::MessageType>(type);
  auto payload_bytes = std::span(frame.storage).subspan(
      ht2mp::ipc::kFrameHeaderSize, frame.header.payload_size);
  if (!read_exact(handle_, payload_bytes, error)) {
    return false;
  }
  frame.size = ht2mp::ipc::kFrameHeaderSize + frame.header.payload_size;

  ht2mp::ipc::FrameHeader validated{};
  std::span<const std::byte> payload{};
  if (ht2mp::ipc::decode_frame(std::span(frame.storage).first(frame.size), validated,
                               payload) != ht2mp::ipc::CodecError::none) {
    error = "IPC frame failed codec validation";
    return false;
  }
  return true;
}

bool PipeServer::write(const std::span<const std::byte> frame, std::string& error) {
  error.clear();
  if (handle_ == INVALID_HANDLE_VALUE) {
    error = "Named pipe is closed";
    return false;
  }
  std::size_t cursor{};
  while (cursor < frame.size()) {
    DWORD written{};
    const auto remaining = frame.size() - cursor;
    if (!WriteFile(handle_, frame.data() + cursor,
                   static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD)),
                   &written, nullptr)) {
      error = "Named-pipe write failed: " +
              ht2mp::windows::win32_error(GetLastError());
      return false;
    }
    if (written == 0U) {
      error = "Named pipe accepted zero bytes";
      return false;
    }
    cursor += written;
  }
  return true;
}

bool PipeServer::available(std::uint32_t& bytes, std::string& error) const {
  bytes = 0U;
  error.clear();
  if (handle_ == INVALID_HANDLE_VALUE) {
    error = "Named pipe is closed";
    return false;
  }
  DWORD available_bytes{};
  if (!PeekNamedPipe(handle_, nullptr, 0U, nullptr, &available_bytes, nullptr)) {
    error = "Cannot inspect named pipe: " +
            ht2mp::windows::win32_error(GetLastError());
    return false;
  }
  bytes = available_bytes;
  return true;
}

void PipeServer::close() noexcept {
  if (handle_ != INVALID_HANDLE_VALUE) {
    DisconnectNamedPipe(handle_);
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
}

} // namespace ht2mp::client
