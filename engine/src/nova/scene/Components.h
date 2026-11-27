#pragma once

/// @file
/// @brief Built-in ECS components.
///
/// Components are plain aggregates: add them with `entity.add<T>(...)` using aggregate
/// initialization, e.g. `e.add<nova::PointLightComponent>(glm::vec3(1, 0.5f, 0.2f), 25.0f, 9.0f)`.

#include "nova/math/GlobalPosition.h"
#include "nova/math/Quat.h"
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
    Quat rotation;
    ScaleVec scale{1.0, 1.0, 1.0}; ///< Per-axis scale (also scales colliders).

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
    /// @brief Euler angles.
    EulerAngles euler(EulerOrder order = kDefaultEulerOrder) const { return rotation.toEuler(order); }
    /// @brief Euler angles in degrees (editor, display).
    glm::dvec3 eulerDegrees(EulerOrder order = kDefaultEulerOrder) const { return rotation.toEulerDegrees(order); }
    /// @brief Sets the rotation from Euler angles.
    void setEuler(const EulerAngles& angles, EulerOrder order = kDefaultEulerOrder) { rotation = Quat::fromEuler(angles, order); }
    /// @brief Sets the rotation from Euler angles in degrees.
    void setEulerDegrees(const glm::dvec3& degrees, EulerOrder order = kDefaultEulerOrder) {
        rotation = Quat::fromEulerDegrees(degrees, order);
    }
    /// @}

    /// @brief Rotates around a world axis (any length and format; zero = none). Fixed-point math:
    ///        the same bits everywhere.
    template <typename A>
    void rotate(Angle angle, const Vec3<A>& worldAxis) {
        rotation = (Quat::fromAxisAngle(worldAxis, angle) * rotation).normalized();
    }
    /// @brief Rotates around one of the entity's own axes, e.g. `rotateLocal(Angle::fromDegrees(90), UnitVec(0, 1, 0))`.
    template <typename A>
    void rotateLocal(Angle angle, const Vec3<A>& localAxis) {
        rotation = (rotation * Quat::fromAxisAngle(localAxis, angle)).normalized();
    }
    /// @brief Turns towards `target` by at most `maxStep`.
    void rotateTowards(const Quat& target, Angle maxStep) { rotation = nova::rotateTowards(rotation, target, maxStep); }

    /// @brief Interpolates two transforms: exact position (shortest way, wrap seams included),
    ///        rotation by slerp, scale linearly. `t` in [0, 1].
    static TransformComponent lerp(const TransformComponent& a, const TransformComponent& b, Unit t) {
        TransformComponent out;
        out.position = nova::lerp(a.position, b.position, t);
        out.rotation = slerp(a.rotation, b.rotation, t);
        out.scale = a.scale + (b.scale - a.scale).scaled(t);
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
        const glm::dvec3 d = (target - position).toMeters();
        const f64 len = glm::length(d);
        if (!(len > 0.0)) return;
        const glm::dvec3 dir = d / len;
        setEuler({Angle::fromRadians(std::asin(glm::clamp(dir.y, -1.0, 1.0))), Angle::fromRadians(std::atan2(-dir.x, -dir.z)),
                  Angle()},
                 EulerOrder::YXZ);
    }
};

/// @brief A pose relative to a parent: position (in the parent's frame, before its scale is
///        applied: it is scaled with the parent), rotation and scale. Fixed point like the poses it
///        comes from: the position in 64-bit world steps (Q45.18, any offset, also one divided by
///        a tiny parent scale; for a root, its position in the world), the rotation and scale in
///        the transform's formats.
struct LocalTransform {
    GlobalLengthVec position;
    Quat rotation;
    ScaleVec scale{1.0, 1.0, 1.0};
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
    Quat placedRotation;
    ScaleVec placedScale{1, 1, 1};
    GlobalPosition parentPosition; ///< The parent's pose the offset is for: followed when it differs.
    Quat parentRotation;
    ScaleVec parentScale{1, 1, 1};
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
    Angle fovY = Angle::fromDegrees(60);  ///< Perspective: vertical field of view.
    Length orthoSize = Length(10.0);      ///< Orthographic: height of the view (m); the width follows the aspect.
    Distance nearClip = Distance(0.05);   ///< Near plane distance (m).
    Distance farClip = Distance(500.0);   ///< Far plane distance (m).
    bool primary = true;                  ///< The first primary camera renders the scene (see Scene::primaryCamera).
};

/// @brief Directional light ("sun"); the light travels along the entity's forward (−Z) axis.
/// @note Only the first one in the scene is used.
struct DirectionalLightComponent {
    glm::vec3 color{1.0f, 0.96f, 0.9f}; ///< Linear RGB color (render data: float).
    f32 intensity = 3.0f;               ///< Radiance multiplier (render data: float).
};

/// @brief Point light at the entity's position.
/// @note At most 16 point lights are rendered per frame.
struct PointLightComponent {
    glm::vec3 color{1.0f};           ///< Linear RGB color (render data: float).
    f32 intensity = 10.0f;           ///< Radiance multiplier (render data: float).
    Length radius = Length(8.0);     ///< Distance at which the light fades to zero (m).
    bool castShadows = true; ///< Things block it (ray traced, or cube shadow maps without ray tracing).
};

/// @brief Built-in behaviour: spins the entity around `axis` while the scene updates.
///
/// Fixed point like the rest of what moves the simulation (it turns kinematic bodies): each
/// update turns it by `speed` times the exact frame duration (angleTurned(): exact, wraps around).
struct RotatorComponent {
    UnitVec axis{0.0, 1.0, 0.0};                   ///< Rotation axis (world space; normalized when used).
    AngleSpeed speed = AngleSpeed::fromRadiansPerSecond(1.0); ///< Angular speed (edited in degrees per second).
};

/// @brief Marks an entity created from a prefab or model asset (see AssetDatabase::instantiate).
struct PrefabInstanceComponent {
    u64 asset = 0; ///< GUID of the source asset.
};

} // namespace nova
