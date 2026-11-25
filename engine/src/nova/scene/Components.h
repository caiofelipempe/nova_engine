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
/// Position, rotation and scale are fixed point (the simulation state: same bits on every machine).
/// Rotation is a Q1.30 quaternion (no gimbal lock, exact round trips with physics); read or set it
/// as Euler angles in any axis order with euler()/setEuler() (see EulerOrder), or assign a
/// `glm::quat` (quantized). orientation() and scaleF() give floats for rendering.
/// The entity's local axes: right = +X, up = +Y, forward = −Z.
struct TransformComponent {
    /// @brief World position: Q13.18 fixed point on the 16 km torus (see WorldSpace). Assign meters directly
    ///        (`position = {0, 3, 10}`), move with offsets (`position += velocity * dt`).
    GlobalPosition position;
    /// @brief World rotation, a unit quaternion in Q1.30 (assigning a `glm::quat` normalizes it).
    FixedQuat rotation;
    ScalarVec scale{1.0, 1.0, 1.0}; ///< Per-axis scale (also scales colliders).

    /// @brief The rotation as floats (rendering, glm math).
    glm::quat orientation() const { return rotation.toGlm(); }
    /// @brief The scale as floats.
    glm::vec3 scaleF() const { return scale.toGlm(); }
    /// @brief Sets the rotation (normalized and quantized).
    void setRotation(const glm::quat& q) { rotation = q; }

    /// @name Euler angles (a view of `rotation`)
    /// `x` = pitch (around X), `y` = yaw (around Y), `z` = roll (around Z), applied in `order`
    /// around the local axes (default YXZ: yaw → pitch → roll).
    /// @{
    /// @brief Euler angles in radians.
    glm::vec3 euler(EulerOrder order = kDefaultEulerOrder) const { return eulerFromQuat(orientation(), order); }
    /// @brief Euler angles in degrees.
    glm::vec3 eulerDegrees(EulerOrder order = kDefaultEulerOrder) const { return eulerDegreesFromQuat(orientation(), order); }
    /// @brief Sets the rotation from Euler angles in radians.
    void setEuler(const glm::vec3& radians, EulerOrder order = kDefaultEulerOrder) { rotation = quatFromEuler(radians, order); }
    /// @brief Sets the rotation from Euler angles in degrees.
    void setEulerDegrees(const glm::vec3& degrees, EulerOrder order = kDefaultEulerOrder) {
        rotation = quatFromEulerDegrees(degrees, order);
    }
    /// @}

    /// @brief Rotates around a world axis (radians). Fixed-point math: the same bits everywhere.
    void rotate(f32 angle, const glm::vec3& worldAxis) { rotation = (turn(angle, worldAxis) * rotation).normalized(); }
    /// @brief Rotates around one of the entity's own axes (radians), e.g. `rotateLocal(dt, {0, 1, 0})`.
    void rotateLocal(f32 angle, const glm::vec3& localAxis) {
        rotation = (rotation * turn(angle, localAxis)).normalized();
    }
    /// @brief Turns towards `target` by at most `maxAngle` radians.
    void rotateTowards(const glm::quat& target, f32 maxAngle) {
        rotation = nova::rotateTowards(orientation(), target, maxAngle);
    }
    /// @brief The fixed-point rotation of `angle` radians around `axis` (any length; zero = none).
    static FixedQuat turn(f32 angle, const glm::vec3& axis) { return turn(static_cast<f64>(angle), glm::dvec3(axis)); }
    /// @brief Same, from doubles (an angle worked out exactly, e.g. a speed times a Duration).
    static FixedQuat turn(f64 angle, const glm::dvec3& axis) {
        const f64 len = glm::length(axis);
        if (!(len > 0.0)) return {};
        const f64 wrapped = std::remainder(angle, 2.0 * glm::pi<f64>()); // [−π, π]: fits Q3.28
        return FixedQuat::fromAxisAngle(UnitVec(axis / len), toFixed<Angle>(wrapped));
    }

    /// @brief Interpolates two transforms: exact position (shortest way, wrap seams included),
    ///        rotation by slerp, scale linearly. `t` in [0, 1].
    static TransformComponent lerp(const TransformComponent& a, const TransformComponent& b, f32 t) {
        TransformComponent out;
        out.position = nova::lerp(a.position, b.position, t);
        out.rotation = slerp(a.orientation(), b.orientation(), t);
        out.scale = glm::mix(a.scale.toDouble(), b.scale.toDouble(), static_cast<f64>(t));
        return out;
    }

    /// @brief Object-to-world matrix (translate · rotate · scale), in float meters from the world origin.
    /// @note Loses precision far from the origin; matrixRelativeTo() is exact around any point.
    glm::mat4 matrix() const { return matrixRelativeTo(GlobalPosition()); }
    /// @brief Object matrix with the translation measured from `origin` (e.g. the camera).
    glm::mat4 matrixRelativeTo(const GlobalPosition& origin) const {
        return glm::translate(glm::mat4(1.0f), (position - origin).toVec3()) * glm::toMat4(orientation()) *
               glm::scale(glm::mat4(1.0f), scaleF());
    }
    /// @brief World-space forward direction (local −Z).
    glm::vec3 forward() const { return orientation() * glm::vec3(0, 0, -1); }
    /// @brief World-space right direction (local +X).
    glm::vec3 right() const { return orientation() * glm::vec3(1, 0, 0); }
    /// @brief World-space up direction (local +Y).
    glm::vec3 up() const { return orientation() * glm::vec3(0, 1, 0); }

    /// @brief Rotates so forward (−Z) points at `target`, using yaw and pitch only (no roll).
    void lookAt(const GlobalPosition& target) {
        const glm::vec3 dir = glm::normalize(glm::vec3((target - position).toMeters()));
        setEuler({std::asin(glm::clamp(dir.y, -1.0f, 1.0f)), std::atan2(-dir.x, -dir.z), 0.0f}, EulerOrder::YXZ);
    }
};

/// @brief A pose relative to a parent: position (in the parent's frame, before its scale is
///        applied: it is scaled with the parent), rotation and scale. Doubles: offsets are small.
struct LocalTransform {
    glm::dvec3 position{0.0};
    glm::dquat rotation{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 scale{1.0};
};

/// @brief Makes the entity a child of another: it keeps its offset from the parent (position,
///        rotation and scale in the parent's frame) while the parent moves, turns and scales.
///
/// The entity's TransformComponent stays its pose **in the world** (what rendering, physics and
/// tools read); the scene keeps it in step with the parent (Scene::updateTransforms(), run by
/// update() and buildRenderScene()). Moved by something else — a script, the gizmo, its own dynamic
/// body — it keeps the new pose and its offset is taken again from there; so is it when the
/// parent changes. Only `parent` is saved: the offset follows from the saved poses.
/// Use Scene::setParent() / Entity::setParent() (they refuse loops), or set `parent` directly
/// (the inspector does): the entity keeps its pose in the world either way.
struct ParentComponent {
    EntityRef parent; ///< The parent (none: a root, as without the component).
    // Kept by the scene (not saved):
    LocalTransform local;         ///< The offset from the parent.
    u64 linkedTo = 0;             ///< The parent the offset was taken from (another one: taken again).
    bool synced = false;          ///< `local` and the poses below are valid.
    GlobalPosition placedPosition; ///< Where the scene last put the entity: moved since if it differs.
    FixedQuat placedRotation;
    ScalarVec placedScale{1, 1, 1};
    GlobalPosition parentPosition; ///< The parent's pose the offset is for: followed when it differs.
    FixedQuat parentRotation;
    ScalarVec parentScale{1, 1, 1};
};

/// @brief Renders a mesh at the entity's transform.
struct MeshComponent {
    MeshHandle mesh;         ///< Mesh from Renderer::createMesh (or Renderer::cube/sphere/plane).
    Material material;       ///< Surface parameters.
    bool castShadows = true; ///< Included in the ray-traced shadow scene.
};

/// @brief Makes the entity a camera; it looks down its forward (−Z) axis.
/// @brief How a camera projects the scene.
enum class CameraProjection : u8 {
    Perspective,  ///< Things shrink with distance (fovY).
    Orthographic, ///< Parallel lines stay parallel; sizes do not change with distance (orthoSize).
};

struct CameraComponent {
    CameraProjection projection = CameraProjection::Perspective; ///< Perspective or orthographic.
    f32 fovY = glm::radians(60.0f); ///< Perspective: vertical field of view in radians.
    f32 orthoSize = 10.0f;          ///< Orthographic: height of the view (m); the width follows the aspect.
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
    bool castShadows = true; ///< Things block it (ray traced, or cube shadow maps without ray tracing).
};

/// @brief Built-in behaviour: spins the entity around `axis` while the scene updates.
///
/// Fixed point like the rest of what moves the simulation (it turns kinematic bodies): each
/// update turns it by `speed` times the exact frame duration.
struct RotatorComponent {
    ScalarVec axis{0, 1, 0}; ///< Rotation axis (world space, any length; normalized internally).
    Scalar speed = 1;        ///< Angular speed in radians per second.
};

/// @brief Marks an entity created from a prefab or model asset (see AssetDatabase::instantiate).
struct PrefabInstanceComponent {
    u64 asset = 0; ///< GUID of the source asset.
};

} // namespace nova
