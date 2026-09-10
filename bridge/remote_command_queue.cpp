#include "remote_command_queue.hpp"

#include <algorithm>
#include <cmath>

namespace ht2mp::bridge {
namespace {

class ExclusiveLock final {
 public:
  explicit ExclusiveLock(SRWLOCK& lock) noexcept : lock_(&lock) {
    AcquireSRWLockExclusive(lock_);
  }
  ~ExclusiveLock() { ReleaseSRWLockExclusive(lock_); }
  ExclusiveLock(const ExclusiveLock&) = delete;
  ExclusiveLock& operator=(const ExclusiveLock&) = delete;

 private:
  SRWLOCK* lock_{};
};

bool ValidIdentity(const std::uint64_t player_id,
                   const std::uint64_t incarnation_id) noexcept {
  return player_id != 0U && incarnation_id != 0U;
}

bool SequenceNewer(const std::uint32_t candidate,
                   const std::uint32_t baseline) noexcept {
  const auto delta = candidate - baseline;
  return delta != 0U && delta < 0x80000000U;
}

bool ValidSpawn(const ht2mp::ipc::SpawnRemoteV1& command) noexcept {
  return ValidIdentity(command.player_id, command.incarnation_id) &&
         command.paint_variant <= 3U &&
         std::find(command.display_name.begin(), command.display_name.end(),
                   '\0') != command.display_name.end();
}

bool ValidState(const ht2mp::ipc::PlayerSampleV1& command) noexcept {
  if (!ValidIdentity(command.player_id, command.incarnation_id) ||
      (command.flags & ~0x07U) != 0U ||
      command.paint_variant > 3U ||
      !ht2mp::protocol::valid_vehicle_state(command.vehicle)) {
    return false;
  }
  const auto finite = [](const auto& values) {
    return std::all_of(values.begin(), values.end(),
                       [](const auto value) { return std::isfinite(value); });
  };
  if (!finite(command.position) || !finite(command.orientation) ||
      !finite(command.linear_velocity) || !finite(command.angular_velocity) ||
      !std::isfinite(command.location.road_distance)) {
    return false;
  }
  double norm_squared{};
  for (const auto component : command.orientation) {
    norm_squared += static_cast<double>(component) * component;
  }
  return std::abs(norm_squared - 1.0) <= 0.05;
}

} // namespace

RemoteQueueResult RemoteCommandQueue::PushSpawn(
    const ht2mp::ipc::SpawnRemoteV1& command) noexcept {
  if (!ValidSpawn(command)) return RemoteQueueResult::invalid;
  const ExclusiveLock guard(lock_);
  if (control_count_ == controls_.size()) return RemoteQueueResult::overflow;
  RemoveStatesForPlayer(command.player_id, command.incarnation_id);
  auto& slot = controls_[control_count_++];
  slot = {};
  slot.kind = RemoteControlKind::spawn;
  slot.spawn = command;
  return RemoteQueueResult::queued;
}

RemoteCommandQueue::StateFifo* RemoteCommandQueue::FindFifo(
    const std::uint64_t player_id,
    const std::uint64_t incarnation_id) noexcept {
  for (auto& fifo : fifos_) {
    if (fifo.player_id == player_id && fifo.incarnation_id == incarnation_id) {
      return &fifo;
    }
  }
  return nullptr;
}

RemoteQueueResult RemoteCommandQueue::PushState(
    const ht2mp::ipc::PlayerSampleV1& command) noexcept {
  if (!ValidState(command)) return RemoteQueueResult::invalid;
  const ExclusiveLock guard(lock_);
  auto* fifo = FindFifo(command.player_id, command.incarnation_id);
  if (fifo == nullptr) {
    for (auto& candidate : fifos_) {
      if (candidate.player_id == 0U) {
        fifo = &candidate;
        break;
      }
    }
    if (fifo == nullptr) return RemoteQueueResult::overflow;
    *fifo = {};
    fifo->player_id = command.player_id;
    fifo->incarnation_id = command.incarnation_id;
  }
  if (fifo->has_sequence &&
      !SequenceNewer(command.sequence, fifo->last_sequence)) {
    return RemoteQueueResult::stale;
  }
  auto result = RemoteQueueResult::queued;
  if (fifo->count == fifo->ring.size()) {
    fifo->head = (fifo->head + 1U) % fifo->ring.size();
    --fifo->count;
    ++dropped_states_;
    result = RemoteQueueResult::dropped_oldest;
  }
  fifo->ring[(fifo->head + fifo->count) % fifo->ring.size()] = command;
  ++fifo->count;
  fifo->last_sequence = command.sequence;
  fifo->has_sequence = true;
  return result;
}

RemoteQueueResult RemoteCommandQueue::PushDespawn(
    const ht2mp::ipc::DespawnRemoteV1& command) noexcept {
  if (!ValidIdentity(command.player_id, command.incarnation_id)) {
    return RemoteQueueResult::invalid;
  }
  const ExclusiveLock guard(lock_);
  if (control_count_ == controls_.size()) return RemoteQueueResult::overflow;
  RemoveStatesForPlayer(command.player_id, 0U);
  auto& slot = controls_[control_count_++];
  slot = {};
  slot.kind = RemoteControlKind::despawn;
  slot.despawn = command;
  return RemoteQueueResult::queued;
}

bool RemoteCommandQueue::TryDrain(RemoteCommandBatch& output) noexcept {
  if (!TryAcquireSRWLockExclusive(&lock_)) return false;
  output.control_count = control_count_;
  std::copy_n(controls_.begin(), control_count_, output.controls.begin());
  std::size_t emitted{};
  for (auto& fifo : fifos_) {
    for (std::size_t index = 0U; index < fifo.count; ++index) {
      output.states[emitted++] = fifo.ring[(fifo.head + index) % fifo.ring.size()];
    }
    fifo.head = 0U;
    fifo.count = 0U;
  }
  output.state_count = emitted;
  output.dropped_states = dropped_states_;
  control_count_ = 0U;
  ReleaseSRWLockExclusive(&lock_);
  return true;
}

void RemoteCommandQueue::Clear() noexcept {
  const ExclusiveLock guard(lock_);
  control_count_ = 0U;
  for (auto& fifo : fifos_) fifo = {};
  dropped_states_ = 0U;
}

void RemoteCommandQueue::RemoveStatesForPlayer(
    const std::uint64_t player_id,
    const std::uint64_t except_incarnation) noexcept {
  for (auto& fifo : fifos_) {
    const bool remove = fifo.player_id == player_id &&
                        (except_incarnation == 0U ||
                         fifo.incarnation_id != except_incarnation);
    if (remove) fifo = {};
  }
}

} // namespace ht2mp::bridge
