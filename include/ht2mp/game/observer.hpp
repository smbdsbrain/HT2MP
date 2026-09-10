#pragma once

#include "ht2mp/game/profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace ht2mp::game {

class ReadOnlyMemory {
 public:
  virtual ~ReadOnlyMemory() = default;
  virtual bool Read(std::uintptr_t address, std::span<std::byte> destination,
                    std::string& error) noexcept = 0;

  // Allocation-free read for callbacks that execute on a game's hot path.
  // Implementations must not format diagnostics, allocate, wait, or throw.
  virtual bool ReadFast(std::uintptr_t address,
                        std::span<std::byte> destination) noexcept = 0;
};

struct ObservedVec3 {
  double x{};
  double y{};
  double z{};
};

struct ObservedQuaternion {
  float x{};
  float y{};
  float z{};
  float w{1.0F};
};

struct LocalTransformSample {
  std::uint64_t sequence{};
  std::uint64_t monotonic_time_us{};
  ObservedVec3 position;
  ObservedQuaternion orientation;
  ObservedVec3 linear_velocity;
  // Body-frame angular velocity (rad/s) derived from consecutive
  // orientations; consumers extrapolate with q ⊗ delta(angular_velocity·dt).
  ObservedVec3 angular_velocity;
  std::array<float, 9> raw_orientation{};
  bool velocity_valid{};
  bool angular_velocity_valid{};
  bool teleport{};
};

// Layout for a contiguous rigid-transform snapshot. It is deliberately free
// of symbol names so the same allocation-free decoder can validate both the
// legacy render anchor and a VehicleInstance-owned transform.
struct RigidTransformLayout {
  std::uint32_t orientation_offset{};
  std::uint32_t position_offset{};
  std::uint32_t read_size{};
  float maximum_absolute_position{};
  float basis_length_tolerance{};
  float basis_dot_tolerance{};
};

enum class ObservationStatus : std::uint8_t {
  sample,
  not_ready,
  read_failed,
  invalid_transform,
};

struct ObservationResult {
  ObservationStatus status{ObservationStatus::not_ready};
  LocalTransformSample sample;
  std::string_view detail;

  [[nodiscard]] bool has_sample() const noexcept {
    return status == ObservationStatus::sample;
  }
};

// The compact result used by game-thread hooks. PollFast and ReadFast are a
// deliberately separate contract so an error path cannot allocate while the
// game is inside its frame callback.
struct FastObservationResult {
  ObservationStatus status{ObservationStatus::not_ready};
  LocalTransformSample sample;

  [[nodiscard]] bool has_sample() const noexcept {
    return status == ObservationStatus::sample;
  }
};

static_assert(std::is_trivially_copyable_v<FastObservationResult>);

// Decodes one already-read transform block. It performs no I/O, allocation or
// formatting and leaves timing/history fields untouched.
[[nodiscard]] ObservationStatus DecodeRigidTransformFast(
    std::span<const std::byte> bytes, const RigidTransformLayout& layout,
    LocalTransformSample& sample) noexcept;

// Reads one contiguous transform snapshot and never writes or calls into the
// game. Create() requires a fully accepted profile verification report.
class LocalTransformObserver {
 public:
  static std::unique_ptr<LocalTransformObserver> Create(
      const ProfileVerification& verified, std::uintptr_t module_base,
      ReadOnlyMemory& memory, std::string& error);

  [[nodiscard]] ObservationResult Poll(std::uint64_t monotonic_time_us) noexcept;
  [[nodiscard]] FastObservationResult PollFast(
      std::uint64_t monotonic_time_us) noexcept;
  void ResetHistory() noexcept;

  [[nodiscard]] std::uintptr_t transform_address() const noexcept {
    return transform_address_;
  }

 private:
  LocalTransformObserver(std::uintptr_t transform_address, ObserverLayout layout,
                         ReadOnlyMemory& memory) noexcept;

  std::uintptr_t transform_address_{};
  ObserverLayout layout_{};
  ReadOnlyMemory* memory_{};
  std::uint64_t sequence_{};
  std::uint64_t previous_time_us_{};
  ObservedVec3 previous_position_{};
  bool have_previous_{};
};

[[nodiscard]] std::string_view ToString(ObservationStatus value) noexcept;

}  // namespace ht2mp::game
