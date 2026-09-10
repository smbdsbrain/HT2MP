#pragma once

#include "ht2mp/protocol/types.hpp"
#include "ht2mp/protocol/validation.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace ht2mp::protocol {

enum class PlaybackState {
    missing,
    interpolated,
    extrapolated,
    frozen,
    hidden,
    despawn,
};

struct TimelineOptions {
    std::uint64_t interpolation_delay_ms{100};
    std::uint64_t max_extrapolation_ms{250};
    std::uint64_t freeze_after_ms{500};
    std::uint64_t hide_after_ms{3000};
    std::uint64_t despawn_after_ms{5000};
    std::size_t max_samples{64};
};

struct PlaybackSample {
    PlaybackState state{PlaybackState::missing};
    std::optional<PlayerState> value;
};

class RemoteTimeline {
public:
    explicit RemoteTimeline(TimelineOptions options = {}, ProfileLimits limits = {});

    [[nodiscard]] ValidationResult push(PlayerState sample);
    [[nodiscard]] PlaybackSample sample(std::uint64_t server_time_ms) const;
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }
    [[nodiscard]] std::uint64_t player_id() const noexcept { return player_id_; }
    [[nodiscard]] std::uint64_t incarnation_id() const noexcept { return incarnation_id_; }

private:
    TimelineOptions options_;
    ProfileLimits limits_;
    std::deque<PlayerState> samples_;
    std::uint64_t player_id_{};
    std::uint64_t incarnation_id_{};
};

[[nodiscard]] bool sequence_newer(std::uint32_t candidate, std::uint32_t baseline) noexcept;

}  // namespace ht2mp::protocol
