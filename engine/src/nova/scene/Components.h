#pragma once

/// @file
/// @brief Built-in ECS components.
///
/// Components are plain aggregates: add them with `entity.add<T>(...)` using aggregate
/// initialization, e.g. `e.add<nova::PointLightComponent>(glm::vec3(1, 0.5f, 0.2f), 25.0f, 9.0f)`.

#include "nova/physics/PhysicsWorld.h"
#include "nova/renderer/Mesh.h"

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
/// Rotation is stored as Euler angles so it can be edited directly; use orientation() for a quaternion.
/// The entity's local axes: right = +X, up = +Y, forward = −Z.
struct TransformComponent {
    glm::vec3 position{0.0f}; ///< World position.
    glm::vec3 rotation{0.0f}; ///< Euler angles in radians: x = pitch, y = yaw, z = roll.
    glm::vec3 scale{1.0f};    ///< Per-axis scale (also scales colliders).

    /// @brief Rotation as a quaternion.
    glm::quat orientation() const { return glm::quat(rotation); }
    /// @brief Object-to-world matrix (translate · rotate · scale).
    glm::mat4 matrix() const {
        return glm::translate(glm::mat4(1.0f), position) * glm::toMat4(orientation()) *
               glm::scale(glm::mat4(1.0f), scale);
    }
    /// @brief World-space forward direction (local −Z).
    glm::vec3 forward() const { return orientation() * glm::vec3(0, 0, -1); }
    /// @brief World-space right direction (local +X).
    glm::vec3 right() const { return orientation() * glm::vec3(1, 0, 0); }
    /// @brief World-space up direction (local +Y).
    glm::vec3 up() const { return orientation() * glm::vec3(0, 1, 0); }

    /// @brief Rotates so forward (−Z) points at `target`, using yaw and pitch only (no roll).
    /// @note Suited to cameras: the result can be edited further by changing rotation.x / rotation.y.
    void lookAt(const glm::vec3& target) {
        glm::vec3 dir = glm::normalize(target - position);
        rotation = {std::asin(glm::clamp(dir.y, -1.0f, 1.0f)), std::atan2(-dir.x, -dir.z), 0.0f};
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

// ----------------------------------------------------------------- physics --

/// @brief Makes the entity a physics rigid body while the scene is simulating.
///
/// The shape comes from a Box/Sphere/CapsuleColliderComponent on the same entity; without one,
/// a box matching the entity's scale is used. Collider sizes are multiplied by the transform scale.
/// Body properties are read when the body is created (Scene::startSimulation or the first
/// update after adding the component); later edits apply on the next simulation start.
struct RigidBodyComponent {
    physics::BodyType type = physics::BodyType::Dynamic; ///< Static, kinematic (follows the transform) or dynamic.
    f32 mass = 1.0f;                                     ///< Mass in kg (dynamic only).
    f32 friction = 0.5f;                                 ///< Friction coefficient.
    f32 restitution = 0.1f;                              ///< Bounciness (0..1).
    f32 linearDamping = 0.05f;                           ///< Linear velocity damping.
    f32 angularDamping = 0.05f;                          ///< Angular velocity damping.
    f32 gravityFactor = 1.0f;                            ///< Gravity multiplier.
    bool isSensor = false;                               ///< Trigger: reports collisions, no response.

    physics::BodyId body; ///< Runtime body; valid while the scene is simulating. Managed by Scene.
};

/// @brief Box collision shape for a RigidBodyComponent.
struct BoxColliderComponent {
    glm::vec3 halfExtents{0.5f}; ///< Half size per axis, before scaling (0.5 = matches the unit cube mesh).
    glm::vec3 offset{0.0f};      ///< Center relative to the entity origin, before scaling.
};

/// @brief Sphere collision shape for a RigidBodyComponent.
struct SphereColliderComponent {
    f32 radius = 0.5f;      ///< Radius before scaling (multiplied by the largest scale axis).
    glm::vec3 offset{0.0f}; ///< Center relative to the entity origin, before scaling.
};

/// @brief Capsule collision shape (aligned with local Y) for a RigidBodyComponent.
struct CapsuleColliderComponent {
    f32 radius = 0.5f;      ///< Radius before scaling (multiplied by max(scale.x, scale.z)).
    f32 halfHeight = 0.5f;  ///< Half length of the cylindrical part, before scaling (multiplied by scale.y).
    glm::vec3 offset{0.0f}; ///< Center relative to the entity origin, before scaling.
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
