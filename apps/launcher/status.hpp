#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ht2mp::launcher {

enum class RunPhase : std::uint8_t {
  idle,
  preparing,
  game_started,
  bridge_ready,
  appearance_ready,
  connected,
  reconnecting,
  rejected,
  shutting_down,
  stopped,
  failed,
};

struct StatusEvent final {
  RunPhase phase{RunPhase::idle};
  std::string detail;
};

[[nodiscard]] std::optional<StatusEvent> parse_status_event(
    std::string_view line);

class RunState final {
 public:
  [[nodiscard]] bool begin() noexcept;
  void apply(const StatusEvent& event) noexcept;
  [[nodiscard]] bool request_shutdown() noexcept;
  void finish(std::uint32_t exit_code) noexcept;

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] RunPhase phase() const noexcept { return phase_; }

 private:
  bool active_{};
  RunPhase phase_{RunPhase::idle};
};

}  // namespace ht2mp::launcher
