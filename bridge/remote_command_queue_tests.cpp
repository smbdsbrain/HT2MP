#include "remote_command_queue.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures{};

void Check(const bool condition, const char* expression, const int line) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
  }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

ht2mp::ipc::SpawnRemoteV1 Spawn(const std::uint64_t player,
                                const std::uint64_t incarnation) {
  ht2mp::ipc::SpawnRemoteV1 value;
  value.player_id = player;
  value.incarnation_id = incarnation;
  value.display_name[0] = 'P';
  value.display_name[1] = '\0';
  return value;
}

ht2mp::ipc::PlayerSampleV1 State(const std::uint64_t player,
                                 const std::uint64_t incarnation,
                                 const std::uint32_t sequence) {
  ht2mp::ipc::PlayerSampleV1 value;
  value.player_id = player;
  value.incarnation_id = incarnation;
  value.sequence = sequence;
  value.flags = 1U;
  value.orientation[3] = 1.0F;
  return value;
}

void ControlFifoAndStateFifo() {
  ht2mp::bridge::RemoteCommandQueue queue;
  CHECK(queue.PushSpawn(Spawn(1U, 10U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  CHECK(queue.PushSpawn(Spawn(2U, 20U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  CHECK(queue.PushState(State(1U, 10U, 1U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  CHECK(queue.PushState(State(1U, 10U, 2U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  CHECK(queue.PushState(State(1U, 10U, 1U)) ==
        ht2mp::bridge::RemoteQueueResult::stale);
  CHECK(queue.PushState(State(2U, 20U, 7U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);

  ht2mp::bridge::RemoteCommandBatch batch;
  CHECK(queue.TryDrain(batch));
  CHECK(batch.control_count == 2U);
  CHECK(batch.controls[0].kind == ht2mp::bridge::RemoteControlKind::spawn);
  CHECK(batch.controls[0].spawn.player_id == 1U);
  CHECK(batch.controls[1].spawn.player_id == 2U);
  // Every state survives, in arrival order, grouped by player.
  CHECK(batch.state_count == 3U);
  CHECK(batch.states[0].player_id == 1U && batch.states[0].sequence == 1U);
  CHECK(batch.states[1].player_id == 1U && batch.states[1].sequence == 2U);
  CHECK(batch.states[2].player_id == 2U && batch.states[2].sequence == 7U);
  CHECK(batch.dropped_states == 0U);

  // Stale detection persists across drains.
  CHECK(queue.PushState(State(1U, 10U, 2U)) ==
        ht2mp::bridge::RemoteQueueResult::stale);
  CHECK(queue.PushState(State(1U, 10U, 3U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  ht2mp::bridge::RemoteCommandBatch second;
  CHECK(queue.TryDrain(second));
  CHECK(second.control_count == 0U && second.state_count == 1U);
  CHECK(second.states[0].sequence == 3U);
}

void StateOverflowDropsOldestNotControls() {
  ht2mp::bridge::RemoteCommandQueue queue;
  for (std::uint32_t sequence = 1U;
       sequence <= ht2mp::bridge::kStateFifoDepth; ++sequence) {
    CHECK(queue.PushState(State(1U, 10U, sequence)) ==
          ht2mp::bridge::RemoteQueueResult::queued);
  }
  CHECK(queue.PushState(State(1U, 10U, 100U)) ==
        ht2mp::bridge::RemoteQueueResult::dropped_oldest);
  CHECK(queue.PushState(State(1U, 10U, 101U)) ==
        ht2mp::bridge::RemoteQueueResult::dropped_oldest);
  ht2mp::bridge::RemoteCommandBatch batch;
  CHECK(queue.TryDrain(batch));
  CHECK(batch.state_count == ht2mp::bridge::kStateFifoDepth);
  CHECK(batch.states[0].sequence == 3U);
  CHECK(batch.states[ht2mp::bridge::kStateFifoDepth - 1U].sequence == 101U);
  CHECK(batch.dropped_states == 2U);
}

void SequenceWrapAndDespawnPurge() {
  ht2mp::bridge::RemoteCommandQueue queue;
  CHECK(queue.PushState(State(1U, 10U, 0xfffffffEU)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  CHECK(queue.PushState(State(1U, 10U, 1U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  CHECK(queue.PushDespawn({1U, 10U}) ==
        ht2mp::bridge::RemoteQueueResult::queued);

  ht2mp::bridge::RemoteCommandBatch batch;
  CHECK(queue.TryDrain(batch));
  CHECK(batch.control_count == 1U);
  CHECK(batch.controls[0].kind == ht2mp::bridge::RemoteControlKind::despawn);
  CHECK(batch.controls[0].despawn.player_id == 1U);
  CHECK(batch.state_count == 0U);

  // After a despawn the FIFO forgets the old sequence horizon.
  CHECK(queue.PushState(State(1U, 10U, 1U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
}

void CapacitiesFailClosedWithoutDroppingControls() {
  ht2mp::bridge::RemoteCommandQueue queue;
  for (std::size_t index = 0U;
       index < ht2mp::bridge::kMaximumPendingControlCommands; ++index) {
    CHECK(queue.PushSpawn(Spawn(static_cast<std::uint64_t>(index + 1U), 1U)) ==
          ht2mp::bridge::RemoteQueueResult::queued);
  }
  CHECK(queue.PushSpawn(Spawn(100U, 1U)) ==
        ht2mp::bridge::RemoteQueueResult::overflow);

  ht2mp::bridge::RemoteCommandBatch batch;
  CHECK(queue.TryDrain(batch));
  CHECK(batch.control_count == ht2mp::bridge::kMaximumPendingControlCommands);
  for (std::size_t index = 0U; index < batch.control_count; ++index) {
    CHECK(batch.controls[index].spawn.player_id == index + 1U);
  }

  for (std::size_t index = 0U; index < ht2mp::bridge::kMaximumRemotePlayers;
       ++index) {
    CHECK(queue.PushState(State(static_cast<std::uint64_t>(index + 1U), 1U,
                                1U)) ==
          ht2mp::bridge::RemoteQueueResult::queued);
  }
  CHECK(queue.PushState(State(100U, 1U, 1U)) ==
        ht2mp::bridge::RemoteQueueResult::overflow);
}

void RejectsMalformedCommandsAndOldIncarnations() {
  ht2mp::bridge::RemoteCommandQueue queue;
  CHECK(queue.PushSpawn(Spawn(0U, 1U)) ==
        ht2mp::bridge::RemoteQueueResult::invalid);
  auto bad_name = Spawn(1U, 1U);
  bad_name.display_name.fill('x');
  CHECK(queue.PushSpawn(bad_name) ==
        ht2mp::bridge::RemoteQueueResult::invalid);
  auto non_finite = State(1U, 1U, 1U);
  non_finite.position[0] = std::numeric_limits<double>::infinity();
  CHECK(queue.PushState(non_finite) ==
        ht2mp::bridge::RemoteQueueResult::invalid);
  auto bad_quaternion = State(1U, 1U, 1U);
  bad_quaternion.orientation = {0.0F, 0.0F, 0.0F, 2.0F};
  CHECK(queue.PushState(bad_quaternion) ==
        ht2mp::bridge::RemoteQueueResult::invalid);

  CHECK(queue.PushState(State(1U, 10U, 1U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  CHECK(queue.PushSpawn(Spawn(1U, 11U)) ==
        ht2mp::bridge::RemoteQueueResult::queued);
  ht2mp::bridge::RemoteCommandBatch batch;
  CHECK(queue.TryDrain(batch));
  CHECK(batch.state_count == 0U);
}

} // namespace

int main() {
  ControlFifoAndStateFifo();
  StateOverflowDropsOldestNotControls();
  SequenceWrapAndDespawnPurge();
  CapacitiesFailClosedWithoutDroppingControls();
  RejectsMalformedCommandsAndOldIncarnations();
  if (failures != 0) {
    std::cerr << failures << " remote command queue test(s) failed\n";
    return 1;
  }
  std::cout << "remote command queue tests passed\n";
  return 0;
}
