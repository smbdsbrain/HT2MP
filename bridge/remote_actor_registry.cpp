#include "remote_actor_registry.hpp"

#include <algorithm>
#include <limits>

namespace ht2mp::bridge {
namespace {

bool IsPlausiblePointer(const std::uint32_t value) noexcept {
  return value >= 0x00010000U && value < 0x80000000U &&
         (value & 0x3U) == 0U;
}

bool IsValid(const RemoteActorBinding& value) noexcept {
  return value.remote.player_id != 0U && value.remote.incarnation_id != 0U &&
         value.generation == 0U && IsPlausiblePointer(value.game_player_id) &&
         IsPlausiblePointer(value.vehicle_instance) &&
         IsPlausiblePointer(value.moving_item) &&
         IsPlausiblePointer(value.physics);
}

}  // namespace

RemoteActorRegistryResult RemoteActorRegistry::Insert(
    RemoteActorBinding candidate, RemoteActorBinding& inserted) noexcept {
  inserted = {};
  if (!IsValid(candidate)) return RemoteActorRegistryResult::invalid;

  for (std::size_t index = 0U; index < size_; ++index) {
    const auto& existing = bindings_[index];
    if (existing.remote.player_id == candidate.remote.player_id) {
      return RemoteActorRegistryResult::remote_player_conflict;
    }
    if (existing.game_player_id == candidate.game_player_id ||
        existing.vehicle_instance == candidate.vehicle_instance ||
        existing.moving_item == candidate.moving_item ||
        existing.physics == candidate.physics) {
      return RemoteActorRegistryResult::game_identity_conflict;
    }
  }
  if (size_ == bindings_.size()) return RemoteActorRegistryResult::full;

  candidate.generation = NextGeneration();
  bindings_[size_++] = candidate;
  inserted = candidate;
  return RemoteActorRegistryResult::inserted;
}

RemoteActorRegistryResult RemoteActorRegistry::Erase(
    const RemoteActorKey& key, const std::uint32_t generation) noexcept {
  if (key.player_id == 0U || key.incarnation_id == 0U || generation == 0U) {
    return RemoteActorRegistryResult::invalid;
  }
  for (std::size_t index = 0U; index < size_; ++index) {
    if (bindings_[index].remote.player_id != key.player_id) continue;
    if (bindings_[index].remote.incarnation_id != key.incarnation_id) {
      return RemoteActorRegistryResult::stale_incarnation;
    }
    if (bindings_[index].generation != generation) {
      return RemoteActorRegistryResult::stale_generation;
    }
    for (std::size_t next = index + 1U; next < size_; ++next) {
      bindings_[next - 1U] = bindings_[next];
    }
    bindings_[--size_] = {};
    return RemoteActorRegistryResult::removed;
  }
  return RemoteActorRegistryResult::not_found;
}

const RemoteActorBinding* RemoteActorRegistry::Find(
    const RemoteActorKey& key) const noexcept {
  const auto found = std::find_if(
      bindings_.begin(), bindings_.begin() + static_cast<std::ptrdiff_t>(size_),
      [&key](const auto& value) { return value.remote == key; });
  return found == bindings_.begin() + static_cast<std::ptrdiff_t>(size_)
             ? nullptr
             : &*found;
}

const RemoteActorBinding* RemoteActorRegistry::FindPlayer(
    const std::uint64_t player_id) const noexcept {
  const auto found = std::find_if(
      bindings_.begin(), bindings_.begin() + static_cast<std::ptrdiff_t>(size_),
      [player_id](const auto& value) {
        return value.remote.player_id == player_id;
      });
  return found == bindings_.begin() + static_cast<std::ptrdiff_t>(size_)
             ? nullptr
             : &*found;
}

std::span<const RemoteActorBinding> RemoteActorRegistry::bindings()
    const noexcept {
  return std::span<const RemoteActorBinding>(bindings_).first(size_);
}

void RemoteActorRegistry::Clear() noexcept {
  std::fill(bindings_.begin(), bindings_.end(), RemoteActorBinding{});
  size_ = 0U;
}

std::uint32_t RemoteActorRegistry::NextGeneration() noexcept {
  auto result = next_generation_++;
  if (result == 0U) result = next_generation_++;
  if (next_generation_ == 0U) next_generation_ = 1U;
  return result;
}

void RemoteCollisionIndex::Publish(
    const std::span<const RemoteActorBinding> bindings) noexcept {
  sequence_.fetch_add(1U, std::memory_order_acq_rel);
  const auto count = std::min(bindings.size(), moving_items_.size());
  for (std::size_t index = 0U; index < moving_items_.size(); ++index) {
    const auto value = index < count ? bindings[index].moving_item : 0U;
    moving_items_[index].store(value, std::memory_order_relaxed);
  }
  sequence_.fetch_add(1U, std::memory_order_release);
}

void RemoteCollisionIndex::Clear() noexcept {
  Publish(std::span<const RemoteActorBinding>{});
}

bool RemoteCollisionIndex::Contains(const std::uint32_t moving_item) const
    noexcept {
  return PairTouchesRemoteStable(moving_item, 0U);
}

bool RemoteCollisionIndex::PairTouchesRemote(
    const std::uint32_t current, const std::uint32_t other) const noexcept {
  return PairTouchesRemoteStable(current, other);
}

bool RemoteCollisionIndex::PairTouchesRemoteStable(
    const std::uint32_t current, const std::uint32_t other) const noexcept {
  if (current == 0U && other == 0U) return false;
  for (std::uint32_t attempt = 0U; attempt < 3U; ++attempt) {
    const auto before = sequence_.load(std::memory_order_acquire);
    if ((before & 1U) != 0U) continue;
    bool matched{};
    for (const auto& slot : moving_items_) {
      const auto value = slot.load(std::memory_order_relaxed);
      matched = matched || (value != 0U &&
                            (value == current || value == other));
    }
    const auto after = sequence_.load(std::memory_order_acquire);
    if (before == after && (after & 1U) == 0U) return matched;
  }
  return false;
}

}  // namespace ht2mp::bridge
