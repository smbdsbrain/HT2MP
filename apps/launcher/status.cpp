#include "status.hpp"

#include <array>
#include <utility>

namespace ht2mp::launcher {

std::optional<StatusEvent> parse_status_event(const std::string_view line) {
  constexpr std::string_view prefix = "HT2MP-EVENT/1 ";
  if (!line.starts_with(prefix)) return std::nullopt;
  const auto state_at = line.find("state=", prefix.size());
  if (state_at == std::string_view::npos) return std::nullopt;
  const auto begin = state_at + 6U;
  const auto end = line.find(' ', begin);
  const auto name = line.substr(begin, end - begin);
  constexpr std::array names{
      std::pair{"game-started", RunPhase::game_started},
      std::pair{"bridge-ready", RunPhase::bridge_ready},
      std::pair{"appearance-ready", RunPhase::appearance_ready},
      std::pair{"connected", RunPhase::connected},
      std::pair{"reconnecting", RunPhase::reconnecting},
      std::pair{"rejected", RunPhase::rejected},
      std::pair{"appearance-mismatch", RunPhase::failed},
      std::pair{"disconnecting", RunPhase::shutting_down},
      std::pair{"stopped", RunPhase::stopped},
      std::pair{"error", RunPhase::failed},
  };
  for (const auto& [text, phase] : names) {
    if (name != text) continue;
    const auto detail_at = line.find("detail=", end);
    return StatusEvent{
        phase, detail_at == std::string_view::npos
                   ? std::string{}
                   : std::string(line.substr(detail_at + 7U))};
  }
  return std::nullopt;
}

bool RunState::begin() noexcept {
  if (active_) return false;
  active_ = true;
  phase_ = RunPhase::preparing;
  return true;
}

void RunState::apply(const StatusEvent& event) noexcept {
  if (active_) phase_ = event.phase;
}

bool RunState::request_shutdown() noexcept {
  if (!active_) return false;
  phase_ = RunPhase::shutting_down;
  return true;
}

void RunState::finish(const std::uint32_t exit_code) noexcept {
  if (!active_) return;
  active_ = false;
  phase_ = exit_code == 0U ? RunPhase::stopped : RunPhase::failed;
}

}  // namespace ht2mp::launcher
