#include "ht2mp/game/observer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace ht2mp::game {
namespace {

float ReadFloat(std::span<const std::byte> bytes, std::size_t offset) noexcept {
  float value = 0.0F;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

double LengthSquared(double x, double y, double z) noexcept {
  return x * x + y * y + z * z;
}

ObservedQuaternion MatrixToQuaternion(const std::array<float, 9>& matrix) noexcept {
  ObservedQuaternion quaternion;
  const auto trace = matrix[0] + matrix[4] + matrix[8];
  if (trace > 0.0F) {
    const auto scale = std::sqrt(trace + 1.0F) * 2.0F;
    quaternion.w = 0.25F * scale;
    quaternion.x = (matrix[7] - matrix[5]) / scale;
    quaternion.y = (matrix[2] - matrix[6]) / scale;
    quaternion.z = (matrix[3] - matrix[1]) / scale;
  } else if (matrix[0] > matrix[4] && matrix[0] > matrix[8]) {
    const auto scale =
        std::sqrt(1.0F + matrix[0] - matrix[4] - matrix[8]) * 2.0F;
    quaternion.w = (matrix[7] - matrix[5]) / scale;
    quaternion.x = 0.25F * scale;
    quaternion.y = (matrix[1] + matrix[3]) / scale;
    quaternion.z = (matrix[2] + matrix[6]) / scale;
  } else if (matrix[4] > matrix[8]) {
    const auto scale =
        std::sqrt(1.0F + matrix[4] - matrix[0] - matrix[8]) * 2.0F;
    quaternion.w = (matrix[2] - matrix[6]) / scale;
    quaternion.x = (matrix[1] + matrix[3]) / scale;
    quaternion.y = 0.25F * scale;
    quaternion.z = (matrix[5] + matrix[7]) / scale;
  } else {
    const auto scale =
        std::sqrt(1.0F + matrix[8] - matrix[0] - matrix[4]) * 2.0F;
    quaternion.w = (matrix[3] - matrix[1]) / scale;
    quaternion.x = (matrix[2] + matrix[6]) / scale;
    quaternion.y = (matrix[5] + matrix[7]) / scale;
    quaternion.z = 0.25F * scale;
  }
  const auto norm = std::sqrt(quaternion.x * quaternion.x +
                              quaternion.y * quaternion.y +
                              quaternion.z * quaternion.z +
                              quaternion.w * quaternion.w);
  if (std::isfinite(norm) && norm > 0.0001F) {
    quaternion.x /= norm;
    quaternion.y /= norm;
    quaternion.z /= norm;
    quaternion.w /= norm;
  } else {
    quaternion = {};
  }
  // q and -q encode the same rotation. A stable hemisphere avoids unnecessary
  // sign flips before the network interpolation layer sees the sample.
  if (quaternion.w < 0.0F) {
    quaternion.x = -quaternion.x;
    quaternion.y = -quaternion.y;
    quaternion.z = -quaternion.z;
    quaternion.w = -quaternion.w;
  }
  return quaternion;
}

enum class MatrixValidation : std::uint8_t {
  valid,
  not_initialized,
  invalid,
};

MatrixValidation ValidateMatrix(const std::array<float, 9>& matrix,
                                const RigidTransformLayout& layout) noexcept {
  for (const auto component : matrix) {
    if (!std::isfinite(component)) {
      return MatrixValidation::invalid;
    }
  }
  const auto largest = std::max_element(
      matrix.begin(), matrix.end(),
      [](float left, float right) { return std::abs(left) < std::abs(right); });
  if (largest == matrix.end() || std::abs(*largest) < 0.0001F) {
    return MatrixValidation::not_initialized;
  }

  const auto tolerance = static_cast<double>(layout.basis_length_tolerance);
  for (std::size_t row = 0; row < 3U; ++row) {
    const auto offset = row * 3U;
    const auto length = std::sqrt(LengthSquared(matrix[offset], matrix[offset + 1U],
                                                matrix[offset + 2U]));
    if (std::abs(length - 1.0) > tolerance) {
      return MatrixValidation::invalid;
    }
  }
  for (std::size_t left = 0; left < 3U; ++left) {
    for (std::size_t right = left + 1U; right < 3U; ++right) {
      const auto a = left * 3U;
      const auto b = right * 3U;
      const auto dot = static_cast<double>(matrix[a]) * matrix[b] +
                       static_cast<double>(matrix[a + 1U]) * matrix[b + 1U] +
                       static_cast<double>(matrix[a + 2U]) * matrix[b + 2U];
      if (std::abs(dot) > layout.basis_dot_tolerance) {
        return MatrixValidation::invalid;
      }
    }
  }
  const auto determinant =
      static_cast<double>(matrix[0]) *
          (static_cast<double>(matrix[4]) * matrix[8] -
           static_cast<double>(matrix[5]) * matrix[7]) -
      static_cast<double>(matrix[1]) *
          (static_cast<double>(matrix[3]) * matrix[8] -
           static_cast<double>(matrix[5]) * matrix[6]) +
      static_cast<double>(matrix[2]) *
          (static_cast<double>(matrix[3]) * matrix[7] -
           static_cast<double>(matrix[4]) * matrix[6]);
  if (!std::isfinite(determinant) || determinant < 0.25 || determinant > 1.75) {
    return MatrixValidation::invalid;
  }
  return MatrixValidation::valid;
}

}  // namespace

ObservationStatus DecodeRigidTransformFast(
    const std::span<const std::byte> bytes,
    const RigidTransformLayout& layout,
    LocalTransformSample& sample) noexcept {
  if (layout.read_size == 0U || layout.read_size > bytes.size() ||
      layout.orientation_offset > layout.read_size ||
      layout.read_size - layout.orientation_offset < 9U * sizeof(float) ||
      layout.position_offset > layout.read_size ||
      layout.read_size - layout.position_offset < 3U * sizeof(float) ||
      layout.maximum_absolute_position <= 0.0F ||
      layout.basis_length_tolerance <= 0.0F ||
      layout.basis_dot_tolerance <= 0.0F) {
    return ObservationStatus::invalid_transform;
  }
  for (std::size_t index = 0; index < sample.raw_orientation.size(); ++index) {
    sample.raw_orientation[index] =
        ReadFloat(bytes, layout.orientation_offset + index * sizeof(float));
  }
  const auto position_x = ReadFloat(bytes, layout.position_offset);
  const auto position_y =
      ReadFloat(bytes, layout.position_offset + sizeof(float));
  const auto position_z =
      ReadFloat(bytes, layout.position_offset + 2U * sizeof(float));
  if (!std::isfinite(position_x) || !std::isfinite(position_y) ||
      !std::isfinite(position_z)) {
    return ObservationStatus::invalid_transform;
  }
  const auto maximum = static_cast<double>(layout.maximum_absolute_position);
  if (std::abs(static_cast<double>(position_x)) > maximum ||
      std::abs(static_cast<double>(position_y)) > maximum ||
      std::abs(static_cast<double>(position_z)) > maximum) {
    return ObservationStatus::invalid_transform;
  }
  const auto matrix_validation =
      ValidateMatrix(sample.raw_orientation, layout);
  if (matrix_validation != MatrixValidation::valid) {
    return matrix_validation == MatrixValidation::not_initialized
               ? ObservationStatus::not_ready
               : ObservationStatus::invalid_transform;
  }
  sample.position = {position_x, position_y, position_z};
  sample.orientation = MatrixToQuaternion(sample.raw_orientation);
  return ObservationStatus::sample;
}

LocalTransformObserver::LocalTransformObserver(std::uintptr_t transform_address,
                                               ObserverLayout layout,
                                               ReadOnlyMemory& memory) noexcept
    : transform_address_(transform_address), layout_(layout), memory_(&memory) {}

std::unique_ptr<LocalTransformObserver> LocalTransformObserver::Create(
    const ProfileVerification& verified, std::uintptr_t module_base,
    ReadOnlyMemory& memory, std::string& error) {
  error.clear();
  if (!verified.accepted()) {
    error = "game profile has not passed fail-closed verification";
    return nullptr;
  }
  if (!verified.profile->observer.enabled) {
    error = "local transform observer is disabled for this profile";
    return nullptr;
  }
  const auto* symbol =
      verified.FindSymbol(verified.profile->observer.transform_symbol);
  if (symbol == nullptr || !symbol->accepted) {
    error = "verified transform symbol is unavailable";
    return nullptr;
  }
  if (symbol->result_rva >
      std::numeric_limits<std::uintptr_t>::max() - module_base) {
    error = "runtime transform address overflows uintptr_t";
    return nullptr;
  }
  const auto& layout = verified.profile->observer;
  if (layout.read_size == 0U || layout.read_size > 256U ||
      layout.orientation_offset > layout.read_size ||
      layout.read_size - layout.orientation_offset < 9U * sizeof(float) ||
      layout.position_offset > layout.read_size ||
      layout.read_size - layout.position_offset < 3U * sizeof(float)) {
    error = "profile contains an invalid observer layout";
    return nullptr;
  }
  return std::unique_ptr<LocalTransformObserver>(new LocalTransformObserver(
      module_base + symbol->result_rva, layout, memory));
}

ObservationResult LocalTransformObserver::Poll(
    std::uint64_t monotonic_time_us) noexcept {
  const auto fast = PollFast(monotonic_time_us);
  ObservationResult result;
  result.status = fast.status;
  result.sample = fast.sample;
  switch (fast.status) {
    case ObservationStatus::sample:
      result.detail = "read-only render transform";
      break;
    case ObservationStatus::not_ready:
      result.detail = "transform has not been initialized";
      break;
    case ObservationStatus::read_failed:
      result.detail = "read-only memory read failed";
      break;
    case ObservationStatus::invalid_transform:
      result.detail = "transform failed profile validation";
      break;
  }
  return result;
}

FastObservationResult LocalTransformObserver::PollFast(
    std::uint64_t monotonic_time_us) noexcept {
  FastObservationResult result;
  std::array<std::byte, 256> storage{};
  const auto requested =
      std::span<std::byte>(storage).first(static_cast<std::size_t>(layout_.read_size));
  if (!memory_->ReadFast(transform_address_, requested)) {
    result.status = ObservationStatus::read_failed;
    ResetHistory();
    return result;
  }
  const auto bytes = std::span<const std::byte>(requested);
  const RigidTransformLayout rigid_layout{
      layout_.orientation_offset,
      layout_.position_offset,
      layout_.read_size,
      layout_.maximum_absolute_position,
      layout_.basis_length_tolerance,
      layout_.basis_dot_tolerance,
  };
  result.status =
      DecodeRigidTransformFast(bytes, rigid_layout, result.sample);
  if (result.status != ObservationStatus::sample) {
    ResetHistory();
    return result;
  }

  result.sample.sequence = ++sequence_;
  result.sample.monotonic_time_us = monotonic_time_us;
  if (have_previous_ && monotonic_time_us > previous_time_us_) {
    const auto elapsed_us = monotonic_time_us - previous_time_us_;
    const auto elapsed_seconds = static_cast<double>(elapsed_us) / 1'000'000.0;
    const auto dx = result.sample.position.x - previous_position_.x;
    const auto dy = result.sample.position.y - previous_position_.y;
    const auto dz = result.sample.position.z - previous_position_.z;
    const auto distance = std::sqrt(LengthSquared(dx, dy, dz));
    result.sample.teleport = distance > 250.0;
    if (elapsed_seconds >= 0.001 && elapsed_seconds <= 1.0 &&
        !result.sample.teleport) {
      result.sample.linear_velocity = {dx / elapsed_seconds, dy / elapsed_seconds,
                                       dz / elapsed_seconds};
      result.sample.velocity_valid = true;
    }
  }
  previous_time_us_ = monotonic_time_us;
  previous_position_ = result.sample.position;
  have_previous_ = true;
  result.status = ObservationStatus::sample;
  return result;
}

void LocalTransformObserver::ResetHistory() noexcept {
  previous_time_us_ = 0U;
  previous_position_ = {};
  have_previous_ = false;
}

std::string_view ToString(ObservationStatus value) noexcept {
  switch (value) {
    case ObservationStatus::sample:
      return "sample";
    case ObservationStatus::not_ready:
      return "not-ready";
    case ObservationStatus::read_failed:
      return "read-failed";
    case ObservationStatus::invalid_transform:
      return "invalid-transform";
  }
  return "unknown";
}

}  // namespace ht2mp::game
