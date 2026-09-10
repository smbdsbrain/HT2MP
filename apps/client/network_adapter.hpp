#pragma once

#include "edition.hpp"
#include "pipe_server.hpp"

#include "ht2mp/net/client.hpp"
#include "ht2mp/protocol/interpolation.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace ht2mp::client {

// A staged exact-Steam client learns the native model-registry selector from
// its first world-ready bridge sample before sending ClientHello.
inline constexpr std::uint16_t kDiscoverVehicleType = 0xffffU;
// The network session may contain 64 players, while the currently validated
// game bridge still owns only seven remote actor slots. Keep that boundary
// explicit and fail closed until the bridge capacity is deliberately raised.
inline constexpr std::size_t kMaximumClientRemoteActors = 7U;

struct NetworkOptions final {
  EditionDescriptor edition{};
  std::string endpoint;
  std::string token;
  std::string player_name;
  std::uint16_t vehicle_type{};
  std::uint8_t paint_variant{};
  bool status_events{};
  // Optional directory for network trace CSVs (local_samples.csv and
  // remote_states.csv). Empty disables tracing.
  std::filesystem::path trace_dir;
};

class NetworkAdapter final {
public:
  NetworkAdapter(NetworkOptions options, std::uint32_t bridge_capabilities,
                 ht2mp::ipc::BridgeMode bridge_mode,
                 bool profile_remote_writes_validated);
  ~NetworkAdapter();
  NetworkAdapter(const NetworkAdapter&) = delete;
  NetworkAdapter& operator=(const NetworkAdapter&) = delete;

  [[nodiscard]] bool start(std::string& error);
  // poll_timeout_ms blocks inside the socket layer so a datagram wakes the
  // sidecar immediately; zero keeps the historical non-blocking behaviour.
  [[nodiscard]] bool pump(PipeServer& pipe, std::string& error,
                          std::uint32_t poll_timeout_ms = 0U);
  [[nodiscard]] bool handle_bridge_frame(
      const ReceivedFrame& frame,
      std::string& error);
  void stop(PipeServer& pipe) noexcept;
  [[nodiscard]] bool enabled() const noexcept;

private:
  struct RemotePeer;

  [[nodiscard]] bool connect_new(std::string& error);
  [[nodiscard]] bool handle_event(
      const ht2mp::net::ClientEvent& event,
      PipeServer& pipe,
      std::string& error);
  [[nodiscard]] bool handle_message(
      const ht2mp::protocol::Message& message,
      std::uint64_t received_at_us,
      PipeServer& pipe,
      std::string& error);
  [[nodiscard]] bool update_liveness(PipeServer& pipe, std::string& error);
  [[nodiscard]] bool emit_spawn(RemotePeer& peer, PipeServer& pipe,
                                std::string& error);
  [[nodiscard]] bool emit_sample(RemotePeer& peer,
                                 const ht2mp::protocol::PlayerState& state,
                                 std::uint64_t receive_time_us,
                                 PipeServer& pipe, std::string& error);
  void trace_remote(const ht2mp::protocol::PlayerState& state,
                    std::uint64_t received_at_us, const char* source);
  void trace_local(const ht2mp::ipc::PlayerSampleV1& sample,
                   std::uint64_t received_at_us);
  [[nodiscard]] bool emit_despawn(RemotePeer& peer, PipeServer& pipe,
                                  std::string& error);
  void schedule_reconnect() noexcept;

  NetworkOptions options_;
  bool vehicle_type_ready_{};
  bool remote_actor_commands_enabled_{};
  bool environment_enabled_{};
  std::optional<std::uint64_t> last_environment_ms_;
  ht2mp::protocol::ProfileLimits profile_limits_{};
  std::string host_;
  std::uint16_t port_{28020U};
  ht2mp::protocol::Token128 token_{};
  std::unique_ptr<ht2mp::net::Client> client_;
  std::unordered_map<std::uint64_t, RemotePeer> remotes_;
  std::uint64_t session_id_{};
  std::uint64_t server_clock_anchor_ms_{};
  std::uint64_t local_clock_anchor_ms_{};
  std::uint64_t next_reconnect_ms_{};
  std::uint32_t reconnect_delay_ms_{1000U};
  bool ever_connected_{};
  bool terminal_rejection_{};
  std::ofstream local_trace_;
  std::ofstream remote_trace_;
  std::uint32_t trace_rows_{};
};

} // namespace ht2mp::client
