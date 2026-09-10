#pragma once

#include "ht2mp/protocol/replication.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ht2mp::bridge {

// Per-actor cosmetic phases. Wheel speed shares the body's delayed sender
// clock; phases themselves need no network agreement. No native AI state is
// used after initialization, so physics cannot reset the animation each frame.
class WheelAnimation final {
 public:
  void Reset() noexcept { *this = {}; }
  void Advance(const ht2mp::protocol::WheelSpeeds& speeds,
               std::int64_t sender_time_us, bool held) noexcept {
    if (!ht2mp::protocol::valid_wheel_speeds(speeds)) return;
    if (!initialized_ || count_ != speeds.count) {
      phases_.fill(0.0F);
      initialized_ = true;
      count_ = speeds.count;
      last_time_us_ = sender_time_us;
      previous_ = speeds.radians_per_second;
      return;
    }
    const auto elapsed = sender_time_us > last_time_us_ ? sender_time_us - last_time_us_ : 0;
    if (!held && elapsed > 0) {
      // Never replay a long occlusion/reconnect gap as a burst of rotation.
      const auto seconds = static_cast<double>(std::min<std::int64_t>(elapsed, 250'000)) / 1'000'000.0;
      for (std::size_t i = 0; i < count_; ++i) {
        // A received zero means stopped, including locked wheels while sliding.
        const auto speed = speeds.radians_per_second[i] == 0.0F ? 0.0 :
            (static_cast<double>(previous_[i]) + speeds.radians_per_second[i]) * 0.5;
        phases_[i] = static_cast<float>(std::remainder(phases_[i] + speed * seconds, 6.283185307179586));
      }
    }
    last_time_us_ = sender_time_us;
    previous_ = speeds.radians_per_second;
  }
  [[nodiscard]] const std::array<float, ht2mp::protocol::kMaxVehicleWheels>& phases() const noexcept { return phases_; }

 private:
  std::array<float, ht2mp::protocol::kMaxVehicleWheels> phases_{}, previous_{};
  std::int64_t last_time_us_{};
  std::uint8_t count_{};
  bool initialized_{};
};

// Native row-major Rx(spin) * Rz(steer), excluding suspension translation.
inline std::array<float, 9> WheelRotation(float spin, float steering_yaw) noexcept {
  const auto cy = std::cos(steering_yaw), sy = std::sin(steering_yaw);
  const auto cs = std::cos(spin), ss = std::sin(spin);
  return {cy, sy, 0.0F, -cs * sy, cs * cy, ss, ss * sy, -ss * cy, cs};
}
} // namespace ht2mp::bridge
