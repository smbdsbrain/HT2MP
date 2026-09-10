#pragma once

#include "ht2mp/game/profile.hpp"
#include "ht2mp/ipc/protocol.hpp"
#include <cstdint>
#include <string>

namespace ht2mp::bridge {
bool InitializeSteamReplication(const ht2mp::game::ProfileVerification&,
                                std::uintptr_t module_base, std::string& error);
void CaptureVehicleState(std::uint32_t vehicle, ht2mp::protocol::VehicleState&) noexcept;
bool QueueEnvironment(const ht2mp::ipc::EnvironmentV1&) noexcept;
void TickReplication(bool local_world_ready) noexcept;
void DisableReplication() noexcept;
struct ReplicationStats {
  std::uint32_t captured{}, applied{}, environment_ticks{}, failures{};
  std::int32_t steering{};
  std::uint32_t headlights{};
  float deformation{}, day_hours{}, weather_phase{};
  std::uint32_t wheel_steps{};
  float local_wheel_speed{}, remote_wheel_speed{}, remote_wheel_phase{};
};
ReplicationStats GetReplicationStats() noexcept;
} // namespace ht2mp::bridge
