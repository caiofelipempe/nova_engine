#pragma once

/// @file
/// @brief Built-in ECS components.
///
/// Components are plain aggregates: add them with `entity.add<T>(...)` using aggregate
/// initialization, e.g. `e.add<nova::PointLightComponent>(glm::vec3(1, 0.5f, 0.2f), 25.0f, 9.0f)`.

#include "nova/math/GlobalPosition.h"
#include "nova/math/Rotation.h"
#include "nova/renderer/Mesh.h"
#include "nova/scene/EntityRef.h"
#include "nova/scene/PhysicsComponents.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <string>

namespace nova {

class Script;

/// @brief Stable identity of an entity (every entity has one).
///
/// Unlike the EnTT handle, the UUID survives saving/loading and editor Play/Stop, so it is what
/// tools and files use to refer to an entity.
struct IdComponent {
    u64 uuid = 0; ///< Random 64-bit id assigned by Scene::createEntity (preserved by serialization).
};

/// @brief Display name of an entity (every entity has one).
struct NameComponent {
    std::string name; ///< Name shown in the editor; also used by Scene::findEntity.
};

/// @brief Position, rotation and scale of an entity (every entity has one).
///
/// Rotation is a quaternion (no gimbal lock, exact round trips with physics); read or set it as
/// Euler angles in any axis order with euler()/setEuler() (see EulerOrder).
/// The entity's local axes: right = +X, up = +Y, forward = −Z.
struct TransformComponent {
    /// @brief World position: exact integer fixed point (see WorldSpace). Assign meters directly
    ///        (`position = {0, 3, 10}`), move with offsets (`position += velocity * dt`).
    GlobalPosition position;
    /// @brief World rotation, a **unit** quaternion. The helpers below keep it unit; when assigning
    ///        one computed elsewhere, prefer setRotation() (it renormalizes cheaply).
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};                      ///< Per-axis scale (also scales colliders).

    /// @brief The rotation (same as `rotation`; kept for readability at call sites).
    glm::quat orientation() const { return rotation; }
    /// @brief Sets the rotation, bringing it back to unit length if needed (cheap when it already is).
    void setRotation(const glm::quat& q) { rotation = renormalize(q); }

    /// @name Euler angles (a view of `rotation`)
    /// `x` = pitch (around X), `y` = yaw (around Y), `z` = roll (around Z), applied in `order`
    /// around the local axes (default YXZ: yaw → pitch → roll).
    /// @{
    /// @brief Euler angles in radians.
    glm::vec3 euler(EulerOrder order = kDefaultEulerOrder) const { return eulerFromQuat(rotation, order); }
    /// @brief Euler angles in degrees.
    glm::vec3 eulerDegrees(EulerOrder order = kDefaultEulerOrder) const { return eulerDegreesFromQuat(rotation, order); }
    /// @brief Sets the rotation from Euler angles in radians.
    void setEuler(const glm::vec3& radians, EulerOrder order = kDefaultEulerOrder) { rotation = quatFromEuler(radians, order); }
    /// @brief Sets the rotation from Euler angles in degrees.
    void setEulerDegrees(const glm::vec3& degrees, EulerOrder order = kDefaultEulerOrder) {
        rotation = quatFromEulerDegrees(degrees, order);
    }
    /// @}

    /// @brief Rotates around a world axis (radians).
    void rotate(f32 angle, const glm::vec3& worldAxis) {
        rotation = renormalize(glm::angleAxis(angle, glm::normalize(worldAxis)) * rotation);
    }
    /// @brief Rotates around one of the entity's own axes (radians), e.g. `rotateLocal(dt, {0, 1, 0})`.
    void rotateLocal(f32 angle, const glm::vec3& localAxis) {
        rotation = renormalize(rotation * glm::angleAxis(angle, glm::normalize(localAxis)));
    }
    /// @brief Turns towards `target` by at most `maxAngle` radians.
    void rotateTowards(const glm::quat& target, f32 maxAngle) { rotation = nova::rotateTowards(rotation, target, maxAngle); }

    /// @brief Interpolates two transforms: exact position (shortest way, wrap seams included),
    ///        rotation by slerp, scale linearly. `t` in [0, 1].
    static TransformComponent lerp(const TransformComponent& a, const TransformComponent& b, f32 t) {
        TransformComponent out;
        out.position = nova::lerp(a.position, b.position, t);
        out.rotation = slerp(a.rotation, b.rotation, t);
        out.scale = glm::mix(a.scale, b.scale, t);
        return out;
    }

    /// @brief Object-to-world matrix (translate · rotate · scale), in float meters from the world origin.
    /// @note Loses precision far from the origin; matrixRelativeTo() is exact around any point.
    glm::mat4 matrix() const { return matrixRelativeTo(GlobalPosition()); }
    /// @brief Object matrix with the translation measured from `origin` (e.g. the camera).
    glm::mat4 matrixRelativeTo(const GlobalPosition& origin) const {
        return glm::translate(glm::mat4(1.0f), (position - origin).toVec3()) * glm::toMat4(rotation) *
               glm::scale(glm::mat4(1.0f), scale);
    }
    /// @brief World-space forward direction (local −Z).
    glm::vec3 forward() const { return rotation * glm::vec3(0, 0, -1); }
    /// @brief World-space right direction (local +X).
    glm::vec3 right() const { return rotation * glm::vec3(1, 0, 0); }
    /// @brief World-space up direction (local +Y).
    glm::vec3 up() const { return rotation * glm::vec3(0, 1, 0); }

    /// @brief Rotates so forward (−Z) points at `target`, using yaw and pitch only (no roll).
    void lookAt(const GlobalPosition& target) {
        const glm::vec3 dir = glm::normalize(glm::vec3((target - position).toMeters()));
        setEuler({std::asin(glm::clamp(dir.y, -1.0f, 1.0f)), std::atan2(-dir.x, -dir.z), 0.0f}, EulerOrder::YXZ);
    }
};

/// @brief Renders a mesh at the entity's transform.
struct MeshComponent {
    MeshHandle mesh;         ///< Mesh from Renderer::createMesh (or Renderer::cube/sphere/plane).
    Material material;       ///< Surface parameters.
    bool castShadows = true; ///< Included in the ray-traced shadow scene.
};

/// @brief Makes the entity a camera; it looks down its forward (−Z) axis.
struct CameraComponent {
    f32 fovY = glm::radians(60.0f); ///< Vertical field of view in radians.
    f32 nearClip = 0.05f;           ///< Near plane distance.
    f32 farClip = 500.0f;           ///< Far plane distance.
    bool primary = true;            ///< The first primary camera renders the scene (see Scene::primaryCamera).
};

/// @brief Directional light ("sun"); the light travels along the entity's forward (−Z) axis.
/// @note Only the first one in the scene is used.
struct DirectionalLightComponent {
    glm::vec3 color{1.0f, 0.96f, 0.9f}; ///< Linear RGB color.
    f32 intensity = 3.0f;               ///< Radiance multiplier.
};

/// @brief Point light at the entity's position.
/// @note At most 16 point lights are rendered per frame.
struct PointLightComponent {
    glm::vec3 color{1.0f}; ///< Linear RGB color.
    f32 intensity = 10.0f; ///< Radiance multiplier.
    f32 radius = 8.0f;     ///< Distance at which the light fades to zero.
};

/// @brief Built-in behaviour: spins the entity around `axis` while the scene updates.
struct RotatorComponent {
    glm::vec3 axis{0.0f, 1.0f, 0.0f}; ///< Rotation axis (world space; normalized internally).
    f32 speed = 1.0f;                 ///< Angular speed in radians per second.
};

/// @brief Marks an entity created from a prefab or model asset (see AssetDatabase::instantiate).
struct PrefabInstanceComponent {
    u64 asset = 0; ///< GUID of the source asset.
};

} // namespace nova
