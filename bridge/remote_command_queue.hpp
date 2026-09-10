#pragma once

#include "ht2mp/ipc/protocol.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ht2mp::bridge {

inline constexpr std::size_t kMaximumRemotePlayers = 7U;
inline constexpr std::size_t kMaximumPendingControlCommands = 32U;
// Authoritative network states are kept per player in arrival order so the
// game-thread pose timeline sees every sample, not only the newest one. Eight
// entries cover 400 ms of a 20 Hz stream; a slower consumer drops the oldest
// entries and counts the loss instead of failing closed.
inline constexpr std::size_t kStateFifoDepth = 8U;
inline constexpr std::size_t kMaximumPendingStates =
    kMaximumRemotePlayers * kStateFifoDepth;

enum class RemoteQueueResult : std::uint8_t {
  queued,
  dropped_oldest,
  stale,
  invalid,
  overflow,
};

enum class RemoteControlKind : std::uint8_t {
  spawn,
  despawn,
};

struct RemoteControlCommand final {
  RemoteControlKind kind{RemoteControlKind::spawn};
  ht2mp::ipc::SpawnRemoteV1 spawn{};
  ht2mp::ipc::DespawnRemoteV1 despawn{};
};

struct RemoteCommandBatch final {
  std::array<RemoteControlCommand, kMaximumPendingControlCommands> controls{};
  std::size_t control_count{};
  // States are ordered by player FIFO and, inside one player, by arrival.
  std::array<ht2mp::ipc::PlayerSampleV1, kMaximumPendingStates> states{};
  std::size_t state_count{};
  // Cumulative number of states discarded by the drop-oldest policy.
  std::uint32_t dropped_states{};
};

// One pipe worker produces commands and the hooked game thread consumes them.
// Control commands are never overwritten. State commands form a bounded FIFO
// per (player_id, incarnation_id); a full FIFO discards its oldest entry. The
// consumer uses TryAcquireSRWLockExclusive so a game tick never waits on the
// worker.
class RemoteCommandQueue final {
 public:
  RemoteCommandQueue() noexcept = default;
  RemoteCommandQueue(const RemoteCommandQueue&) = delete;
  RemoteCommandQueue& operator=(const RemoteCommandQueue&) = delete;

  [[nodiscard]] RemoteQueueResult PushSpawn(
      const ht2mp::ipc::SpawnRemoteV1& command) noexcept;
  [[nodiscard]] RemoteQueueResult PushState(
      const ht2mp::ipc::PlayerSampleV1& command) noexcept;
  [[nodiscard]] RemoteQueueResult PushDespawn(
      const ht2mp::ipc::DespawnRemoteV1& command) noexcept;

  // Returns false only when the worker momentarily owns the lock. In that
  // case output is left unchanged and the game thread should retry next tick.
  [[nodiscard]] bool TryDrain(RemoteCommandBatch& output) noexcept;
  void Clear() noexcept;

 private:
  struct StateFifo final {
    std::uint64_t player_id{};
    std::uint64_t incarnation_id{};
    std::array<ht2mp::ipc::PlayerSampleV1, kStateFifoDepth> ring{};
    std::size_t head{};
    std::size_t count{};
    std::uint32_t last_sequence{};
    bool has_sequence{};
  };

  [[nodiscard]] StateFifo* FindFifo(std::uint64_t player_id,
                                    std::uint64_t incarnation_id) noexcept;
  void RemoveStatesForPlayer(std::uint64_t player_id,
                             std::uint64_t except_incarnation) noexcept;

  SRWLOCK lock_ = SRWLOCK_INIT;
  std::array<RemoteControlCommand, kMaximumPendingControlCommands> controls_{};
  std::size_t control_count_{};
  std::array<StateFifo, kMaximumRemotePlayers> fifos_{};
  std::uint32_t dropped_states_{};
};

} // namespace ht2mp::bridge
