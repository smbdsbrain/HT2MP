#include "test_support.hpp"
#include "session.hpp"
#include "ht2mp/protocol/codec.hpp"
#if defined(HT2MP_TEST_IPC)
#include "ht2mp/ipc/codec.hpp"
#endif
#include <array>
#include <cmath>
#include <limits>
#include <set>

namespace {
bool world_clock_and_join() {
  ht2mp::coordinator::SessionConfig config;
  config.profile_id = "steam-8138acee";
  config.session_id = 42;
  config.environment.day_hours = 23.5;
  config.environment.day_duration_s = 120;
  ht2mp::coordinator::Session server(config);
  HT2MP_CHECK(server.accept(1, {config.profile_id, {}, "A", 11, 0}, 1000).accepted);
  auto first = server.tick(1000);
  HT2MP_CHECK(first.size() == 1);
  const auto& empty = std::get<ht2mp::protocol::WorldSnapshot>(first[0].message);
  HT2MP_CHECK(empty.players.empty() && empty.environment.session_id == 42);
  HT2MP_CHECK(empty.environment.day_hours == 23.5);
  // Midnight, no player updates, and a newly joining peer must all see the
  // same running world. A client cannot submit a replacement world snapshot.
  HT2MP_CHECK(server.accept(2, {config.profile_id, {}, "B", 22, 0}, 11000).accepted);
  auto later = server.tick(11000);
  HT2MP_CHECK(later.size() == 2 && later[0].message == later[1].message);
  const auto& world = std::get<ht2mp::protocol::WorldSnapshot>(later[0].message);
  HT2MP_CHECK(std::abs(world.environment.day_hours - 1.5) < 1e-8);
  HT2MP_CHECK(!server.receive(1, world, 11001).accepted);
  HT2MP_CHECK(server.tick(11002).empty());
  return true;
}

bool full_room_snapshot_chunks_and_repair() {
  ht2mp::coordinator::SessionConfig config;
  config.profile_id = "steam-8138acee";
  config.session_id = 42;
  ht2mp::coordinator::Session server(config);
  ht2mp::protocol::PlayerState state;
  state.session_id = 42;
  state.flags = 1;
  state.sequence = 1;
  state.vehicle.valid = true;
  state.vehicle.steering = -85;
  state.vehicle.lights = {1, 0, 1, 2};
  state.vehicle.condition.fill(0.73F);
  state.vehicle.wheels = {8, {-12.0F, 30.0F, 90.0F, 100.0F, 0.0F, 25.0F, 44.0F, 65.0F}};
  state.vehicle.horn = {true, 2, 65535};
  for (std::uint64_t i = 1; i <= ht2mp::protocol::kMaxPlayers; ++i) {
    HT2MP_CHECK(server.accept(i, {config.profile_id, {}, std::to_string(i), i + 10, 0}, 1000).accepted);
    state.player_id = server.peer(i)->player_id;
    state.incarnation_id = i + 10;
    HT2MP_CHECK(server.receive(i, state, 1001).accepted);
  }
  const auto routes = server.tick(1050);
  constexpr std::size_t chunk_count =
      (ht2mp::protocol::kMaxPlayers +
       ht2mp::protocol::kMaxSnapshotPlayers - 1U) /
      ht2mp::protocol::kMaxSnapshotPlayers;
  static_assert(chunk_count == 22U);
  HT2MP_CHECK(routes.size() ==
              ht2mp::protocol::kMaxPlayers * chunk_count);
  std::set<std::uint64_t> players;
  for (const auto& route : routes) {
    const auto encoded = ht2mp::protocol::encode_packet(route.message);
    HT2MP_CHECK(encoded && encoded.bytes.size() <= 1200);
    const auto decoded = ht2mp::protocol::decode_packet(encoded.bytes);
    HT2MP_CHECK(decoded && decoded.message == route.message);
    if (route.target == 1) {
      for (const auto& player : std::get<ht2mp::protocol::WorldSnapshot>(route.message).players) {
        HT2MP_CHECK(players.insert(player.player_id).second);
        HT2MP_CHECK(player.vehicle == state.vehicle);
      }
    }
  }
  HT2MP_CHECK(players.size() == ht2mp::protocol::kMaxPlayers);
  // A real repair replaces damage, so no monotonic-damage/max merge is allowed.
  state.sequence = 2;
  state.vehicle.condition.fill(0.0F);
  state.vehicle.steering = 80;
  state.vehicle.lights = {};
  state.vehicle.horn = {false, 2, 0}; // release + another tap, counter wraps
  const auto repair = server.receive(ht2mp::protocol::kMaxPlayers, state, 1100);
  HT2MP_CHECK(repair.accepted &&
              repair.routes.size() == ht2mp::protocol::kMaxPlayers - 1U);
  HT2MP_CHECK(std::get<ht2mp::protocol::PlayerState>(repair.routes[0].message).vehicle == state.vehicle);
  HT2MP_CHECK(server.receive(ht2mp::protocol::kMaxPlayers, state, 1101).routes.empty());
  return true;
}

bool environment_ipc_and_validation() {
#if defined(HT2MP_TEST_IPC)
  ht2mp::ipc::EnvironmentV1 source;
  source.server_time_ms = 12345;
  source.receive_time_us = 777777;
  source.state.session_id = 42;
  source.state.day_hours = 23.99;
  source.state.weather_phase = 4.0;
  source.state.weather_preset = 7;
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> bytes{};
  std::size_t size{};
  HT2MP_CHECK(ht2mp::ipc::encode_environment(source, bytes, size) == ht2mp::ipc::CodecError::none);
  ht2mp::ipc::FrameHeader header;
  std::span<const std::byte> payload;
  HT2MP_CHECK(ht2mp::ipc::decode_frame(std::span(bytes).first(size), header, payload) == ht2mp::ipc::CodecError::none);
  HT2MP_CHECK(header.type == ht2mp::ipc::MessageType::environment);
  ht2mp::ipc::EnvironmentV1 decoded;
  HT2MP_CHECK(ht2mp::ipc::decode_environment(payload, decoded) == ht2mp::ipc::CodecError::none);
  HT2MP_CHECK(decoded.state == source.state && decoded.receive_time_us == source.receive_time_us && decoded.server_time_ms == source.server_time_ms);
  HT2MP_CHECK(ht2mp::ipc::decode_environment(payload.first(payload.size() - 1), decoded) != ht2mp::ipc::CodecError::none);
  source.state.day_hours = std::numeric_limits<double>::quiet_NaN();
  HT2MP_CHECK(ht2mp::ipc::encode_environment(source, bytes, size) != ht2mp::ipc::CodecError::none);
#endif
  ht2mp::protocol::VehicleState invalid;
  invalid.lights[0] = 3;
  HT2MP_CHECK(!ht2mp::protocol::valid_vehicle_state(invalid));
  invalid = {};
  invalid.condition[0] = 1.01F;
  HT2MP_CHECK(!ht2mp::protocol::valid_vehicle_state(invalid));
  invalid.condition[0] = std::numeric_limits<float>::infinity();
  HT2MP_CHECK(!ht2mp::protocol::valid_vehicle_state(invalid));
  invalid = {};
  invalid.wheels.count = 9;
  HT2MP_CHECK(!ht2mp::protocol::valid_vehicle_state(invalid));
  invalid.wheels.count = 2;
  invalid.wheels.radians_per_second[0] = std::numeric_limits<float>::quiet_NaN();
  HT2MP_CHECK(!ht2mp::protocol::valid_vehicle_state(invalid));
  invalid.wheels.radians_per_second[0] = 2001.0F;
  HT2MP_CHECK(!ht2mp::protocol::valid_vehicle_state(invalid));
  invalid.wheels.radians_per_second[0] = -1999.0F;
  HT2MP_CHECK(ht2mp::protocol::valid_vehicle_state(invalid));
  invalid.wheels.radians_per_second[7] = 1.0F;
  HT2MP_CHECK(!ht2mp::protocol::valid_vehicle_state(invalid));
  invalid = {};
  invalid.horn.tone = 3;
  HT2MP_CHECK(!ht2mp::protocol::valid_vehicle_state(invalid));
  return true;
}
}

int main() {
  return run_test("server world clock and late join", world_clock_and_join) +
         run_test("64 players, packet budget and repair", full_room_snapshot_chunks_and_repair) +
         run_test("environment IPC and hostile values", environment_ipc_and_validation);
}
