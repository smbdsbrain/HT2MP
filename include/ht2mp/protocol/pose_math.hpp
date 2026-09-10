#pragma once

// Header-only pose mathematics shared by the network interpolation layer, the
// game-side observers and the in-process bridge. It deliberately depends on
// <array>/<cmath>/<cstdint> only: the bridge is compiled with exceptions and
// RTTI disabled and must never pull std::string/std::deque onto the hooked
// game thread.
//
// Conventions (identical on every side of the wire):
// - Quaternions are {x, y, z, w}; quat_multiply(a, b) is the Hamilton product
//   a ⊗ b. A body-frame delta rotation is applied as q ⊗ delta, which is what
//   quat_extrapolate() does and what body_angular_velocity() inverts.
// - quat_to_matrix() and matrix_to_quaternion() round-trip the 3x3 row-major
//   basis stored inside the game's VehicleInstance/physics objects.

#include <array>
#include <cmath>
#include <cstdint>

namespace ht2mp::posemath {

using Vec3d = std::array<double, 3>;
using Vec3f = std::array<float, 3>;
using Quat = std::array<float, 4>;
using Mat3 = std::array<float, 9>;

inline constexpr Quat kIdentityQuat{0.0F, 0.0F, 0.0F, 1.0F};
inline constexpr Mat3 kIdentityMat3{1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                    0.0F, 0.0F, 0.0F, 1.0F};

[[nodiscard]] inline Vec3d hermite(const Vec3d& first, const Vec3f& first_velocity,
                                   const Vec3d& second, const Vec3f& second_velocity,
                                   const double alpha,
                                   const double duration_seconds) noexcept {
  const double a2 = alpha * alpha;
  const double a3 = a2 * alpha;
  const double h00 = 2.0 * a3 - 3.0 * a2 + 1.0;
  const double h10 = a3 - 2.0 * a2 + alpha;
  const double h01 = -2.0 * a3 + 3.0 * a2;
  const double h11 = a3 - a2;
  Vec3d result{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    result[axis] = h00 * first[axis] +
                   h10 * duration_seconds * first_velocity[axis] +
                   h01 * second[axis] +
                   h11 * duration_seconds * second_velocity[axis];
  }
  return result;
}

[[nodiscard]] inline Vec3f lerp(const Vec3f& first, const Vec3f& second,
                                const float alpha) noexcept {
  return {first[0] + (second[0] - first[0]) * alpha,
          first[1] + (second[1] - first[1]) * alpha,
          first[2] + (second[2] - first[2]) * alpha};
}

[[nodiscard]] inline float quat_dot(const Quat& a, const Quat& b) noexcept {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

[[nodiscard]] inline Quat quat_normalized(Quat value) noexcept {
  const float norm = std::sqrt(quat_dot(value, value));
  if (!(norm > 1.0e-6F) || !std::isfinite(norm)) return kIdentityQuat;
  for (auto& component : value) component /= norm;
  return value;
}

[[nodiscard]] inline Quat quat_conjugate(const Quat& value) noexcept {
  return {-value[0], -value[1], -value[2], value[3]};
}

// Hamilton product first ⊗ second (not normalized).
[[nodiscard]] inline Quat quat_multiply_raw(const Quat& a, const Quat& b) noexcept {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

[[nodiscard]] inline Quat quat_multiply(const Quat& a, const Quat& b) noexcept {
  return quat_normalized(quat_multiply_raw(a, b));
}

// Shortest-arc spherical interpolation with a linear fast path for nearly
// identical orientations.
[[nodiscard]] inline Quat quat_slerp(const Quat& first, Quat second,
                                     const float alpha) noexcept {
  float dot = quat_dot(first, second);
  if (dot < 0.0F) {
    for (auto& component : second) component = -component;
    dot = -dot;
  }
  if (dot > 1.0F) dot = 1.0F;
  if (dot > 0.9995F) {
    return quat_normalized({first[0] + alpha * (second[0] - first[0]),
                            first[1] + alpha * (second[1] - first[1]),
                            first[2] + alpha * (second[2] - first[2]),
                            first[3] + alpha * (second[3] - first[3])});
  }
  const float angle = std::acos(dot);
  const float sine = std::sin(angle);
  if (!(sine > 1.0e-6F)) return quat_normalized(first);
  const float first_weight = std::sin((1.0F - alpha) * angle) / sine;
  const float second_weight = std::sin(alpha * angle) / sine;
  return quat_normalized({first[0] * first_weight + second[0] * second_weight,
                          first[1] * first_weight + second[1] * second_weight,
                          first[2] * first_weight + second[2] * second_weight,
                          first[3] * first_weight + second[3] * second_weight});
}

// Rotation of |axis_angle| radians about axis_angle/|axis_angle|.
[[nodiscard]] inline Quat quat_from_rotation_vector(const Vec3f& rotation) noexcept {
  const double speed = std::sqrt(static_cast<double>(rotation[0]) * rotation[0] +
                                 static_cast<double>(rotation[1]) * rotation[1] +
                                 static_cast<double>(rotation[2]) * rotation[2]);
  if (speed < 1.0e-9) return kIdentityQuat;
  const double half = speed * 0.5;
  const double scale = std::sin(half) / speed;
  return quat_normalized({static_cast<float>(rotation[0] * scale),
                          static_cast<float>(rotation[1] * scale),
                          static_cast<float>(rotation[2] * scale),
                          static_cast<float>(std::cos(half))});
}

// Body-frame angular velocity applied for `seconds`: q ⊗ delta.
[[nodiscard]] inline Quat quat_extrapolate(const Quat& orientation,
                                           const Vec3f& angular_velocity,
                                           const double seconds) noexcept {
  const double speed = std::sqrt(static_cast<double>(angular_velocity[0]) * angular_velocity[0] +
                                 static_cast<double>(angular_velocity[1]) * angular_velocity[1] +
                                 static_cast<double>(angular_velocity[2]) * angular_velocity[2]);
  if (speed < 1.0e-7) return orientation;
  const Vec3f rotation{static_cast<float>(angular_velocity[0] * seconds),
                       static_cast<float>(angular_velocity[1] * seconds),
                       static_cast<float>(angular_velocity[2] * seconds)};
  return quat_multiply(orientation, quat_from_rotation_vector(rotation));
}

// Inverse of quat_extrapolate: the body-frame angular velocity that rotates
// `previous` into `current` over `seconds`. Returns false for degenerate
// inputs (dt <= 0, non-finite orientations).
[[nodiscard]] inline bool body_angular_velocity(const Quat& previous, Quat current,
                                                const double seconds,
                                                Vec3f& angular_velocity) noexcept {
  angular_velocity = {};
  if (!(seconds > 0.0) || !std::isfinite(seconds)) return false;
  for (const auto component : previous) {
    if (!std::isfinite(component)) return false;
  }
  for (const auto component : current) {
    if (!std::isfinite(component)) return false;
  }
  // q and -q are the same rotation; pick the hemisphere closest to previous
  // so the delta is the short way round.
  if (quat_dot(previous, current) < 0.0F) {
    for (auto& component : current) component = -component;
  }
  Quat delta = quat_multiply(quat_conjugate(quat_normalized(previous)),
                             quat_normalized(current));
  if (delta[3] < 0.0F) {
    for (auto& component : delta) component = -component;
  }
  const double sine = std::sqrt(static_cast<double>(delta[0]) * delta[0] +
                                static_cast<double>(delta[1]) * delta[1] +
                                static_cast<double>(delta[2]) * delta[2]);
  if (sine < 1.0e-9) return true;  // no rotation
  const double angle = 2.0 * std::atan2(sine, static_cast<double>(delta[3]));
  const double scale = angle / (sine * seconds);
  angular_velocity = {static_cast<float>(delta[0] * scale),
                      static_cast<float>(delta[1] * scale),
                      static_cast<float>(delta[2] * scale)};
  return true;
}

// Angle in radians between two orientations (0..pi).
[[nodiscard]] inline double quat_angle_between(const Quat& a, const Quat& b) noexcept {
  double dot = std::fabs(static_cast<double>(quat_dot(quat_normalized(a), quat_normalized(b))));
  if (dot > 1.0) dot = 1.0;
  return 2.0 * std::acos(dot);
}

// Row-major 3x3 basis matching the game's in-memory layout. Non-normalized or
// degenerate input yields the identity basis, never NaN.
[[nodiscard]] inline Mat3 quat_to_matrix(const Quat& input) noexcept {
  const double x = input[0];
  const double y = input[1];
  const double z = input[2];
  const double w = input[3];
  const double norm = std::sqrt(x * x + y * y + z * z + w * w);
  if (!std::isfinite(norm) || norm < 0.5) return kIdentityMat3;
  const double nx = x / norm;
  const double ny = y / norm;
  const double nz = z / norm;
  const double nw = w / norm;
  return {
      static_cast<float>(1.0 - 2.0 * (ny * ny + nz * nz)),
      static_cast<float>(2.0 * (nx * ny - nz * nw)),
      static_cast<float>(2.0 * (nx * nz + ny * nw)),
      static_cast<float>(2.0 * (nx * ny + nz * nw)),
      static_cast<float>(1.0 - 2.0 * (nx * nx + nz * nz)),
      static_cast<float>(2.0 * (ny * nz - nx * nw)),
      static_cast<float>(2.0 * (nx * nz - ny * nw)),
      static_cast<float>(2.0 * (ny * nz + nx * nw)),
      static_cast<float>(1.0 - 2.0 * (nx * nx + ny * ny)),
  };
}

[[nodiscard]] inline Mat3 transpose(const Mat3& m) noexcept {
  return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
}

// Shepperd's method for the row-major basis above, normalized and
// canonicalized to w >= 0 so the wire never flips sign needlessly.
[[nodiscard]] inline Quat matrix_to_quaternion(const Mat3& matrix) noexcept {
  Quat q{};
  const float trace = matrix[0] + matrix[4] + matrix[8];
  if (trace > 0.0F) {
    const float scale = std::sqrt(trace + 1.0F) * 2.0F;
    q[3] = 0.25F * scale;
    q[0] = (matrix[7] - matrix[5]) / scale;
    q[1] = (matrix[2] - matrix[6]) / scale;
    q[2] = (matrix[3] - matrix[1]) / scale;
  } else if (matrix[0] > matrix[4] && matrix[0] > matrix[8]) {
    const float scale = std::sqrt(1.0F + matrix[0] - matrix[4] - matrix[8]) * 2.0F;
    q[3] = (matrix[7] - matrix[5]) / scale;
    q[0] = 0.25F * scale;
    q[1] = (matrix[1] + matrix[3]) / scale;
    q[2] = (matrix[2] + matrix[6]) / scale;
  } else if (matrix[4] > matrix[8]) {
    const float scale = std::sqrt(1.0F + matrix[4] - matrix[0] - matrix[8]) * 2.0F;
    q[3] = (matrix[2] - matrix[6]) / scale;
    q[0] = (matrix[1] + matrix[3]) / scale;
    q[1] = 0.25F * scale;
    q[2] = (matrix[5] + matrix[7]) / scale;
  } else {
    const float scale = std::sqrt(1.0F + matrix[8] - matrix[0] - matrix[4]) * 2.0F;
    q[3] = (matrix[3] - matrix[1]) / scale;
    q[0] = (matrix[2] + matrix[6]) / scale;
    q[1] = (matrix[5] + matrix[7]) / scale;
    q[2] = 0.25F * scale;
  }
  const float norm = std::sqrt(quat_dot(q, q));
  if (!std::isfinite(norm) || norm <= 0.0001F) return kIdentityQuat;
  for (auto& component : q) component /= norm;
  if (q[3] < 0.0F) {
    for (auto& component : q) component = -component;
  }
  return q;
}

}  // namespace ht2mp::posemath
