#pragma once

#include "remote_command_queue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ht2mp::bridge {

struct RemoteActorKey final {
  std::uint64_t player_id{};
  std::uint64_t incarnation_id{};

  friend bool operator==(const RemoteActorKey&, const RemoteActorKey&) = default;
};

// Game-owned addresses are retained only on the game-thread side of the
// bridge. They are never serialized to IPC or the network. Generation is
// assigned by this registry so a recycled PlayerId address cannot satisfy an
// operation queued for an older actor lifetime.
struct RemoteActorBinding final {
  RemoteActorKey remote{};
  std::uint32_t game_player_id{};
  std::uint32_t generation{};
  std::uint32_t vehicle_instance{};
  std::uint32_t moving_item{};
  std::uint32_t physics{};
};

enum class RemoteActorRegistryResult : std::uint8_t {
  inserted,
  removed,
  not_found,
  stale_incarnation,
  stale_generation,
  remote_player_conflict,
  game_identity_conflict,
  invalid,
  full,
};

// Fixed-capacity, game-thread-only ownership table. This class intentionally
// performs no allocation, locking, or game call. The future actor backend must
// validate all pointers and pool membership before inserting and before each
// Publish call.
class RemoteActorRegistry final {
 public:
  [[nodiscard]] RemoteActorRegistryResult Insert(
      RemoteActorBinding candidate, RemoteActorBinding& inserted) noexcept;
  [[nodiscard]] RemoteActorRegistryResult Erase(
      const RemoteActorKey& key, std::uint32_t generation) noexcept;

  [[nodiscard]] const RemoteActorBinding* Find(
      const RemoteActorKey& key) const noexcept;
  [[nodiscard]] const RemoteActorBinding* FindPlayer(
      std::uint64_t player_id) const noexcept;
  [[nodiscard]] std::span<const RemoteActorBinding> bindings() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
  void Clear() noexcept;

 private:
  [[nodiscard]] std::uint32_t NextGeneration() noexcept;

  std::array<RemoteActorBinding, kMaximumRemotePlayers> bindings_{};
  std::size_t size_{};
  std::uint32_t next_generation_{1U};
};

// Collision hooks must not walk the actor registry or take a lock. The game
// thread publishes only its already revalidated MovingItem identities here.
// Every word is atomic, so the bounded sequence check has no C++ data race.
// A reader that races repeated publication returns false and lets the original
// game collision path run; write-enabled operation must purge actors before
// clearing this index during teardown.
class RemoteCollisionIndex final {
 public:
  RemoteCollisionIndex() noexcept = default;
  RemoteCollisionIndex(const RemoteCollisionIndex&) = delete;
  RemoteCollisionIndex& operator=(const RemoteCollisionIndex&) = delete;

  void Publish(std::span<const RemoteActorBinding> bindings) noexcept;
  void Clear() noexcept;

  [[nodiscard]] bool Contains(std::uint32_t moving_item) const noexcept;
  [[nodiscard]] bool PairTouchesRemote(std::uint32_t current,
                                       std::uint32_t other) const noexcept;

 private:
  [[nodiscard]] bool PairTouchesRemoteStable(std::uint32_t current,
                                             std::uint32_t other) const noexcept;

  std::atomic<std::uint32_t> sequence_{};
  std::array<std::atomic<std::uint32_t>, kMaximumRemotePlayers> moving_items_{};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

}  // namespace ht2mp::bridge
