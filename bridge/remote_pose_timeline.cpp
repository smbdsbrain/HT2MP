#include "remote_pose_timeline.hpp"

#include <algorithm>
#include <cmath>

namespace ht2mp::bridge {
namespace {

using posemath::Quat;
using posemath::Vec3d;

bool sequence_newer(const std::uint32_t candidate,
                    const std::uint32_t baseline) noexcept {
  const auto delta = candidate - baseline;
  return delta != 0U && delta < 0x80000000U;
}

bool finite_sample(const PoseSample& sample) noexcept {
  if (!ht2mp::protocol::valid_wheel_speeds(sample.wheels)) return false;
  for (const auto value : sample.position) {
    if (!std::isfinite(value)) return false;
  }
  for (const auto value : sample.orientation) {
    if (!std::isfinite(value)) return false;
  }
  for (const auto value : sample.linear_velocity) {
    if (!std::isfinite(value)) return false;
  }
  for (const auto value : sample.angular_velocity) {
    if (!std::isfinite(value)) return false;
  }
  return true;
}

void extrapolate_pose(const PoseSample& sample, const double seconds,
                      Vec3d& position, Quat& orientation) noexcept {
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    position[axis] = sample.position[axis] +
                     static_cast<double>(sample.linear_velocity[axis]) * seconds;
  }
  orientation =
      posemath::quat_extrapolate(sample.orientation, sample.angular_velocity, seconds);
}

bool zero_velocity(const posemath::Vec3f& velocity) noexcept {
  return velocity[0] == 0.0F && velocity[1] == 0.0F && velocity[2] == 0.0F;
}

double position_distance(const Vec3d& a, const Vec3d& b) noexcept {
  const double dx = a[0] - b[0];
  const double dy = a[1] - b[1];
  const double dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

void RemotePoseTimeline::Reset() noexcept {
  const auto config = config_;
  *this = RemotePoseTimeline{};
  config_ = config;
}

void RemotePoseTimeline::ResetSamples() noexcept {
  head_ = 0U;
  count_ = 0U;
  consumed_.fill(false);
  has_last_ = false;
  new_data_ = false;
  correction_position_ = {};
  correction_orientation_ = posemath::kIdentityQuat;
  // Keep the clock buckets and the playback clock: they describe the sender,
  // not the trajectory. The next Evaluate() snaps to the fresh samples.
  have_output_ = false;
}

bool RemotePoseTimeline::Push(const PoseSample& sample) noexcept {
  if (!finite_sample(sample) || sample.receive_time_us == 0U) {
    ++stats_.rejected;
    return false;
  }
  if (has_last_) {
    if (!sequence_newer(sample.sequence, last_sequence_) ||
        sample.send_time_us <= last_send_time_us_) {
      ++stats_.rejected;
      return false;
    }
  }

  // Sender-clock offset and cadence bookkeeping (one-second buckets).
  const auto offset_us = static_cast<std::int64_t>(sample.receive_time_us) -
                         static_cast<std::int64_t>(sample.send_time_us);
  const auto epoch_s = sample.receive_time_us / 1'000'000U;
  auto& bucket = buckets_[static_cast<std::size_t>(epoch_s % kPoseClockBuckets)];
  if (!bucket.valid || bucket.epoch_s != epoch_s) {
    bucket = {};
    bucket.epoch_s = epoch_s;
    bucket.min_offset_us = offset_us;
    bucket.valid = true;
  } else {
    bucket.min_offset_us = std::min(bucket.min_offset_us, offset_us);
  }
  const auto floor_us = offset_estimate(sample.receive_time_us);
  if (offset_us > floor_us) {
    bucket.max_jitter_us = std::max(
        bucket.max_jitter_us, static_cast<std::uint64_t>(offset_us - floor_us));
  }
  if (has_last_) {
    bucket.max_interval_us =
        std::max(bucket.max_interval_us, sample.send_time_us - last_send_time_us_);
  }
  have_offset_ = true;

  const bool discontinuity =
      sample.teleport || (has_last_ && sample.room_id != last_room_id_);
  if (discontinuity) {
    ResetSamples();
    ++stats_.snaps;
  }

  if (count_ == kPoseTimelineCapacity) {
    head_ = (head_ + 1U) % kPoseTimelineCapacity;
    --count_;
  }
  const auto slot = (head_ + count_) % kPoseTimelineCapacity;
  ring_[slot] = sample;
  consumed_[slot] = false;
  ++count_;
  last_send_time_us_ = sample.send_time_us;
  last_sequence_ = sample.sequence;
  last_room_id_ = sample.room_id;
  has_last_ = true;
  new_data_ = true;
  ++stats_.pushed;
  stats_.buffered = static_cast<std::uint32_t>(count_);
  return true;
}

std::int64_t RemotePoseTimeline::offset_estimate(
    const std::uint64_t now_us) const noexcept {
  const auto now_s = now_us / 1'000'000U;
  bool found = false;
  std::int64_t minimum{};
  for (const auto& bucket : buckets_) {
    if (!bucket.valid || bucket.epoch_s + kPoseClockBuckets <= now_s) continue;
    if (!found || bucket.min_offset_us < minimum) {
      minimum = bucket.min_offset_us;
      found = true;
    }
  }
  if (found) return minimum;
  if (count_ != 0U) {
    const auto& newest = at(count_ - 1U);
    return static_cast<std::int64_t>(newest.receive_time_us) -
           static_cast<std::int64_t>(newest.send_time_us);
  }
  return last_offset_estimate_us_;
}

void RemotePoseTimeline::recent_maxima(
    const std::uint64_t now_us, std::uint64_t& interval_max_us,
    std::uint64_t& jitter_max_us) const noexcept {
  interval_max_us = 0U;
  jitter_max_us = 0U;
  const auto now_s = now_us / 1'000'000U;
  constexpr std::uint64_t kRecentSeconds = 5U;
  std::array<std::uint64_t, kRecentSeconds> intervals{};
  std::array<std::uint64_t, kRecentSeconds> jitters{};
  std::size_t count{};
  for (const auto& bucket : buckets_) {
    if (!bucket.valid || bucket.epoch_s + kRecentSeconds <= now_s) continue;
    if (count == kRecentSeconds) break;
    intervals[count] = bucket.max_interval_us;
    jitters[count] = bucket.max_jitter_us;
    ++count;
  }
  if (count == 0U) return;
  std::sort(intervals.begin(), intervals.begin() + static_cast<std::ptrdiff_t>(count));
  std::sort(jitters.begin(), jitters.begin() + static_cast<std::ptrdiff_t>(count));
  interval_max_us = intervals[count / 2U];
  jitter_max_us = jitters[count / 2U];
}

void RemotePoseTimeline::record_latency(
    const std::uint64_t now_us, const std::int64_t target_send_time_us) noexcept {
  for (std::size_t index = 0U; index < count_; ++index) {
    const auto& sample = at(index);
    if (static_cast<std::int64_t>(sample.send_time_us) > target_send_time_us) break;
    auto& consumed = consumed_at(index);
    if (consumed) continue;
    consumed = true;
    const auto latency_us =
        now_us > sample.receive_time_us ? now_us - sample.receive_time_us : 0U;
    stats_.latency_max_us = std::max(stats_.latency_max_us, latency_us);
    const auto bucket = static_cast<std::size_t>(std::min<std::uint64_t>(
        kPoseLatencyBuckets - 1U, latency_us / kPoseLatencyBucketUs));
    ++stats_.latency_histogram[bucket];
  }
}

RemotePoseTimeline::RawPose RemotePoseTimeline::evaluate_raw(
    const std::int64_t target) const noexcept {
  RawPose raw;
  const auto& oldest = at(0U);
  const auto& newest = at(count_ - 1U);
  raw.sequence = oldest.sequence;
  raw.wheels = oldest.wheels;
  if (target <= static_cast<std::int64_t>(oldest.send_time_us)) {
    raw.position = oldest.position;
    raw.orientation = oldest.orientation;
    raw.state = PosePlaybackState::interpolated;
    return raw;
  }
  if (target >= static_cast<std::int64_t>(newest.send_time_us)) {
    const auto beyond = static_cast<std::uint64_t>(
        target - static_cast<std::int64_t>(newest.send_time_us));
    raw.sequence = newest.sequence;
    raw.wheels = newest.wheels;
    if (beyond <= config_.max_extrapolation_us) {
      extrapolate_pose(newest, static_cast<double>(beyond) / 1'000'000.0,
                       raw.position, raw.orientation);
      raw.state = PosePlaybackState::extrapolated;
    } else {
      extrapolate_pose(newest,
                       static_cast<double>(config_.max_extrapolation_us) / 1'000'000.0,
                       raw.position, raw.orientation);
      raw.state = PosePlaybackState::held;
    }
    return raw;
  }
  for (std::size_t index = 1U; index < count_; ++index) {
    const auto& first = at(index - 1U);
    const auto& second = at(index);
    if (target > static_cast<std::int64_t>(second.send_time_us)) continue;
    raw.sequence = first.sequence;
    raw.wheels = first.wheels;
    if (first.wheels.count == second.wheels.count) {
      const auto span = second.send_time_us - first.send_time_us;
      const auto alpha = span == 0 ? 1.0 : std::clamp(
          static_cast<double>(target - static_cast<std::int64_t>(first.send_time_us)) /
              static_cast<double>(span), 0.0, 1.0);
      for (std::size_t i = 0; i < raw.wheels.count; ++i)
        raw.wheels.radians_per_second[i] = static_cast<float>(
            first.wheels.radians_per_second[i] * (1.0 - alpha) +
            second.wheels.radians_per_second[i] * alpha);
    }
    raw.state = PosePlaybackState::interpolated;
    // A stationary sender must produce a bit-identical pose every frame, so
    // identical endpoints bypass the interpolants entirely.
    if (first.position == second.position && zero_velocity(first.linear_velocity) &&
        zero_velocity(second.linear_velocity)) {
      raw.position = first.position;
    } else {
      const auto duration_us = second.send_time_us - first.send_time_us;
      const double alpha =
          duration_us == 0U
              ? 1.0
              : static_cast<double>(target - static_cast<std::int64_t>(first.send_time_us)) /
                    static_cast<double>(duration_us);
      raw.position = posemath::hermite(
          first.position, first.linear_velocity, second.position,
          second.linear_velocity, std::clamp(alpha, 0.0, 1.0),
          static_cast<double>(duration_us) / 1'000'000.0);
    }
    if (first.orientation == second.orientation) {
      raw.orientation = first.orientation;
    } else {
      const auto duration_us = second.send_time_us - first.send_time_us;
      const double alpha =
          duration_us == 0U
              ? 1.0
              : static_cast<double>(target - static_cast<std::int64_t>(first.send_time_us)) /
                    static_cast<double>(duration_us);
      raw.orientation = posemath::quat_slerp(
          first.orientation, second.orientation,
          static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
    }
    return raw;
  }
  raw.position = newest.position;
  raw.orientation = newest.orientation;
  raw.sequence = newest.sequence;
  raw.wheels = newest.wheels;
  raw.state = PosePlaybackState::interpolated;
  return raw;
}

PoseOutput RemotePoseTimeline::Evaluate(const std::uint64_t now_us) noexcept {
  PoseOutput output;
  if (count_ == 0U) {
    have_playback_ = false;
    have_output_ = false;
    return output;
  }

  // 1. Where should playback be on the sender clock right now?
  const auto offset_us = offset_estimate(now_us);
  last_offset_estimate_us_ = offset_us;
  std::uint64_t interval_max_us{};
  std::uint64_t jitter_max_us{};
  recent_maxima(now_us, interval_max_us, jitter_max_us);
  const double delay_raw = config_.interval_factor * static_cast<double>(interval_max_us) +
                           static_cast<double>(jitter_max_us) +
                           static_cast<double>(config_.jitter_margin_us);
  const auto delay_us = static_cast<std::uint64_t>(std::clamp(
      delay_raw, static_cast<double>(config_.min_delay_us),
      static_cast<double>(config_.max_delay_us)));
  const auto ideal = static_cast<std::int64_t>(now_us) - offset_us -
                     static_cast<std::int64_t>(delay_us);

  double advance_us{};
  const auto previous_playback = playback_send_time_us_;
  if (!have_playback_) {
    playback_send_time_us_ = ideal;
    have_playback_ = true;
  } else {
    advance_us = now_us > last_now_us_
                     ? static_cast<double>(now_us - last_now_us_)
                     : 0.0;
    playback_send_time_us_ += static_cast<std::int64_t>(advance_us);
    const auto error = ideal - playback_send_time_us_;
    const auto magnitude = error < 0 ? -error : error;
    if (static_cast<std::uint64_t>(magnitude) > config_.resync_threshold_us) {
      playback_send_time_us_ = ideal;
      correction_position_ = {};
      correction_orientation_ = posemath::kIdentityQuat;
      ++stats_.resyncs;
    } else {
      const double warp =
          static_cast<std::uint64_t>(magnitude) > config_.fast_warp_threshold_us
              ? config_.fast_time_warp
              : config_.max_time_warp;
      const auto limit = static_cast<std::int64_t>(warp * advance_us);
      playback_send_time_us_ += std::clamp(error, -limit, limit);
    }
  }
  last_now_us_ = now_us;
  const auto target = playback_send_time_us_;

  stats_.clock_offset_us = offset_us;
  stats_.delay_us = delay_us;
  stats_.interval_max_us = interval_max_us;
  stats_.jitter_max_us = jitter_max_us;

  // 2. Raw pose from the buffered trajectory.
  const auto raw = evaluate_raw(target);
  output.target_send_time_us = target;
  output.sequence = raw.sequence;
  output.state = raw.state;
  output.wheels = raw.wheels;
  record_latency(now_us, target);

  if (raw.state == PosePlaybackState::held) {
    // Nothing to play: freeze the playback clock so the trajectory resumes
    // exactly where it stopped instead of skipping the missing stretch.
    playback_send_time_us_ = previous_playback;
    output.target_send_time_us = previous_playback;
    ++stats_.held;
    if (have_output_) {
      output.position = last_output_.position;
      output.orientation = last_output_.orientation;
    } else {
      output.position = raw.position;
      output.orientation = raw.orientation;
    }
    last_output_ = output;
    have_output_ = true;
    return output;
  }
  if (raw.state == PosePlaybackState::interpolated) {
    ++stats_.interpolated;
  } else {
    ++stats_.extrapolated;
  }

  // 3. New data changes the curve under the actor. Compare the new curve with
  // the previous output AT THE PREVIOUS TARGET TIME so ordinary motion is not
  // mistaken for an error, then let the difference decay instead of jumping.
  if (new_data_) {
    new_data_ = false;
    if (have_output_) {
      const auto previous = evaluate_raw(last_output_.target_send_time_us);
      const double distance = position_distance(last_output_.position, previous.position);
      const double angle =
          posemath::quat_angle_between(last_output_.orientation, previous.orientation);
      if (distance <= config_.max_position_correction &&
          angle <= config_.max_rotation_correction) {
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          correction_position_[axis] =
              last_output_.position[axis] - previous.position[axis];
        }
        correction_orientation_ = posemath::quat_multiply(
            posemath::quat_conjugate(previous.orientation), last_output_.orientation);
      } else {
        correction_position_ = {};
        correction_orientation_ = posemath::kIdentityQuat;
        ++stats_.snaps;
      }
    }
  }
  const bool correcting =
      correction_position_[0] != 0.0 || correction_position_[1] != 0.0 ||
      correction_position_[2] != 0.0 ||
      correction_orientation_ != posemath::kIdentityQuat;
  if (correcting) {
    if (advance_us > 0.0 && config_.correction_tau_us > 0.0) {
      const double decay = std::exp(-advance_us / config_.correction_tau_us);
      for (auto& component : correction_position_) {
        component *= decay;
        if (std::fabs(component) < 1.0e-4) component = 0.0;
      }
      correction_orientation_ = posemath::quat_slerp(
          posemath::kIdentityQuat, correction_orientation_, static_cast<float>(decay));
      if (posemath::quat_angle_between(posemath::kIdentityQuat,
                                       correction_orientation_) < 1.0e-4) {
        correction_orientation_ = posemath::kIdentityQuat;
      }
    }
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      output.position[axis] = raw.position[axis] + correction_position_[axis];
    }
    output.orientation = posemath::quat_multiply(raw.orientation, correction_orientation_);
  } else {
    output.position = raw.position;
    output.orientation = raw.orientation;
  }
  last_output_ = output;
  have_output_ = true;
  return output;
}

} // namespace ht2mp::bridge
