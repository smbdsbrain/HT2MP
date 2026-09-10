#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ht2mp::ipc {

inline constexpr std::uint32_t kBootstrapMagic = 0x504D3248U; // "H2MP"
inline constexpr std::uint32_t kBootstrapAbiVersion = 2U;
inline constexpr std::size_t kProfileIdCapacity = 32U;
inline constexpr std::size_t kPipeNameCapacity = 192U;
inline constexpr std::size_t kNonceSize = 16U;

enum class BootstrapFlags : std::uint32_t {
  none = 0U,
  observer_only = 1U << 0U,
  // Allows installation of exact-profile telemetry.  The historical field
  // name is retained to keep BootstrapV1 binary-compatible.
  experimental_gog_telemetry = 1U << 1U,
  // Adds five ABI-preserving, count-only actor-boundary detours. This flag is
  // valid only together with experimental_gog_telemetry on the exact GOG
  // profile and never authorizes remote actor writes.
  experimental_gog_actor_diagnostics = 1U << 2U,
  // Uses the exact build's native menu dispatchers to select Single Player
  // and Load on the game thread.  The sidecar permits it only when the staged
  // runtime contains exactly one .pl1 driver, so selection is deterministic.
  auto_enter_world = 1U << 3U,
  // Enables the exact-profile dedicated-actor experiment.  The historical
  // field name is retained for BootstrapV1 compatibility.  It authorizes
  // bounded game-thread writes only after every active symbol has been
  // resolved from the exact executable and its live bytes checked again.
  experimental_gog_remote_actors = 1U << 4U,
  // Steam exact-build online-world hooks. Both bits are emitted together by
  // --online-mode; separate bits let the sidecar fail closed if either half
  // could not be initialized transactionally.
  stock_npc_suppression = 1U << 5U,
  background_tick = 1U << 6U,
  // The sidecar supplied an allowlisted model selector and one of the four
  // stock paint variants. The bridge applies both before acquirePlayerId.
  appearance_override = 1U << 7U,
};

enum class BootstrapResult : std::uint32_t {
  ok = 0U,
  invalid_argument = 1U,
  unsupported_abi = 2U,
  already_initialized = 3U,
  wrong_process = 4U,
  pipe_unavailable = 5U,
  handshake_failed = 6U,
  worker_start_failed = 7U,
  internal_error = 8U,
};

// This structure crosses a process boundary. It deliberately contains no
// pointers, handles, bools, or implementation-defined containers.
#pragma pack(push, 1)
struct BootstrapV1 final {
  std::uint32_t magic{kBootstrapMagic};
  std::uint32_t struct_size{sizeof(BootstrapV1)};
  std::uint32_t abi_version{kBootstrapAbiVersion};
  std::uint32_t flags{static_cast<std::uint32_t>(BootstrapFlags::observer_only)};
  std::uint64_t run_id{};
  std::array<std::uint8_t, kNonceSize> nonce{};
  std::array<char, kProfileIdCapacity> profile_id{};
  std::array<wchar_t, kPipeNameCapacity> pipe_name{};
  std::uint32_t vehicle_selector{0xffffffffU};
  std::uint32_t paint_variant{};
  std::array<std::uint32_t, 6> reserved{};
};
#pragma pack(pop)

static_assert(std::is_standard_layout_v<BootstrapV1>);
static_assert(std::is_trivially_copyable_v<BootstrapV1>);
static_assert(sizeof(wchar_t) == 2U, "BootstrapV1 is a Windows-only ABI");
static_assert(sizeof(BootstrapV1) == 488U);

using BootstrapEntrypoint = std::uint32_t(__stdcall*)(const BootstrapV1*);

} // namespace ht2mp::ipc
