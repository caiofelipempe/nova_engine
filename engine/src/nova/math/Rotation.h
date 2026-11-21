#pragma once

/// @file
/// @brief Euler angles ↔ quaternions in any of the six axis orders.
///
/// Rotations are stored as quaternions (TransformComponent::rotation); Euler angles are a view for
/// people. Angles are always given per axis — `x` = pitch (around X), `y` = yaw (around Y),
/// `z` = roll (around Z) — and the EulerOrder says in which order they are applied.

#include "nova/core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace nova {

/// @brief Order in which the three axis rotations are applied, around the object's **local** axes
///        (intrinsic): `YXZ` = yaw first, then pitch around the new X, then roll around the new Z.
///
/// Equivalently, the quaternion is the product in that order: `YXZ` → `qY * qX * qZ`.
/// Reading the letters backwards gives the same rotation around the fixed world axes (extrinsic).
enum class EulerOrder : u8 {
    XYZ,
    XZY,
    YXZ, ///< Yaw → pitch → roll: cameras and characters (the engine default; same as Unity).
    YZX,
    ZXY,
    ZYX, ///< Roll → yaw → pitch (local); what `glm::quat(vec3)` and older Nova scene files use.
};

/// @brief Default order of the engine's Euler helpers.
constexpr EulerOrder kDefaultEulerOrder = EulerOrder::YXZ;

/// @brief Name of an order ("YXZ").
const char* eulerOrderName(EulerOrder order);

/// @brief Quaternion from Euler angles in radians (`x` pitch, `y` yaw, `z` roll) applied in `order`.
glm::quat quatFromEuler(const glm::vec3& radians, EulerOrder order = kDefaultEulerOrder);
/// @brief Euler angles in radians that rebuild `q` in `order`.
///
/// The middle rotation lies in [−90°, 90°], the others in (−180°, 180°]. At the singularity
/// (middle angle ±90°, "gimbal lock") the last angle is reported as 0.
glm::vec3 eulerFromQuat(const glm::quat& q, EulerOrder order = kDefaultEulerOrder);

/// @name Unit quaternions
/// Rotations must be unit quaternions (a non-unit one also scales). Products of unit quaternions
/// drift from length 1 only by rounding, so they are fixed lazily and cheaply.
/// @{
/// @brief Brings `q` back to unit length if it drifted: one dot product when it is already unit;
///        a Newton step (no square root) for small drift; a full normalization otherwise.
///        A zero quaternion becomes the identity.
inline glm::quat renormalize(const glm::quat& q) {
    const f32 d = glm::dot(q, q);
    const f32 drift = d - 1.0f;
    if (drift > -1e-6f && drift < 1e-6f) return q;                  // already unit (the common case)
    // 1/sqrt(d) ≈ (3 − d) / 2 near 1; error ≈ 3/8 · drift², under 4e-7 in this band. Rounding drift of
    // quaternion products is ~1e-7 per operation, so this is the path long chains of rotations take.
    if (drift > -1e-3f && drift < 1e-3f) return q * (1.5f - 0.5f * d);
    if (d < 1e-12f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    return q * (1.0f / std::sqrt(d));
}

/// @brief Spherical interpolation along the shortest arc: constant angular speed. `t` in [0, 1].
inline glm::quat slerp(const glm::quat& a, const glm::quat& b, f32 t) {
    glm::quat to = b;
    f32 cosAngle = glm::dot(a, b);
    if (cosAngle < 0.0f) { // q and −q are the same rotation: take the short way
        to = -b;
        cosAngle = -cosAngle;
    }
    if (cosAngle > 0.9995f) return renormalize(a + (to - a) * t); // nearly equal: nlerp is exact enough
    const f32 angle = std::acos(cosAngle);
    const f32 s = std::sin(angle);
    return renormalize(a * (std::sin((1.0f - t) * angle) / s) + to * (std::sin(t * angle) / s));
}

/// @brief Normalized linear interpolation along the shortest arc: cheaper than slerp, same path,
///        but not constant speed (fine for small steps, e.g. smoothing every frame).
inline glm::quat nlerp(const glm::quat& a, const glm::quat& b, f32 t) {
    const glm::quat to = glm::dot(a, b) < 0.0f ? -b : b;
    const glm::quat l = a + (to - a) * t; // may be as short as ~0.71: needs a real normalization
    const f32 d = glm::dot(l, l);
    return d < 1e-12f ? a : l * (1.0f / std::sqrt(d));
}

/// @brief Angle in radians between two rotations (0..π).
inline f32 angleBetween(const glm::quat& a, const glm::quat& b) {
    const f32 c = std::min(1.0f, std::abs(glm::dot(a, b)));
    return 2.0f * std::acos(c);
}

/// @brief Rotates `from` towards `to` by at most `maxAngle` radians (reaches `to` exactly when close).
inline glm::quat rotateTowards(const glm::quat& from, const glm::quat& to, f32 maxAngle) {
    const f32 angle = angleBetween(from, to);
    if (angle <= maxAngle || angle < 1e-7f) return to;
    return slerp(from, to, maxAngle / angle);
}
/// @}

/// @brief quatFromEuler() with angles in degrees.
inline glm::quat quatFromEulerDegrees(const glm::vec3& degrees, EulerOrder order = kDefaultEulerOrder) {
    return quatFromEuler(glm::radians(degrees), order);
}
/// @brief eulerFromQuat() in degrees.
inline glm::vec3 eulerDegreesFromQuat(const glm::quat& q, EulerOrder order = kDefaultEulerOrder) {
    return glm::degrees(eulerFromQuat(q, order));
}

} // namespace nova
