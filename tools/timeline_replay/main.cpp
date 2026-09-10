// Replays a sidecar `remote_states.csv` trace (written by `ht2mp-client run
// --trace-dir`) through the bridge's RemotePoseTimeline exactly as the game
// thread would consume it: every state is pushed at its recorded receive time
// and the timeline is evaluated at a fixed step in between.
//
// Usage: ht2mp-timeline-replay <remote_states.csv> <playback.csv> [step_us]
//
// Output columns: t_us (receive clock), state, seq, target_send_us, x, y, z,
// qx, qy, qz, qw. Summary statistics go to stdout.

#include "remote_pose_timeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct TraceRow final {
  std::uint64_t receive_us{};
  std::uint32_t sequence{};
  std::uint64_t sample_time_ms{};
  std::uint32_t flags{};
  double x{}, y{}, z{};
  double qx{}, qy{}, qz{}, qw{};
  float vx{}, vy{}, vz{};
  float wx{}, wy{}, wz{};
  std::int32_t room{};
};

bool parse_row(const std::string& line, TraceRow& row) {
  std::stringstream stream(line);
  std::string field;
  std::vector<std::string> fields;
  while (std::getline(stream, field, ',')) fields.push_back(field);
  if (fields.size() < 20) return false;
  std::size_t index = 0;
  auto next = [&]() -> const std::string& { return fields[index++]; };
  row.receive_us = std::strtoull(next().c_str(), nullptr, 10);
  const auto& source = next();
  (void)source;
  index++;  // player
  row.sequence = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
  row.sample_time_ms = std::strtoull(next().c_str(), nullptr, 10);
  row.flags = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 10));
  row.x = std::strtod(next().c_str(), nullptr);
  row.y = std::strtod(next().c_str(), nullptr);
  row.z = std::strtod(next().c_str(), nullptr);
  row.qx = std::strtod(next().c_str(), nullptr);
  row.qy = std::strtod(next().c_str(), nullptr);
  row.qz = std::strtod(next().c_str(), nullptr);
  row.qw = std::strtod(next().c_str(), nullptr);
  row.vx = std::strtof(next().c_str(), nullptr);
  row.vy = std::strtof(next().c_str(), nullptr);
  row.vz = std::strtof(next().c_str(), nullptr);
  row.wx = std::strtof(next().c_str(), nullptr);
  row.wy = std::strtof(next().c_str(), nullptr);
  row.wz = std::strtof(next().c_str(), nullptr);
  row.room = static_cast<std::int32_t>(std::strtol(next().c_str(), nullptr, 10));
  return true;
}

const char* state_name(const ht2mp::bridge::PosePlaybackState state) {
  switch (state) {
    case ht2mp::bridge::PosePlaybackState::interpolated: return "interp";
    case ht2mp::bridge::PosePlaybackState::extrapolated: return "extrap";
    case ht2mp::bridge::PosePlaybackState::held: return "held";
    default: return "none";
  }
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: ht2mp-timeline-replay <remote_states.csv> <playback.csv> [step_us]\n";
    return 2;
  }
  const std::uint64_t step_us = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1'000U;
  std::ifstream input(argv[1]);
  if (!input) {
    std::cerr << "cannot open " << argv[1] << '\n';
    return 1;
  }
  std::vector<TraceRow> rows;
  std::string line;
  std::getline(input, line);  // header
  while (std::getline(input, line)) {
    TraceRow row;
    if (parse_row(line, row)) rows.push_back(row);
  }
  std::stable_sort(rows.begin(), rows.end(), [](const TraceRow& a, const TraceRow& b) {
    return a.receive_us < b.receive_us;
  });
  if (rows.empty()) {
    std::cerr << "no rows\n";
    return 1;
  }
  std::ofstream output(argv[2]);
  output << "t_us,state,seq,target_send_us,x,y,z,qx,qy,qz,qw\n";
  output.precision(9);

  ht2mp::bridge::RemotePoseTimeline timeline;
  timeline.Reset();
  std::uint64_t now = rows.front().receive_us;
  std::size_t next_row = 0;
  std::uint32_t rejected = 0;
  std::uint64_t evaluations = 0;
  const std::uint64_t end = rows.back().receive_us + 200'000U;
  while (now <= end) {
    while (next_row < rows.size() && rows[next_row].receive_us <= now) {
      const auto& row = rows[next_row++];
      ht2mp::bridge::PoseSample sample;
      sample.send_time_us = row.sample_time_ms * 1'000U;
      sample.receive_time_us = row.receive_us;
      sample.sequence = row.sequence;
      sample.room_id = row.room;
      sample.teleport = (row.flags & 4U) != 0U;
      sample.position = {row.x, row.y, row.z};
      sample.orientation = {static_cast<float>(row.qx), static_cast<float>(row.qy),
                            static_cast<float>(row.qz), static_cast<float>(row.qw)};
      sample.linear_velocity = {row.vx, row.vy, row.vz};
      sample.angular_velocity = {row.wx, row.wy, row.wz};
      if (!timeline.Push(sample)) ++rejected;
    }
    const auto out = timeline.Evaluate(now);
    ++evaluations;
    if (out.state != ht2mp::bridge::PosePlaybackState::none) {
      output << now << ',' << state_name(out.state) << ',' << out.sequence << ','
             << out.target_send_time_us << ',' << out.position[0] << ','
             << out.position[1] << ',' << out.position[2] << ','
             << out.orientation[0] << ',' << out.orientation[1] << ','
             << out.orientation[2] << ',' << out.orientation[3] << '\n';
    }
    now += step_us;
  }
  const auto& stats = timeline.stats();
  std::cout << "rows=" << rows.size() << " evaluations=" << evaluations
            << " rejected=" << rejected << " pushed=" << stats.pushed
            << " interp=" << stats.interpolated << " extrap=" << stats.extrapolated
            << " held=" << stats.held << " resync=" << stats.resyncs
            << " snaps=" << stats.snaps << " delay_us=" << stats.delay_us
            << " offset_us=" << stats.clock_offset_us << '\n';
  return 0;
}
