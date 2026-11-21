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

/// @brief quatFromEuler() with angles in degrees.
inline glm::quat quatFromEulerDegrees(const glm::vec3& degrees, EulerOrder order = kDefaultEulerOrder) {
    return quatFromEuler(glm::radians(degrees), order);
}
/// @brief eulerFromQuat() in degrees.
inline glm::vec3 eulerDegreesFromQuat(const glm::quat& q, EulerOrder order = kDefaultEulerOrder) {
    return glm::degrees(eulerFromQuat(q, order));
}

} // namespace nova
