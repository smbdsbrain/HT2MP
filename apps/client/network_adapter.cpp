#include "network_adapter.hpp"

#include "ht2mp/ipc/codec.hpp"
#include "ht2mp/windows/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ht2mp::client {
namespace {

std::uint64_t monotonic_ms() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

int hex_nibble(const char ch) noexcept {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool parse_token(const std::string_view text, ht2mp::protocol::Token128& token) {
  if (text.empty()) {
    token.fill(std::byte{0U});
    return true;
  }
  if (text.size() != token.size() * 2U) return false;
  for (std::size_t i = 0; i < token.size(); ++i) {
    const auto high = hex_nibble(text[i * 2U]);
    const auto low = hex_nibble(text[i * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    token[i] = static_cast<std::byte>((high << 4) | low);
  }
  return true;
}

bool parse_endpoint(const std::string_view endpoint, std::string& host,
                    std::uint16_t& port, std::string& error) {
  if (endpoint.empty()) {
    error = "Coordinator endpoint is empty";
    return false;
  }
  std::string_view host_view;
  std::string_view port_view;
  if (endpoint.front() == '[') {
    const auto close = endpoint.find(']');
    if (close == std::string_view::npos || close == 1U || close + 1U >= endpoint.size() ||
        endpoint[close + 1U] != ':') {
      error = "IPv6 endpoints must use [address]:port";
      return false;
    }
    host_view = endpoint.substr(1U, close - 1U);
    port_view = endpoint.substr(close + 2U);
  } else {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string_view::npos) {
      host_view = endpoint;
    } else {
      if (endpoint.find(':') != colon) {
        error = "IPv6 endpoints must use [address]:port";
        return false;
      }
      host_view = endpoint.substr(0U, colon);
      port_view = endpoint.substr(colon + 1U);
    }
  }
  if (host_view.empty()) {
    error = "Coordinator host is empty";
    return false;
  }
  port = 28020U;
  if (!port_view.empty()) {
    std::uint32_t parsed{};
    const auto result = std::from_chars(port_view.data(),
                                        port_view.data() + port_view.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != port_view.data() + port_view.size() ||
        parsed == 0U || parsed > std::numeric_limits<std::uint16_t>::max()) {
      error = "Coordinator port is invalid";
      return false;
    }
    port = static_cast<std::uint16_t>(parsed);
  } else if (endpoint.ends_with(':')) {
    error = "Coordinator port is empty";
    return false;
  }
  host.assign(host_view);
  return true;
}

bool new_incarnation(std::uint64_t& value, std::string& error) {
  std::array<std::uint8_t, sizeof(value)> bytes{};
  if (!ht2mp::windows::secure_random(bytes, error)) return false;
  std::memcpy(&value, bytes.data(), sizeof(value));
  if (value == 0U) value = 1U;
  return true;
}

ht2mp::ipc::PlayerSampleV1 to_ipc(const ht2mp::protocol::PlayerState& state) {
  ht2mp::ipc::PlayerSampleV1 output;
  output.player_id = state.player_id;
  output.incarnation_id = state.incarnation_id;
  output.sample_time_ms = state.sample_time_ms;
  output.sequence = state.sequence;
  output.flags = state.flags;
  output.position = {state.position.x, state.position.y, state.position.z};
  output.orientation = {state.orientation.x, state.orientation.y,
                        state.orientation.z, state.orientation.w};
  output.linear_velocity = {state.linear_velocity.x, state.linear_velocity.y,
                            state.linear_velocity.z};
  output.angular_velocity = {state.angular_velocity.x, state.angular_velocity.y,
                             state.angular_velocity.z};
  output.vehicle_type = state.vehicle_type;
  output.paint_variant = state.paint_variant;
  output.vehicle = state.vehicle;
  output.location = {state.location.room_id,
                     state.location.road_id,
                     state.location.node_id,
                     state.location.road_distance,
                     state.location.road_segment_vector_id,
                     state.location.road_segment_id,
                     state.location.aux0,
                     state.location.aux1};
  return output;
}

ht2mp::protocol::PlayerState to_network(const ht2mp::ipc::PlayerSampleV1& state) {
  ht2mp::protocol::PlayerState output;
  output.sequence = state.sequence;
  // The capture instant on the bridge's QPC clock travels unchanged; the
  // receiving bridge estimates this sender's clock offset from arrival times.
  output.sample_time_ms = state.sample_time_ms;
  output.flags = state.flags;
  output.position = {state.position[0], state.position[1], state.position[2]};
  output.orientation = {state.orientation[0], state.orientation[1],
                        state.orientation[2], state.orientation[3]};
  output.linear_velocity = {state.linear_velocity[0], state.linear_velocity[1],
                            state.linear_velocity[2]};
  output.angular_velocity = {state.angular_velocity[0], state.angular_velocity[1],
                             state.angular_velocity[2]};
  output.vehicle_type = state.vehicle_type;
  output.paint_variant = state.paint_variant;
  output.vehicle = state.vehicle;
  output.location = {state.location.room_id,
                     state.location.road_id,
                     state.location.node_id,
                     state.location.road_distance,
                     state.location.road_segment_vector_id,
                     state.location.road_segment_id,
                     state.location.aux0,
                     state.location.aux1};
  return output;
}

template <typename Encoder, typename Message>
bool write_ipc(PipeServer& pipe, Encoder encoder, const Message& message,
               std::string& error) {
  std::array<std::byte, ht2mp::ipc::kMaximumFrameSize> frame{};
  std::size_t size{};
  if (encoder(message, frame, size) != ht2mp::ipc::CodecError::none) {
    error = "Cannot encode bridge IPC message";
    return false;
  }
  return pipe.write(std::span(frame).first(size), error);
}

} // namespace

struct NetworkAdapter::RemotePeer final {
  std::uint64_t player_id{};
  std::uint64_t incarnation_id{};
  std::uint16_t vehicle_type{};
  std::uint8_t paint_variant{};
  std::string display_name;
  // Newest accepted authoritative state; playback interpolation now lives in
  // the bridge, which receives every state with its arrival stamp.
  std::optional<ht2mp::protocol::PlayerState> latest;
  std::uint64_t last_receive_us{};
  std::uint32_t last_sequence{};
  bool has_sequence{};
  bool has_metadata{};
  bool spawned{};
  bool hidden{};
};

namespace {

constexpr std::uint64_t kHideAfterUs = 3'000'000U;
constexpr std::uint64_t kDespawnAfterUs = 5'000'000U;

std::uint64_t receive_stamp_or_now(const std::uint64_t received_at_us) noexcept {
  if (received_at_us != 0U) return received_at_us;
  const auto now = ht2mp::net::qpc_microseconds();
  return now == 0U ? 1U : now;
}

} // namespace

NetworkAdapter::NetworkAdapter(
    NetworkOptions options, const std::uint32_t bridge_capabilities,
    const ht2mp::ipc::BridgeMode bridge_mode,
    const bool profile_remote_writes_validated)
    : options_(std::move(options)),
      vehicle_type_ready_(!options_.endpoint.empty() ? false :
                          options_.vehicle_type != kDiscoverVehicleType),
      remote_actor_commands_enabled_(
          profile_remote_writes_validated &&
          bridge_mode == ht2mp::ipc::BridgeMode::active &&
          (bridge_capabilities & static_cast<std::uint32_t>(
                                     ht2mp::ipc::Capability::remote_actors)) !=
              0U) {
  if (const auto limits =
          ht2mp::protocol::limits_for_profile(options_.edition.profile_id)) {
    environment_enabled_ = remote_actor_commands_enabled_ &&
        (bridge_capabilities & static_cast<std::uint32_t>(ht2mp::ipc::Capability::environment)) != 0U;
    profile_limits_ = *limits;
  }
}

NetworkAdapter::~NetworkAdapter() = default;

bool NetworkAdapter::enabled() const noexcept {
  return !options_.endpoint.empty();
}

bool NetworkAdapter::start(std::string& error) {
  error.clear();
  if (!enabled()) return true;
  if (!parse_endpoint(options_.endpoint, host_, port_, error) ||
      !parse_token(options_.token, token_)) {
    if (error.empty()) error = "Session token is malformed";
    return false;
  }
  if (!options_.trace_dir.empty()) {
    std::error_code trace_error;
    std::filesystem::create_directories(options_.trace_dir, trace_error);
    local_trace_.open(options_.trace_dir / "local_samples.csv",
                      std::ios::out | std::ios::trunc);
    remote_trace_.open(options_.trace_dir / "remote_states.csv",
                       std::ios::out | std::ios::trunc);
    if (local_trace_) {
      local_trace_ << "recv_us,seq,sample_time_ms,flags,x,y,z,qx,qy,qz,qw,"
                      "vx,vy,vz,wx,wy,wz,room,road,dist\n";
    }
    if (remote_trace_) {
      remote_trace_ << "recv_us,source,player,seq,sample_time_ms,flags,x,y,z,"
                       "qx,qy,qz,qw,vx,vy,vz,wx,wy,wz,room,road,dist\n";
    }
  }
  if (!vehicle_type_ready_) return true;
  if (!connect_new(error)) {
    std::cerr << "network: " << error << "; retry scheduled\n";
    error.clear();
  }
  return true;
}

void NetworkAdapter::trace_remote(const ht2mp::protocol::PlayerState& state,
                                  const std::uint64_t received_at_us,
                                  const char* source) {
  if (!remote_trace_) return;
  remote_trace_ << received_at_us << ',' << source << ',' << state.player_id
                << ',' << state.sequence << ',' << state.sample_time_ms << ','
                << static_cast<unsigned>(state.flags) << ',' << state.position.x
                << ',' << state.position.y << ',' << state.position.z << ','
                << state.orientation.x << ',' << state.orientation.y << ','
                << state.orientation.z << ',' << state.orientation.w << ','
                << state.linear_velocity.x << ',' << state.linear_velocity.y
                << ',' << state.linear_velocity.z << ','
                << state.angular_velocity.x << ',' << state.angular_velocity.y
                << ',' << state.angular_velocity.z << ','
                << state.location.room_id << ',' << state.location.road_id
                << ',' << state.location.road_distance << '\n';
  if (++trace_rows_ % 16U == 0U) remote_trace_.flush();
}

void NetworkAdapter::trace_local(const ht2mp::ipc::PlayerSampleV1& sample,
                                 const std::uint64_t received_at_us) {
  if (!local_trace_) return;
  local_trace_ << received_at_us << ',' << sample.sequence << ','
               << sample.sample_time_ms << ','
               << static_cast<unsigned>(sample.flags) << ','
               << sample.position[0] << ',' << sample.position[1] << ','
               << sample.position[2] << ',' << sample.orientation[0] << ','
               << sample.orientation[1] << ',' << sample.orientation[2] << ','
               << sample.orientation[3] << ',' << sample.linear_velocity[0]
               << ',' << sample.linear_velocity[1] << ','
               << sample.linear_velocity[2] << ',' << sample.angular_velocity[0]
               << ',' << sample.angular_velocity[1] << ','
               << sample.angular_velocity[2] << ',' << sample.location.room_id
               << ',' << sample.location.road_id << ','
               << sample.location.road_distance << '\n';
  if (++trace_rows_ % 16U == 0U) local_trace_.flush();
}

bool NetworkAdapter::connect_new(std::string& error) {
  std::uint64_t incarnation{};
  if (!new_incarnation(incarnation, error)) return false;
  ht2mp::net::ClientConfig config;
  config.server_host = host_;
  config.server_port = port_;
  config.profile_id = std::string(options_.edition.profile_id);
  config.token = token_;
  config.display_name = options_.player_name;
  config.incarnation_id = incarnation;
  config.vehicle_type = options_.vehicle_type;
  config.paint_variant = options_.paint_variant;
  config.connect_timeout_ms = 5'000U;
  client_ = std::make_unique<ht2mp::net::Client>(std::move(config));
  if (!client_->start(error)) {
    client_.reset();
    schedule_reconnect();
    return false;
  }
  return true;
}

void NetworkAdapter::schedule_reconnect() noexcept {
  next_reconnect_ms_ = monotonic_ms() + reconnect_delay_ms_;
  reconnect_delay_ms_ = std::min(reconnect_delay_ms_ * 2U, 5'000U);
}

bool NetworkAdapter::pump(PipeServer& pipe, std::string& error,
                          const std::uint32_t poll_timeout_ms) {
  error.clear();
  if (!enabled() || terminal_rejection_) return true;
  if (!vehicle_type_ready_) return update_liveness(pipe, error);
  if (!client_) {
    if (monotonic_ms() < next_reconnect_ms_) {
      return update_liveness(pipe, error);
    }
    std::string connect_error;
    if (!connect_new(connect_error)) {
      std::cerr << "network: " << connect_error << "; retry scheduled\n";
      return update_liveness(pipe, error);
    }
  }
  if (client_->state() == ht2mp::net::ClientState::connected) {
    server_clock_anchor_ms_ = client_->estimated_server_time_ms();
    local_clock_anchor_ms_ = monotonic_ms();
  }
  for (const auto& event : client_->poll(poll_timeout_ms)) {
    if (!handle_event(event, pipe, error)) return false;
  }
  return update_liveness(pipe, error);
}

bool NetworkAdapter::handle_event(const ht2mp::net::ClientEvent& event,
                                  PipeServer& pipe, std::string& error) {
  switch (event.kind) {
  case ht2mp::net::ClientEventKind::connected: {
    const auto new_session = client_->session_id();
    // ServerWelcome is the authoritative membership epoch. A reconnect can
    // return to the same coordinator process and therefore the same session
    // ID after its reliable PeerLeft was lost with the old transport. Keeping
    // any previous roster here would retain stale timelines and can exhaust
    // the seven-remote limit. Tear down every old actor before accepting the
    // membership stream that follows this welcome.
    for (auto& [unused, peer] : remotes_) {
      (void)unused;
      if (peer.spawned && !emit_despawn(peer, pipe, error)) return false;
    }
    remotes_.clear();
    session_id_ = new_session;
    last_environment_ms_.reset();
    server_clock_anchor_ms_ = client_->estimated_server_time_ms();
    local_clock_anchor_ms_ = monotonic_ms();
    reconnect_delay_ms_ = 1'000U;
    ever_connected_ = true;
    std::cout << "Connected to coordinator; session=" << session_id_
              << " player=" << client_->player_id() << '\n';
    if (options_.status_events) {
      std::cout << "HT2MP-EVENT/1 state=connected session=" << session_id_
                << " player=" << client_->player_id() << '\n' << std::flush;
    }
    break;
  }
  case ht2mp::net::ClientEventKind::message:
    if (event.message &&
        !handle_message(*event.message, event.received_at_us, pipe, error)) {
      return false;
    }
    break;
  case ht2mp::net::ClientEventKind::rejected:
    if (ht2mp::net::admission_rejection_is_retryable(event.reason,
                                                      ever_connected_)) {
      std::cerr << "coordinator has not released the previous membership yet: "
                << event.detail << "; reconnect scheduled\n";
      client_.reset();
      schedule_reconnect();
    } else {
      terminal_rejection_ = true;
      std::cerr << "coordinator rejected client: " << event.detail << '\n';
      if (options_.status_events) {
        std::cout << "HT2MP-EVENT/1 state=rejected detail=" << event.detail
                  << '\n' << std::flush;
      }
      {
        const ht2mp::ipc::EnterSafeModeV1 safe_mode{2U};
        if (!write_ipc(pipe, ht2mp::ipc::encode_enter_safe_mode, safe_mode,
                       error)) {
          return false;
        }
      }
    }
    break;
  case ht2mp::net::ClientEventKind::protocol_violation:
    terminal_rejection_ = true;
    std::cerr << "coordinator protocol violation: " << event.detail << '\n';
    {
      const ht2mp::ipc::EnterSafeModeV1 safe_mode{3U};
      if (!write_ipc(pipe, ht2mp::ipc::encode_enter_safe_mode, safe_mode,
                     error)) {
        return false;
      }
    }
    client_.reset();
    break;
  case ht2mp::net::ClientEventKind::disconnected:
    if (ht2mp::net::disconnect_is_terminal(event.reason)) {
      terminal_rejection_ = true;
      std::cerr << "coordinator terminated the session: " << event.detail
                << '\n';
      const ht2mp::ipc::EnterSafeModeV1 safe_mode{3U};
      if (!write_ipc(pipe, ht2mp::ipc::encode_enter_safe_mode, safe_mode,
                     error)) {
        return false;
      }
      client_.reset();
      break;
    }
    [[fallthrough]];
  case ht2mp::net::ClientEventKind::error:
    std::cerr << "network: " << event.detail << "; reconnect scheduled\n";
    if (options_.status_events) {
      std::cout << "HT2MP-EVENT/1 state=reconnecting detail=" << event.detail
                << '\n' << std::flush;
    }
    client_.reset();
    schedule_reconnect();
    break;
  }
  return true;
}

bool NetworkAdapter::handle_message(const ht2mp::protocol::Message& message,
                                    const std::uint64_t received_at_us,
                                    PipeServer& pipe, std::string& error) {
  if (const auto* joined = std::get_if<ht2mp::protocol::PeerJoined>(&message)) {
    if (const auto validation = ht2mp::protocol::validate(message, profile_limits_);
        !validation) {
      error = "Coordinator sent invalid peer metadata: " + validation.detail;
      return false;
    }
    if (!client_ || client_->state() != ht2mp::net::ClientState::connected ||
        session_id_ == 0U) {
      error = "Coordinator sent peer metadata outside an admitted session";
      return false;
    }
    if (joined->player_id == client_->player_id()) {
      error = "Coordinator advertised the local player as a remote peer";
      return false;
    }
    const auto duplicate_name = std::find_if(
        remotes_.begin(), remotes_.end(), [joined](const auto& item) {
          return item.first != joined->player_id && item.second.has_metadata &&
                 item.second.display_name == joined->display_name;
        });
    if (joined->display_name == options_.player_name ||
        duplicate_name != remotes_.end()) {
      error = "Coordinator advertised a duplicate peer name";
      return false;
    }
    auto iterator = remotes_.find(joined->player_id);
    if (iterator == remotes_.end()) {
      if (remotes_.size() >= kMaximumClientRemoteActors) {
        error = "Coordinator advertised more than seven remote peers";
        return false;
      }
      iterator = remotes_.try_emplace(joined->player_id).first;
    }
    auto& peer = iterator->second;
    if (peer.has_metadata && peer.incarnation_id == joined->incarnation_id &&
        (peer.vehicle_type != joined->vehicle_type ||
         peer.paint_variant != joined->paint_variant ||
         peer.display_name != joined->display_name)) {
      error = "Coordinator changed immutable peer metadata";
      return false;
    }
    if (peer.incarnation_id != 0U && peer.incarnation_id != joined->incarnation_id &&
        peer.spawned && !emit_despawn(peer, pipe, error)) {
      return false;
    }
    if (peer.incarnation_id != joined->incarnation_id) {
      peer.latest.reset();
      peer.has_sequence = false;
      peer.spawned = false;
      peer.hidden = false;
    }
    peer.player_id = joined->player_id;
    peer.incarnation_id = joined->incarnation_id;
    peer.vehicle_type = joined->vehicle_type;
    peer.paint_variant = joined->paint_variant;
    peer.display_name = joined->display_name;
    peer.has_metadata = true;
    return true;
  }
  if (const auto* spawn = std::get_if<ht2mp::protocol::SpawnMetadata>(&message)) {
    const ht2mp::protocol::PeerJoined joined{spawn->player_id, spawn->incarnation_id,
                                             spawn->vehicle_type, spawn->display_name,
                                             spawn->paint_variant};
    return handle_message(joined, received_at_us, pipe, error);
  }
  if (const auto* left = std::get_if<ht2mp::protocol::PeerLeft>(&message)) {
    if (!client_ || client_->state() != ht2mp::net::ClientState::connected ||
        left->player_id == client_->player_id()) {
      error = "Coordinator sent invalid PeerLeft membership data";
      return false;
    }
    const auto iterator = remotes_.find(left->player_id);
    if (iterator != remotes_.end() &&
        iterator->second.incarnation_id == left->incarnation_id) {
      if (iterator->second.spawned && !emit_despawn(iterator->second, pipe, error)) {
        return false;
      }
      remotes_.erase(iterator);
    }
    return true;
  }

  const auto accept_state = [&](const ht2mp::protocol::PlayerState& state,
                                const char* source) -> bool {
    if (const auto validation = ht2mp::protocol::validate(state, profile_limits_);
        !validation) {
      error = "Coordinator sent an invalid player state: " + validation.detail;
      return false;
    }
    if (!client_ || client_->state() != ht2mp::net::ClientState::connected ||
        session_id_ == 0U || state.session_id != session_id_) {
      error = "Coordinator sent state for a different or inactive session";
      return false;
    }
    if (state.player_id == client_->player_id()) {
      if (state.incarnation_id != client_->incarnation_id() ||
          state.vehicle_type != options_.vehicle_type ||
          state.paint_variant != options_.paint_variant) {
        error = "Coordinator reflected a conflicting local-player identity";
        return false;
      }
      return true;
    }
    const auto iterator = remotes_.find(state.player_id);
    // Reliable membership and state use separate ENet channels, so a valid
    // state can race ahead of PeerJoined. Drop it without allocating or
    // changing an existing actor; the next 20 Hz snapshot will recover it.
    if (iterator == remotes_.end()) return true;
    auto& peer = iterator->second;
    if (!peer.has_metadata || peer.incarnation_id != state.incarnation_id) {
      return true;
    }
    if (peer.vehicle_type != state.vehicle_type ||
        peer.paint_variant != state.paint_variant) {
      error = "Coordinator changed remote appearance without membership metadata";
      return false;
    }
    // Immediate fan-out and the 20 Hz snapshot both carry the same states;
    // the sequence decides which copy is new.
    if (peer.has_sequence &&
        !ht2mp::protocol::sequence_newer(state.sequence, peer.last_sequence)) {
      return true;
    }
    const auto stamp = receive_stamp_or_now(received_at_us);
    peer.last_sequence = state.sequence;
    peer.has_sequence = true;
    peer.last_receive_us = stamp;
    peer.latest = state;
    trace_remote(state, stamp, source);
    if (state.has(ht2mp::protocol::PlayerStateFlag::in_world)) {
      if (!peer.spawned && !emit_spawn(peer, pipe, error)) return false;
      if (!emit_sample(peer, state, stamp, pipe, error)) return false;
      peer.hidden = false;
    } else if (peer.spawned && !peer.hidden) {
      // The sender left the world (menu, loading, room mismatch): forward the
      // transition so the bridge removes the actor instead of freezing it.
      if (!emit_sample(peer, state, stamp, pipe, error)) return false;
      peer.hidden = true;
    }
    return true;
  };
  if (const auto* direct_state = std::get_if<ht2mp::protocol::PlayerState>(&message)) {
    if (!accept_state(*direct_state, "direct")) return false;
  } else if (const auto* snapshot = std::get_if<ht2mp::protocol::WorldSnapshot>(&message)) {
    if (snapshot->environment.session_id != 0U) {
      if (!client_ || client_->state() != ht2mp::net::ClientState::connected ||
          snapshot->environment.session_id != session_id_ || !ht2mp::protocol::valid_environment(snapshot->environment)) {
        error = "Coordinator sent an invalid environment/session";
        return false;
      }
      if (environment_enabled_ && (!last_environment_ms_ || snapshot->server_time_ms > *last_environment_ms_)) {
        ht2mp::ipc::EnvironmentV1 environment{snapshot->server_time_ms,
            receive_stamp_or_now(received_at_us), snapshot->environment};
        if (!write_ipc(pipe, ht2mp::ipc::encode_environment, environment, error)) return false;
        last_environment_ms_ = snapshot->server_time_ms;
      }
    }
    for (const auto& state : snapshot->players) {
      if (!accept_state(state, "snapshot")) return false;
    }
  }
  return true;
}

bool NetworkAdapter::handle_bridge_frame(const ReceivedFrame& frame,
                                         std::string& error) {
  error.clear();
  if (frame.header.type != ht2mp::ipc::MessageType::local_sample) return true;
  if (!enabled()) return true;
  ht2mp::ipc::PlayerSampleV1 sample;
  if (ht2mp::ipc::decode_player_sample(frame.payload(), sample) !=
      ht2mp::ipc::CodecError::none) {
    error = "Bridge emitted a malformed LocalSample";
    return false;
  }
  if ((sample.flags & 1U) != 0U) {
    if (!vehicle_type_ready_) {
      const ht2mp::protocol::ClientHello probe{
          std::string(options_.edition.profile_id), token_,
          options_.player_name, 1U, sample.vehicle_type, sample.paint_variant};
      if (const auto validation =
              ht2mp::protocol::validate(probe, profile_limits_);
          !validation) {
        error = "Bridge reported an invalid local vehicle model: " +
                validation.detail;
        return false;
      }
      if (options_.vehicle_type != kDiscoverVehicleType &&
          (sample.vehicle_type != options_.vehicle_type ||
           sample.paint_variant != options_.paint_variant)) {
        error = "Loaded vehicle appearance does not match launcher selection";
        if (options_.status_events) {
          std::cout << "HT2MP-EVENT/1 state=appearance-mismatch expected_vehicle="
                    << options_.vehicle_type << " actual_vehicle=" << sample.vehicle_type
                    << " expected_paint=" << static_cast<unsigned>(options_.paint_variant)
                    << " actual_paint=" << static_cast<unsigned>(sample.paint_variant)
                    << '\n' << std::flush;
        }
        return false;
      }
      options_.vehicle_type = sample.vehicle_type;
      options_.paint_variant = sample.paint_variant;
      vehicle_type_ready_ = true;
      next_reconnect_ms_ = 0U;
      std::cout << "Local vehicle model selector: "
                << options_.vehicle_type << ", paint "
                << static_cast<unsigned>(options_.paint_variant) << '\n';
      if (options_.status_events) {
        std::cout << "HT2MP-EVENT/1 state=appearance-ready vehicle="
                  << options_.vehicle_type << " paint="
                  << static_cast<unsigned>(options_.paint_variant) << '\n'
                  << std::flush;
      }
    } else if (sample.vehicle_type != options_.vehicle_type ||
               sample.paint_variant != options_.paint_variant) {
      error = "Local vehicle appearance changed during an admitted session";
      return false;
    }
  }
  trace_local(sample, receive_stamp_or_now(0U));
  if (!client_ || client_->state() != ht2mp::net::ClientState::connected) {
    return true;
  }
  auto state = to_network(sample);
  if (!client_->send_state(std::move(state), error)) {
    // A concurrent transport loss is recoverable; pump() will schedule it.
    if (client_->state() != ht2mp::net::ClientState::connected) {
      error.clear();
      return true;
    }
    return false;
  }
  return true;
}

bool NetworkAdapter::emit_spawn(RemotePeer& peer, PipeServer& pipe,
                                std::string& error) {
  if (!remote_actor_commands_enabled_) {
    return true;
  }
  ht2mp::ipc::SpawnRemoteV1 spawn;
  spawn.player_id = peer.player_id;
  spawn.incarnation_id = peer.incarnation_id;
  spawn.vehicle_type = peer.vehicle_type;
  spawn.paint_variant = peer.paint_variant;
  const auto size = std::min(peer.display_name.size(), spawn.display_name.size() - 1U);
  std::memcpy(spawn.display_name.data(), peer.display_name.data(), size);
  spawn.display_name[size] = '\0';
  if (!write_ipc(pipe, ht2mp::ipc::encode_spawn_remote, spawn, error)) return false;
  peer.spawned = true;
  peer.hidden = false;
  return true;
}

bool NetworkAdapter::emit_sample(RemotePeer& peer,
                                 const ht2mp::protocol::PlayerState& state,
                                 const std::uint64_t receive_time_us,
                                 PipeServer& pipe, std::string& error) {
  static_cast<void>(peer);
  if (!remote_actor_commands_enabled_) {
    return true;
  }
  // Forward the authoritative sample as-is: network sequence for the bridge's
  // stale check, sender capture time for its clock estimate, and the local
  // arrival stamp for latency accounting.
  auto sample = to_ipc(state);
  sample.receive_time_us = receive_time_us;
  return write_ipc(pipe, ht2mp::ipc::encode_remote_sample, sample, error);
}

bool NetworkAdapter::emit_despawn(RemotePeer& peer, PipeServer& pipe,
                                  std::string& error) {
  if (remote_actor_commands_enabled_) {
    const ht2mp::ipc::DespawnRemoteV1 despawn{peer.player_id, peer.incarnation_id};
    if (!write_ipc(pipe, ht2mp::ipc::encode_despawn_remote, despawn, error)) return false;
  }
  peer.spawned = false;
  peer.hidden = false;
  return true;
}

bool NetworkAdapter::update_liveness(PipeServer& pipe, std::string& error) {
  // Liveness is judged by local receive age only: sample_time_ms belongs to
  // the sender's clock. A silent peer keeps its last pose (the bridge holds
  // it) until the despawn threshold removes the actor.
  const auto now = receive_stamp_or_now(0U);
  for (auto& [unused, peer] : remotes_) {
    (void)unused;
    if (!peer.has_metadata || !peer.latest) continue;
    const auto age = now > peer.last_receive_us ? now - peer.last_receive_us : 0U;
    if (age >= kDespawnAfterUs) {
      if (peer.spawned && !emit_despawn(peer, pipe, error)) return false;
      peer.latest.reset();
      peer.has_sequence = false;
      peer.hidden = false;
    } else if (age >= kHideAfterUs && peer.spawned && !peer.hidden) {
      // Nothing to forward: the bridge's own hold already froze the pose.
      peer.hidden = true;
    }
  }
  return true;
}

void NetworkAdapter::stop(PipeServer& pipe) noexcept {
  if (local_trace_) local_trace_.flush();
  if (remote_trace_) remote_trace_.flush();
  if (client_) {
    client_->disconnect(ht2mp::protocol::DisconnectReason::normal,
                        "sidecar stopped");
  }
  std::string ignored;
  for (auto& [unused, peer] : remotes_) {
    (void)unused;
    if (peer.spawned) (void)emit_despawn(peer, pipe, ignored);
  }
  remotes_.clear();
  client_.reset();
}

} // namespace ht2mp::client
