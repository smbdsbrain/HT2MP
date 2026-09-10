#include "remote_pose_timeline.hpp"
#include "wheel_animation.hpp"
#include "horn_playback.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

int failures{};

void Check(const bool condition, const char* expression, const int line) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    ++failures;
  }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

constexpr double kSpeed = 20.0;  // metres per second along +x

// Deterministic pseudo-random jitter in [0, span_us).
std::uint64_t jitter(const std::uint32_t index, const std::uint64_t span_us) {
  return span_us == 0U ? 0U : ((index * 2654435761U) % 1000U) * span_us / 1000U;
}

ht2mp::bridge::PoseSample sample(const std::uint32_t sequence,
                                 const std::uint64_t send_us,
                                 const std::uint64_t receive_us,
                                 const double x) {
  ht2mp::bridge::PoseSample value;
  value.sequence = sequence;
  value.send_time_us = send_us;
  value.receive_time_us = receive_us;
  value.room_id = 1;
  value.position = {x, 0.0, 0.0};
  value.orientation = ht2mp::posemath::kIdentityQuat;
  value.linear_velocity = {static_cast<float>(kSpeed), 0.0F, 0.0F};
  return value;
}

double truth_x(const std::int64_t send_time_us) {
  return kSpeed * static_cast<double>(send_time_us) / 1'000'000.0;
}

// Feeds a constant-velocity stream: sender period `period_us`, transit
// `transit_us` plus deterministic jitter, evaluated every `frame_us`.
struct StreamResult final {
  std::uint32_t frames{};
  std::uint32_t interpolated{};
  std::uint32_t extrapolated{};
  std::uint32_t held{};
  double max_abs_error{};     // |output.x - truth(target)| after warmup
  double max_step{};          // largest per-frame x advance after warmup
  double min_step{1.0e9};     // smallest per-frame x advance after warmup
};

StreamResult run_stream(ht2mp::bridge::RemotePoseTimeline& timeline,
                        const std::uint64_t period_us,
                        const std::uint64_t transit_us,
                        const std::uint64_t jitter_span_us,
                        const std::uint64_t frame_us,
                        const std::uint64_t duration_us,
                        const std::uint64_t warmup_us,
                        const double sender_rate = 1.0,
                        const std::uint64_t drop_from_us = 0U,
                        const std::uint64_t drop_until_us = 0U) {
  StreamResult result;
  std::uint32_t sequence{};
  std::uint64_t next_send_true_us = 0U;
  bool have_previous = false;
  double previous_x{};
  for (std::uint64_t now = 0U; now <= duration_us; now += frame_us) {
    // Deliver every sample whose arrival time has passed.
    while (true) {
      const auto send_true = next_send_true_us;
      const auto arrival = send_true + transit_us + jitter(sequence + 1U, jitter_span_us);
      if (arrival > now) break;
      next_send_true_us += period_us;
      ++sequence;
      if (drop_until_us != 0U && send_true >= drop_from_us && send_true < drop_until_us) continue;
      const auto send_stamp = static_cast<std::uint64_t>(
          static_cast<double>(send_true) * sender_rate) + 5'000'000U;
      auto value = sample(sequence, send_stamp, arrival + 1U, truth_x(static_cast<std::int64_t>(send_true)));
      (void)timeline.Push(value);
    }
    const auto output = timeline.Evaluate(now + 1U);
    ++result.frames;
    if (output.state == ht2mp::bridge::PosePlaybackState::interpolated) ++result.interpolated;
    if (output.state == ht2mp::bridge::PosePlaybackState::extrapolated) ++result.extrapolated;
    if (output.state == ht2mp::bridge::PosePlaybackState::held) ++result.held;
    if (now < warmup_us || output.state == ht2mp::bridge::PosePlaybackState::none) {
      have_previous = false;
      continue;
    }
    // Truth at the playback target, mapped back to the true sender time.
    const double target_true_us =
        (static_cast<double>(output.target_send_time_us) - 5'000'000.0) / sender_rate;
    const double error = std::fabs(output.position[0] - kSpeed * target_true_us / 1'000'000.0);
    result.max_abs_error = std::max(result.max_abs_error, error);
    if (have_previous) {
      const double step = output.position[0] - previous_x;
      result.max_step = std::max(result.max_step, step);
      result.min_step = std::min(result.min_step, step);
    }
    previous_x = output.position[0];
    have_previous = true;
  }
  std::cout << "  frames=" << result.frames << " interp=" << result.interpolated
            << " extrap=" << result.extrapolated << " held=" << result.held
            << " max_err=" << result.max_abs_error << " step=[" << result.min_step
            << ',' << result.max_step << "] delay_ms=" << timeline.stats().delay_us / 1000U
            << " offset_ms=" << timeline.stats().clock_offset_us / 1000
            << " snaps=" << timeline.stats().snaps << " resync=" << timeline.stats().resyncs
            << " lat_max_ms=" << timeline.stats().latency_max_us / 1000U
            << std::endl;
  return result;
}

void SteadyTwentyHertzIsExactAndSmooth() {
  ht2mp::bridge::RemotePoseTimeline timeline;
  const auto result = run_stream(timeline, 50'000U, 30'000U, 0U, 14'500U,
                                 10'000'000U, 2'000'000U);
  CHECK(result.frames > 600U);
  CHECK(result.held == 0U);
  CHECK(result.extrapolated == 0U);
  CHECK(result.max_abs_error < 0.02);
  // 20 m/s at 14.5 ms per frame is 0.29 m; time warp is limited to 5 %.
  CHECK(result.min_step > 0.25);
  CHECK(result.max_step < 0.33);
  const auto& stats = timeline.stats();
  CHECK(stats.delay_us >= 80'000U && stats.delay_us <= 120'000U);
  // Sender stamps are offset by +5 s in this harness and arrive 30 ms later.
  CHECK(stats.clock_offset_us > -4'971'000 && stats.clock_offset_us < -4'969'000);
  CHECK(stats.latency_max_us < 200'000U);
}

void JitteryNineHertzNeverMovesBackwards() {
  ht2mp::bridge::RemotePoseTimeline timeline;
  const auto result = run_stream(timeline, 111'000U, 30'000U, 80'000U, 14'500U,
                                 20'000'000U, 3'000'000U);
  CHECK(result.frames > 1'000U);
  CHECK(result.min_step >= 0.0);
  CHECK(result.max_step < 0.60);
  CHECK(result.max_abs_error < 1.0);
  CHECK(result.held == 0U);
  const auto& stats = timeline.stats();
  CHECK(stats.delay_us > 150'000U && stats.delay_us <= 350'000U);
}

void LossBurstHoldsThenResumesWithoutSnap() {
  ht2mp::bridge::RemotePoseTimeline timeline;
  // Drop every sample sent between 5.0 s and 5.4 s.
  const auto result = run_stream(timeline, 50'000U, 30'000U, 0U, 14'500U,
                                 10'000'000U, 2'000'000U, 1.0,
                                 5'000'000U, 5'400'000U);
  CHECK(result.extrapolated > 0U);
  CHECK(result.held > 0U);
  // Resuming after the hold must be smoothed: no single frame may jump more
  // than a few frames' worth of motion, and motion never reverses.
  CHECK(result.max_step < 1.2);
  CHECK(result.min_step >= 0.0);
  CHECK(timeline.stats().snaps == 0U);
}

void SenderClockDriftStaysLocked() {
  ht2mp::bridge::RemotePoseTimeline timeline;
  // Sender clock runs 200 ppm fast for ten minutes.
  const auto result = run_stream(timeline, 50'000U, 30'000U, 10'000U, 14'500U,
                                 600'000'000U, 5'000'000U, 1.0002);
  CHECK(result.held == 0U);
  CHECK(result.extrapolated < result.frames / 100U);
  CHECK(result.max_abs_error < 0.2);
  CHECK(timeline.stats().resyncs == 0U);
}

void StationaryInputIsBitStable() {
  ht2mp::bridge::RemotePoseTimeline timeline;
  std::uint32_t sequence{};
  bool have_previous = false;
  ht2mp::bridge::PoseOutput previous;
  std::uint32_t identical{};
  std::uint32_t compared{};
  for (std::uint64_t now = 0U; now < 6'000'000U; now += 14'500U) {
    while ((sequence + 1U) * 50'000U + 30'000U <= now) {
      ++sequence;
      auto value = sample(sequence, sequence * 50'000U + 5'000'000U,
                          sequence * 50'000U + 30'000U, 12.5);
      value.linear_velocity = {};
      value.orientation = {0.0F, 0.0F, 0.3826834F, 0.9238795F};
      CHECK(timeline.Push(value));
    }
    const auto output = timeline.Evaluate(now + 1U);
    if (now < 2'000'000U) continue;
    if (have_previous) {
      ++compared;
      if (output.position == previous.position &&
          output.orientation == previous.orientation) {
        ++identical;
      }
    }
    previous = output;
    have_previous = true;
  }
  CHECK(compared > 200U);
  CHECK(identical == compared);
  CHECK(std::fabs(previous.position[0] - 12.5) < 1.0e-9);
}

void TeleportResetsAndRejectionsCount() {
  ht2mp::bridge::RemotePoseTimeline timeline;
  CHECK(timeline.Push(sample(1U, 1'000'000U, 1'030'000U, 0.0)));
  CHECK(timeline.Push(sample(2U, 1'050'000U, 1'080'000U, 1.0)));
  CHECK(!timeline.Push(sample(2U, 1'100'000U, 1'130'000U, 2.0)));  // duplicate sequence
  CHECK(!timeline.Push(sample(3U, 1'050'000U, 1'130'000U, 2.0)));  // time not advancing
  auto bad = sample(3U, 1'100'000U, 1'130'000U, 2.0);
  bad.position[1] = std::nan("");
  CHECK(!timeline.Push(bad));
  CHECK(timeline.stats().rejected == 3U);
  CHECK(timeline.size() == 2U);

  auto teleport = sample(3U, 1'100'000U, 1'130'000U, 500.0);
  teleport.teleport = true;
  CHECK(timeline.Push(teleport));
  CHECK(timeline.size() == 1U);
  const auto output = timeline.Evaluate(1'400'000U);
  CHECK(output.state != ht2mp::bridge::PosePlaybackState::none);
  CHECK(std::fabs(output.position[0] - 500.0) < 30.0);

  auto room_change = sample(4U, 1'150'000U, 1'180'000U, 900.0);
  room_change.room_id = 2;
  CHECK(timeline.Push(room_change));
  CHECK(timeline.size() == 1U);

  timeline.Reset();
  CHECK(timeline.size() == 0U);
  CHECK(timeline.Evaluate(2'000'000U).state == ht2mp::bridge::PosePlaybackState::none);
}

void OrientationExtrapolationUsesBodyAngularVelocity() {
  // A single sample turning at 1 rad/s about +z: after 100 ms of
  // extrapolation the yaw must have advanced by about 0.1 rad.
  ht2mp::bridge::RemotePoseTimeline timeline;
  ht2mp::bridge::PoseTimelineConfig config;
  config.min_delay_us = 0U;
  config.jitter_margin_us = 0U;
  timeline.set_config(config);
  auto value = sample(1U, 1'000'000U, 1'000'100U, 0.0);
  value.linear_velocity = {};
  value.angular_velocity = {0.0F, 0.0F, 1.0F};
  CHECK(timeline.Push(value));
  const auto output = timeline.Evaluate(1'100'100U);
  CHECK(output.state == ht2mp::bridge::PosePlaybackState::extrapolated);
  const double yaw = 2.0 * std::atan2(output.orientation[2], output.orientation[3]);
  CHECK(std::fabs(yaw - 0.1) < 0.01);

  ht2mp::posemath::Vec3f recovered{};
  CHECK(ht2mp::posemath::body_angular_velocity(value.orientation, output.orientation,
                                               0.1, recovered));
  CHECK(std::fabs(recovered[2] - 1.0F) < 0.02F);
  CHECK(std::fabs(recovered[0]) < 0.001F && std::fabs(recovered[1]) < 0.001F);
}

void MatrixQuaternionRoundTrip() {
  const ht2mp::posemath::Quat source =
      ht2mp::posemath::quat_normalized({0.2F, -0.4F, 0.1F, 0.85F});
  const auto matrix = ht2mp::posemath::quat_to_matrix(source);
  const auto back = ht2mp::posemath::matrix_to_quaternion(matrix);
  CHECK(ht2mp::posemath::quat_angle_between(source, back) < 1.0e-4);
  const auto again = ht2mp::posemath::quat_to_matrix(back);
  for (std::size_t index = 0U; index < 9U; ++index) {
    CHECK(std::fabs(again[index] - matrix[index]) < 1.0e-5F);
  }
}

void FramePlaybackContinuesBetweenBatchedAiTicks() {
  // Reproduce the actual boundary split: 20 Hz input drained only at 12.5 Hz
  // AI ticks, but a new pose requested every 8 ms. Extra same-time AI/frame
  // evaluations must not advance the clock twice or leave 80 ms plateaus.
  ht2mp::bridge::RemotePoseTimeline timeline;
  std::uint32_t sequence{};
  double previous_x{};
  std::uint32_t compared{};
  for (std::uint64_t now = 0U; now <= 10'000'000U; now += 8'000U) {
    if (now % 80'000U == 0U) {
      while ((sequence + 1U) * 50'000ULL + 10'000U <= now) {
        ++sequence;
        const auto capture = sequence * 50'000ULL;
        CHECK(timeline.Push(sample(sequence, capture + 5'000'000U,
                                   capture + 10'000U,
                                   truth_x(static_cast<std::int64_t>(capture)))));
      }
      const auto at_ai_tick = timeline.Evaluate(now + 1U);
      const auto same_time_frame = timeline.Evaluate(now + 1U);
      CHECK(at_ai_tick.target_send_time_us == same_time_frame.target_send_time_us);
      CHECK(at_ai_tick.position == same_time_frame.position);
    }
    const auto output = timeline.Evaluate(now + 1U);
    if (now > 2'000'000U) {
      const auto step = output.position[0] - previous_x;
      CHECK(step > 0.10 && step < 0.21);  // 20 m/s * 8 ms, including clock warp
      CHECK(output.state != ht2mp::bridge::PosePlaybackState::held);
      ++compared;
    }
    previous_x = output.position[0];
  }
  CHECK(compared >= 1'000U);
  CHECK(timeline.stats().snaps == 0U);
  CHECK(timeline.stats().resyncs == 0U);
}

void WheelAnimationUsesPlaybackTimeAndStopsOnLoss() {
  ht2mp::bridge::WheelAnimation animation;
  ht2mp::protocol::WheelSpeeds speeds{8, {10, -20, 0, 40, 5, 6, 7, 8}};
  animation.Advance(speeds, 1'000'000, false);
  animation.Advance(speeds, 1'100'000, false);
  CHECK(std::abs(animation.phases()[0] - 1.0F) < 1e-6F);
  CHECK(std::abs(animation.phases()[1] + 2.0F) < 1e-6F);
  CHECK(animation.phases()[2] == 0.0F);
  CHECK(std::abs(animation.phases()[7] - 0.8F) < 1e-6F);
  const auto initial = animation.phases();
  animation.Advance(speeds, 1'100'000, false); // multiple render passes
  animation.Advance(speeds, 1'350'000, true); // body playback holds on loss
  CHECK(animation.phases() == initial);
  speeds.radians_per_second.fill(0.0F);
  animation.Advance(speeds, 1'400'000, false);
  CHECK(animation.phases() == initial);
  speeds.radians_per_second.fill(-10.0F);
  animation.Advance(speeds, 1'500'000, false);
  CHECK(animation.phases()[0] < initial[0]); // reverse resumes smoothly
  animation.Reset();
  animation.Advance(speeds, 5'000'000, false);
  CHECK(animation.phases()[0] == 0.0F); // reused actor has no previous phase
  const auto front = ht2mp::bridge::WheelRotation(1.0F, 0.785398185F);
  const auto rear = ht2mp::bridge::WheelRotation(1.0F, 0.0F);
  CHECK(std::abs(std::atan2(front[1], front[0]) - 0.785398185F) < 1e-6F);
  CHECK(std::abs(front[5] - std::sin(1.0F)) < 1e-6F);
  CHECK(rear[0] == 1.0F && rear[1] == 0.0F && rear[5] == front[5]);
}

void WheelSpeedsShareBodyInterpolationClock() {
  ht2mp::bridge::RemotePoseTimeline timeline;
  ht2mp::bridge::PoseTimelineConfig config;
  config.min_delay_us = config.max_delay_us = 80'000;
  timeline.set_config(config);
  auto first = sample(1, 1'000'000, 1'500'000, 0.0);
  first.wheels = {4, {0, 20, -40, 10}};
  auto second = sample(2, 1'100'000, 1'600'000, 2.0);
  second.wheels = {4, {20, 40, -20, 0}};
  CHECK(timeline.Push(first));
  CHECK(timeline.Push(second));
  const auto output = timeline.Evaluate(1'630'000);
  CHECK(output.target_send_time_us == 1'050'000);
  CHECK(output.wheels.count == 4);
  CHECK(output.wheels.radians_per_second[0] == 10.0F);
  CHECK(output.wheels.radians_per_second[1] == 30.0F);
  CHECK(output.wheels.radians_per_second[2] == -30.0F);
  CHECK(output.wheels.radians_per_second[3] == 5.0F);
}

void HornTapsHoldReleaseLossAndReuse() {
  using ht2mp::bridge::HornPlayback;
  HornPlayback horn;
  horn.Observe({false, 1, 12}, 1'000'000, 1'000'000);
  CHECK(!horn.Playing(1'000'000)); // late join must not replay historical taps
  horn.Observe({false, 1, 13}, 1'100'000, 1'100'000);
  CHECK(horn.Playing(1'100'000)); // press+release between network samples
  horn.Observe({false, 1, 13}, 1'150'000, 1'150'000);
  CHECK(horn.Playing(1'219'999));
  CHECK(!horn.Playing(1'220'000)); // repeats do not extend the pulse
  horn.Observe({true, 2, 14}, 1'300'000, 1'300'000);
  horn.Observe({true, 2, 14}, 1'600'000, 1'600'000);
  CHECK(horn.Playing(1'800'000) && horn.tone() == 2);
  horn.Observe({false, 2, 14}, 1'850'000, 1'850'000);
  CHECK(!horn.Playing(1'850'000)); // a held horn stops on release
  horn.Observe({true, 1, 15}, 2'000'000, 2'000'000);
  CHECK(horn.Playing(2'500'000));
  CHECK(!horn.Playing(2'500'001)); // lost release cannot latch the sound
  horn.Observe({false, 1, 16}, 2'100'000, 3'000'000);
  CHECK(!horn.Playing(3'000'000)); // stale tap expires
  horn.Reset();
  horn.Observe({false, 0, 65535}, 4'000'000, 4'000'000);
  CHECK(!horn.Playing(4'000'000));
  horn.Observe({false, 0, 0}, 4'100'000, 4'100'000);
  CHECK(horn.Playing(4'100'000)); // 16-bit counter wrap
  HornPlayback second;
  second.Observe({true, 2, 99}, 4'150'000, 4'150'000);
  CHECK(horn.Playing(4'150'000) && second.Playing(4'150'000));
  horn.Reset();
  CHECK(!horn.Playing(4'150'000) && second.Playing(4'150'000));
  second.Observe({false, 2, 100}, 4'140'000, 4'160'000);
  CHECK(second.Playing(4'300'000)); // older packet cannot replace held state
  second.Reset();
  CHECK(!second.Playing(4'300'000));
}

} // namespace

int main() {
  HornTapsHoldReleaseLossAndReuse();
  WheelAnimationUsesPlaybackTimeAndStopsOnLoss();
  WheelSpeedsShareBodyInterpolationClock();
  SteadyTwentyHertzIsExactAndSmooth();
  JitteryNineHertzNeverMovesBackwards();
  LossBurstHoldsThenResumesWithoutSnap();
  SenderClockDriftStaysLocked();
  StationaryInputIsBitStable();
  TeleportResetsAndRejectionsCount();
  OrientationExtrapolationUsesBodyAngularVelocity();
  MatrixQuaternionRoundTrip();
  FramePlaybackContinuesBetweenBatchedAiTicks();
  if (failures != 0) {
    std::cerr << failures << " remote pose timeline test(s) failed\n";
    return 1;
  }
  std::cout << "remote pose timeline tests passed\n";
  return 0;
}
