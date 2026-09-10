#pragma once

#include "ht2mp/protocol/pose_math.hpp"
#include "ht2mp/protocol/replication.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace ht2mp::bridge {

inline constexpr std::size_t kPoseTimelineCapacity = 16U;
// One-second clock/jitter buckets; the sliding minimum spans this window.
inline constexpr std::size_t kPoseClockBuckets = 24U;
// Receive-to-consume latency histogram, 25 ms per bucket, last is overflow.
inline constexpr std::size_t kPoseLatencyBuckets = 16U;
inline constexpr std::uint64_t kPoseLatencyBucketUs = 25'000U;

// One authoritative network sample as seen by the receiving bridge. All
// times are microseconds: send_time_us is on the SENDER's clock, receive_time_us
// on the local QPC clock.
struct PoseSample final {
  std::uint64_t send_time_us{};
  std::uint64_t receive_time_us{};
  std::uint32_t sequence{};
  std::int32_t room_id{};
  bool teleport{};
  posemath::Vec3d position{};
  posemath::Quat orientation{posemath::kIdentityQuat};
  posemath::Vec3f linear_velocity{};
  posemath::Vec3f angular_velocity{};
  ht2mp::protocol::WheelSpeeds wheels{};
};

enum class PosePlaybackState : std::uint8_t {
  none,
  interpolated,
  extrapolated,
  held,
};

struct PoseTimelineConfig final {
  std::uint64_t min_delay_us{80'000U};
  std::uint64_t max_delay_us{350'000U};
  std::uint64_t max_extrapolation_us{250'000U};
  // delay = interval_factor * typical worst-per-second sender interval
  //         + typical worst-per-second arrival jitter + jitter_margin,
  //         clamped to [min_delay, max_delay]. "Typical" is the median of the
  //         last five one-second maxima, so one bad second does not inflate
  //         the delay for everyone; extrapolation covers the outliers.
  double interval_factor{1.0};
  std::uint64_t jitter_margin_us{20'000U};
  // How fast the playback clock may run faster/slower than real time while it
  // converges on a changed offset/delay (fraction of elapsed time).
  double max_time_warp{0.05};
  // Catch-up rate once playback lags the ideal position by more than
  // fast_warp_threshold_us (after a hold the clock must recover its delay).
  double fast_time_warp{0.25};
  std::uint64_t fast_warp_threshold_us{150'000U};
  // Beyond this playback error the clock re-anchors instead of slewing.
  std::uint64_t resync_threshold_us{1'000'000U};
  // Error-offset smoothing when new data moves the curve under the actor.
  double correction_tau_us{80'000.0};
  double max_position_correction{2.0};   // metres, larger errors snap
  double max_rotation_correction{0.5236}; // radians (30 degrees)
};

struct PoseOutput final {
  PosePlaybackState state{PosePlaybackState::none};
  posemath::Vec3d position{};
  posemath::Quat orientation{posemath::kIdentityQuat};
  std::uint32_t sequence{};            // newest sample at or before the target
  std::int64_t target_send_time_us{};  // playback position on the sender clock
  ht2mp::protocol::WheelSpeeds wheels{};
};

struct PoseTimelineStats final {
  std::int64_t clock_offset_us{};   // estimated local - sender clock
  std::uint64_t delay_us{};         // delay currently targeted
  std::uint64_t interval_max_us{};  // recent max gap between sender samples
  std::uint64_t jitter_max_us{};    // recent max arrival jitter above the floor
  std::uint32_t pushed{};
  std::uint32_t rejected{};
  std::uint32_t interpolated{};
  std::uint32_t extrapolated{};
  std::uint32_t held{};
  std::uint32_t resyncs{};
  std::uint32_t snaps{};            // corrections too large to smooth
  std::uint32_t buffered{};
  std::uint64_t latency_max_us{};   // receive -> first consumed
  std::array<std::uint32_t, kPoseLatencyBuckets> latency_histogram{};
};

// Fixed-capacity, allocation-free playback timeline for one remote actor.
// Push() runs on the game thread when the FIFO is drained; Evaluate() runs
// once per game frame and returns the pose to display right now. Everything
// is plain data so the worker can snapshot stats() through a seqlock copy.
class RemotePoseTimeline final {
 public:
  void Reset() noexcept;
  // Drops buffered poses but keeps the clock estimate (teleport, room change).
  void ResetSamples() noexcept;
  void set_config(const PoseTimelineConfig& config) noexcept { config_ = config; }

  [[nodiscard]] bool Push(const PoseSample& sample) noexcept;
  [[nodiscard]] PoseOutput Evaluate(std::uint64_t now_us) noexcept;

  [[nodiscard]] const PoseTimelineStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] bool has_output() const noexcept { return have_output_; }
  [[nodiscard]] const PoseOutput& last_output() const noexcept { return last_output_; }

 private:
  struct ClockBucket final {
    std::uint64_t epoch_s{};
    std::int64_t min_offset_us{};
    std::uint64_t max_jitter_us{};
    std::uint64_t max_interval_us{};
    bool valid{};
  };

  [[nodiscard]] const PoseSample& at(std::size_t index) const noexcept {
    return ring_[(head_ + index) % kPoseTimelineCapacity];
  }
  [[nodiscard]] bool& consumed_at(std::size_t index) noexcept {
    return consumed_[(head_ + index) % kPoseTimelineCapacity];
  }
  struct RawPose final {
    PosePlaybackState state{PosePlaybackState::none};
    posemath::Vec3d position{};
    posemath::Quat orientation{posemath::kIdentityQuat};
    std::uint32_t sequence{};
    ht2mp::protocol::WheelSpeeds wheels{};
  };
  [[nodiscard]] RawPose evaluate_raw(std::int64_t target_send_time_us) const noexcept;
  [[nodiscard]] std::int64_t offset_estimate(std::uint64_t now_us) const noexcept;
  void recent_maxima(std::uint64_t now_us, std::uint64_t& interval_max_us,
                     std::uint64_t& jitter_max_us) const noexcept;
  void record_latency(std::uint64_t now_us, std::int64_t target_send_time_us) noexcept;

  PoseTimelineConfig config_{};
  std::array<PoseSample, kPoseTimelineCapacity> ring_{};
  std::array<bool, kPoseTimelineCapacity> consumed_{};
  std::size_t head_{};
  std::size_t count_{};
  std::uint64_t last_send_time_us_{};
  std::uint32_t last_sequence_{};
  bool has_last_{};
  std::int32_t last_room_id_{};

  std::array<ClockBucket, kPoseClockBuckets> buckets_{};
  bool have_offset_{};
  std::int64_t last_offset_estimate_us_{};

  bool have_playback_{};
  std::int64_t playback_send_time_us_{};
  std::uint64_t last_now_us_{};

  bool new_data_{};
  bool have_output_{};
  PoseOutput last_output_{};
  posemath::Vec3d correction_position_{};
  posemath::Quat correction_orientation_{posemath::kIdentityQuat};
  PoseTimelineStats stats_{};
};

} // namespace ht2mp::bridge
