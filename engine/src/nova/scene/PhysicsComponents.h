#pragma once

/// @file
/// @brief Physics components (frontend): what games put on entities. Scene turns them into bodies
///        of a physics::PhysicsWorld; no backend type appears here.

#include "nova/physics/PhysicsWorld.h"
#include "nova/renderer/Mesh.h"
#include "nova/scene/EntityRef.h"

#include <array>
#include <string>
#include <string_view>

namespace nova {

/// @brief Axes a dynamic body may not move or turn along (world axes).
struct RigidBodyLocks {
    bool positionX = false, positionY = false, positionZ = false; ///< No motion along the axis.
    bool rotationX = false, rotationY = false, rotationZ = false; ///< No turning around the axis.
    /// @brief As physics::AxisLock bits.
    u8 bits() const {
        return static_cast<u8>((positionX ? physics::LockPositionX : 0) | (positionY ? physics::LockPositionY : 0) |
                               (positionZ ? physics::LockPositionZ : 0) | (rotationX ? physics::LockRotationX : 0) |
                               (rotationY ? physics::LockRotationY : 0) | (rotationZ ? physics::LockRotationZ : 0));
    }
};

/// @brief Makes the entity a physics rigid body while the scene is simulating.
///
/// The shape comes from the collider components on the same entity: one gives that shape,
/// several are joined into one compound shape (e.g. a box and a sphere); without any, a box
/// matching the entity's scale is used. Collider sizes are multiplied by the transform scale.
///
/// Edits apply while simulating: friction, restitution, damping, gravity factor, sensor, layer
/// and continuous collision change in place; type, mass, locks, colliders and scale rebuild the
/// body (keeping its velocity; its touches end and begin again). Static bodies follow their
/// transform when it is moved.
struct RigidBodyComponent {
    physics::BodyType type = physics::BodyType::Dynamic; ///< Static, kinematic (follows the transform) or dynamic.
    f32 mass = 1.0f;                                     ///< Mass in kg (dynamic only).
    f32 friction = 0.5f;                                 ///< Friction coefficient.
    f32 restitution = 0.1f;                              ///< Bounciness (0..1).
    f32 linearDamping = 0.05f;                           ///< Linear velocity damping.
    f32 angularDamping = 0.05f;                          ///< Angular velocity damping.
    f32 gravityFactor = 1.0f;                            ///< Gravity multiplier.
    bool isSensor = false;                               ///< Trigger: reports overlaps (Script::onTriggerEnter/Exit), no response.
    u32 layer = 0;                                       ///< Collision layer (Scene::physicsSettings() says which collide).
    bool continuousCollision = false;                    ///< For fast bodies (bullets): never tunnel through thin walls.
    RigidBodyLocks locks;                                ///< Dynamic bodies: axes they may not move / turn along.

    physics::BodyId body; ///< Runtime body; valid while the scene is simulating. Managed by Scene.
    u64 appliedShape = 0; ///< Runtime: what the body was built from (Scene rebuilds it when this changes).
    u64 appliedProperties = 0; ///< Runtime: the properties last applied.
    u64 appliedPose = 0;  ///< Runtime: static bodies' last applied pose.
};

/// @brief Box collision shape for a RigidBodyComponent.
struct BoxColliderComponent {
    glm::vec3 halfExtents{0.5f}; ///< Half size per axis, before scaling (0.5 = matches the unit cube mesh).
    glm::vec3 offset{0.0f};      ///< Center relative to the entity origin, before scaling.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< Orientation relative to the entity.
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
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< Orientation relative to the entity (axis = local Y).
};

/// @brief Cylinder collision shape (axis = local Y) for a RigidBodyComponent.
struct CylinderColliderComponent {
    f32 radius = 0.5f;      ///< Radius before scaling (multiplied by max(scale.x, scale.z)).
    f32 halfHeight = 0.5f;  ///< Half height before scaling (multiplied by scale.y).
    glm::vec3 offset{0.0f}; ///< Center relative to the entity origin, before scaling.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< Orientation relative to the entity (axis = local Y).
};

/// @brief Convex polyhedron collision shape: the convex hull of a mesh's vertices (scaled by the
///        transform). Works for any body type, dynamic included.
struct ConvexHullColliderComponent {
    MeshHandle mesh;        ///< Source mesh; invalid = the entity's MeshComponent mesh.
    glm::vec3 offset{0.0f}; ///< Shift relative to the entity origin, before scaling.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< Orientation relative to the entity.
};

/// @brief Exact triangle-mesh collision shape (level geometry, glTF scenery). Static and kinematic
///        bodies only: on a dynamic body it falls back to the mesh's convex hull.
struct MeshColliderComponent {
    MeshHandle mesh; ///< Source mesh; invalid = the entity's MeshComponent mesh.
};

/// @brief Connects this entity's rigid body to another one (or to the world): a hinge, a
///        slider, a rope... Both entities need a RigidBodyComponent.
///
/// Anchor and axis are in this entity's local space, taken where the bodies are when the
/// simulation starts (or when the joint is added). Edits apply while simulating (motor changes in
/// place, the rest rebuilds the joint). A joint with a break force or torque breaks when it holds
/// more (Script::onJointBreak); it stays broken until `broken` is cleared.
struct JointComponent {
    physics::JointType type = physics::JointType::Hinge; ///< Kind of joint.
    EntityRef connected;             ///< The other body's entity; none = the world.
    glm::vec3 anchor{0.0f};          ///< Pivot on this entity (local, scaled with it).
    glm::vec3 axis{0.0f, 1.0f, 0.0f}; ///< Hinge axis / slider direction (local to this entity).
    /// @brief Distance joints: the attachment on the other entity (its local space) or, with the
    ///        world, an offset from this entity's position at start (world axes).
    glm::vec3 connectedAnchor{0.0f, 2.0f, 0.0f};
    bool limitsEnabled = false;      ///< Hinge / slider: limit the motion.
    f32 limitMin = -45.0f;           ///< Hinge: degrees; slider: meters (from the start).
    f32 limitMax = 45.0f;            ///< See limitMin.
    f32 minDistance = -1.0f;         ///< Distance joints: shortest (m; −1 = at start).
    f32 maxDistance = -1.0f;         ///< Distance joints: longest (m; −1 = at start).
    f32 springFrequency = 0.0f;      ///< Distance joints: softness (Hz; 0 = rigid).
    f32 springDamping = 0.5f;        ///< Damping ratio of that spring.
    physics::JointMotorMode motorMode = physics::JointMotorMode::Off; ///< Hinge / slider motor.
    f32 motorTarget = 0.0f;          ///< Hinge: deg/s or degrees; slider: m/s or meters.
    f32 motorMaxForce = 0.0f;        ///< Hinge: N·m; slider: N; 0 = unlimited.
    f32 breakForce = 0.0f;           ///< Breaks above this force (N); 0 = never.
    f32 breakTorque = 0.0f;          ///< Breaks above this torque (N·m); 0 = never.
    bool broken = false;             ///< Set when it broke; clear it to build the joint again.

    physics::JointId joint;          ///< Runtime joint. Managed by Scene.
    physics::BodyId connectedBody;   ///< Runtime: the other body when the joint was made.
    u64 appliedKey = 0;              ///< Runtime: what the joint was built from.
    u64 appliedMotor = 0;            ///< Runtime: the motor last applied.
};

/// @brief A walking character (player, NPC): an upright capsule moved by velocity, not by forces.
///
/// Scripts set `velocity` every frame (typically: keep its vertical part, replace the horizontal
/// part with the input, add a jump); the scene moves the character with it plus gravity while it is
/// in the air, sliding along walls, walking up steps and slopes, sticking to the floor going down
/// and riding moving platforms. After the update `velocity` holds what it really moved with, and
/// `grounded` / `groundNormal` / `ground` say what it stands on. It pushes dynamic bodies, and
/// queries and sensors see it. Do not put a RigidBodyComponent on the same entity.
///
/// The entity's origin is at the character's feet: `offset` goes from there to the capsule's center.
struct CharacterControllerComponent {
    f32 radius = 0.3f;                       ///< Capsule radius (m).
    f32 height = 1.8f;                       ///< Total height (m), radius included.
    glm::vec3 offset{0.0f, 0.9f, 0.0f};      ///< Capsule center relative to the entity origin (world axes).
    f32 maxSlope = 50.0f;                    ///< Steepest walkable slope (degrees).
    f32 stepHeight = 0.3f;                   ///< Highest step it walks up (m).
    f32 stickToFloor = 0.5f;                 ///< Down steps it follows instead of flying off (m).
    f32 mass = 70.0f;                        ///< Kg (pushing and being pushed).
    f32 maxPushForce = 100.0f;               ///< Strongest push on dynamic bodies (N).
    f32 gravityFactor = 1.0f;                ///< Gravity multiplier.
    u32 layer = 0;                           ///< Collision layer.
    glm::vec3 velocity{0.0f};                ///< In: what to move with; out: what it moved with (m/s).

    bool grounded = false;                   ///< Runtime: standing on walkable ground.
    physics::GroundState groundState = physics::GroundState::InAir; ///< Runtime: what it stands on.
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f}; ///< Runtime: normal of the ground below.
    EntityRef ground;                        ///< Runtime: the entity below (none: in the air or not an entity).
    physics::CharacterId character;          ///< Runtime character. Managed by Scene.
    u64 appliedKey = 0;                      ///< Runtime: what it was built from.
    u64 appliedPose = 0;                     ///< Runtime: the position last written (teleports are detected).
};

/// @brief Scene-wide physics settings (saved with the scene).
struct PhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f}; ///< Gravity acceleration (m/s²).
    physics::CollisionLayers layers;       ///< Which layers collide (default: all).
    /// @brief Display names; empty = "Layer N" (layer 0 is "Default").
    std::array<std::string, physics::kCollisionLayerCount> layerNames;

    /// @brief Name to show for layer `i`.
    std::string layerName(u32 i) const {
        if (i >= physics::kCollisionLayerCount) return {};
        if (!layerNames[i].empty()) return layerNames[i];
        if (i == 0) return "Default";
        return "Layer " + std::to_string(i);
    }
    /// @brief Index of the layer with this name (as layerName() shows it), or −1.
    int findLayer(std::string_view name) const {
        for (u32 i = 0; i < physics::kCollisionLayerCount; ++i)
            if (layerName(i) == name) return static_cast<int>(i);
        return -1;
    }
    /// @brief Mask of the named layers (unknown names are ignored).
    u32 maskOf(std::initializer_list<std::string_view> names) const {
        u32 m = 0;
        for (std::string_view n : names)
            if (const int i = findLayer(n); i >= 0) m |= physics::layerBit(static_cast<u32>(i));
        return m;
    }
    bool operator==(const PhysicsSettings&) const = default;
};

} // namespace nova
