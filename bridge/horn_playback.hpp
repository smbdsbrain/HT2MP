#pragma once

#include "ht2mp/protocol/replication.hpp"
#include <cstdint>

namespace ht2mp::bridge {

// Sound uses the latest authoritative button state. A counter preserves a
// short tap that starts and ends between 20 Hz network samples. Repeated
// snapshots never restart the pulse, and old presses do not play on join.
class HornPlayback {
 public:
  static constexpr std::uint64_t kFreshnessUs = 500'000;
  static constexpr std::uint64_t kMinimumPulseUs = 120'000;

  void Observe(const ht2mp::protocol::HornState& state,
               std::uint64_t received_us, std::uint64_t now_us) noexcept {
    if (state.tone > 2U || received_us == 0 ||
        (initialized_ && received_us <= received_us_)) return;
    const bool fresh = now_us >= received_us && now_us - received_us <= kFreshnessUs;
    const bool new_press = initialized_ && state.press_sequence != state_.press_sequence;
    if (fresh && (new_press || (!initialized_ && state.active)))
      pulse_until_us_ = now_us + kMinimumPulseUs;
    if (!fresh) pulse_until_us_ = 0;
    initialized_ = true;
    received_us_ = received_us;
    state_ = state;
  }

  [[nodiscard]] bool Playing(std::uint64_t now_us) const noexcept {
    return initialized_ && now_us >= received_us_ &&
           now_us - received_us_ <= kFreshnessUs &&
           (state_.active || now_us < pulse_until_us_);
  }
  [[nodiscard]] std::uint8_t tone() const noexcept { return state_.tone; }
  void Reset() noexcept { *this = {}; }

 private:
  ht2mp::protocol::HornState state_{};
  std::uint64_t received_us_{}, pulse_until_us_{};
  bool initialized_{};
};
} // namespace ht2mp::bridge
