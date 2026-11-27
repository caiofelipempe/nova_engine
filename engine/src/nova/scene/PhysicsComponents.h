#pragma once

/// @file
/// @brief Physics components (frontend): what games put on entities. Scene turns them into bodies
///        of a physics::PhysicsWorld; no backend type appears here.

#include "nova/math/Quat.h"
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
/// Simulation attributes are fixed point, each in its own dimension (math/Scalar.h): numbers go
/// in directly (`rb.mass = 2`, `rb.friction = 0.8f`).
///
/// Edits apply while simulating: friction, restitution, damping, gravity factor, sensor, layer
/// and continuous collision change in place; type, mass, locks, colliders and scale rebuild the
/// body (keeping its velocity; its touches end and begin again). Static bodies follow their
/// transform when it is moved.
struct RigidBodyComponent {
    physics::BodyType type = physics::BodyType::Dynamic; ///< Static, kinematic (follows the transform) or dynamic.
    Mass mass = Mass(1.0);                               ///< Mass in kg (dynamic only; up to 131 t).
    Friction friction = Friction(0.5);                   ///< Friction coefficient.
    Restitution restitution = Restitution(0.1);          ///< Bounciness (0..1).
    Damping linearDamping = Damping(0.05);               ///< Linear velocity damping (1/s).
    Damping angularDamping = Damping(0.05);              ///< Angular velocity damping (1/s).
    GravityFactor gravityFactor = GravityFactor(1.0);    ///< Gravity multiplier.
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
    LengthVec halfExtents{0.5, 0.5, 0.5}; ///< Half size per axis, before scaling (0.5 = matches the unit cube mesh).
    LengthVec offset;                     ///< Center relative to the entity origin, before scaling.
    Quat rotation;                   ///< Orientation relative to the entity.
};

/// @brief Sphere collision shape for a RigidBodyComponent.
struct SphereColliderComponent {
    Length radius = Length(0.5); ///< Radius before scaling (multiplied by the largest scale axis).
    LengthVec offset;            ///< Center relative to the entity origin, before scaling.
};

/// @brief Capsule collision shape (aligned with local Y) for a RigidBodyComponent.
struct CapsuleColliderComponent {
    Length radius = Length(0.5);     ///< Radius before scaling (multiplied by max(scale.x, scale.z)).
    Length halfHeight = Length(0.5); ///< Half length of the cylindrical part, before scaling (multiplied by scale.y).
    LengthVec offset;                ///< Center relative to the entity origin, before scaling.
    Quat rotation;              ///< Orientation relative to the entity (axis = local Y).
};

/// @brief Cylinder collision shape (axis = local Y) for a RigidBodyComponent.
struct CylinderColliderComponent {
    Length radius = Length(0.5);     ///< Radius before scaling (multiplied by max(scale.x, scale.z)).
    Length halfHeight = Length(0.5); ///< Half height before scaling (multiplied by scale.y).
    LengthVec offset;                ///< Center relative to the entity origin, before scaling.
    Quat rotation;              ///< Orientation relative to the entity (axis = local Y).
};

/// @brief Convex polyhedron collision shape: the convex hull of a mesh's vertices (scaled by the
///        transform). Works for any body type, dynamic included.
struct ConvexHullColliderComponent {
    MeshHandle mesh;    ///< Source mesh; invalid = the entity's MeshComponent mesh.
    LengthVec offset;   ///< Shift relative to the entity origin, before scaling.
    Quat rotation; ///< Orientation relative to the entity.
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
    LengthVec anchor;                ///< Pivot on this entity (local, scaled with it).
    UnitVec axis{0.0, 1.0, 0.0};     ///< Hinge axis / slider direction (local to this entity).
    /// @brief Distance joints: the attachment on the other entity (its local space) or, with the
    ///        world, an offset from this entity's position at start (world axes).
    LengthVec connectedAnchor{0.0, 2.0, 0.0};
    bool limitsEnabled = false;      ///< Hinge / slider: limit the motion.
    Angle angleMin = Angle::fromDegrees(-45); ///< Hinge: lowest angle from the start ([−180°, 0°]).
    Angle angleMax = Angle::fromDegrees(45);  ///< Hinge: highest angle from the start ([0°, 180°]).
    Length limitMin = Length(-45.0); ///< Slider: lowest position from the start (m).
    Length limitMax = Length(45.0);  ///< Slider: highest position from the start (m).
    Length minDistance = Length(-1.0); ///< Distance joints: shortest (m; −1 = at start).
    Length maxDistance = Length(-1.0); ///< Distance joints: longest (m; −1 = at start).
    Frequency springFrequency = Frequency(0.0);  ///< Distance joints: softness (Hz; 0 = rigid).
    DampingRatio springDamping = DampingRatio(0.5); ///< Damping ratio of that spring.
    physics::JointMotorMode motorMode = physics::JointMotorMode::Off; ///< Hinge / slider motor.
    AngleSpeed motorSpeed;                   ///< Hinge velocity motor: the angular speed to reach.
    Angle motorAngle;                        ///< Hinge position motor: the angle to reach (from the start).
    Speed motorVelocity;                     ///< Slider velocity motor: the speed to reach (m/s).
    Length motorPosition;                    ///< Slider position motor: the position to reach (m, from the start).
    Force motorMaxForce = Force(0.0);  ///< Hinge: N·m; slider: N; 0 = unlimited.
    Force breakForce = Force(0.0);     ///< Breaks above this force (N); 0 = never.
    Force breakTorque = Force(0.0);    ///< Breaks above this torque (N·m); 0 = never.
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
    Length radius = Length(0.3);             ///< Capsule radius (m).
    Length height = Length(1.8);             ///< Total height (m), radius included.
    LengthVec offset{0.0, 0.9, 0.0};         ///< Capsule center relative to the entity origin (world axes).
    Angle maxSlope = Angle::fromDegrees(50); ///< Steepest walkable slope.
    Length stepHeight = Length(0.3);         ///< Highest step it walks up (m).
    Length stickToFloor = Length(0.5);       ///< Down steps it follows instead of flying off (m).
    Mass mass = Mass(70.0);                  ///< Kg (pushing and being pushed).
    Force maxPushForce = Force(100.0);       ///< Strongest push on dynamic bodies (N).
    GravityFactor gravityFactor = GravityFactor(1.0); ///< Gravity multiplier.
    u32 layer = 0;                           ///< Collision layer.
    SpeedVec velocity;                       ///< In: what to move with; out: what it moved with (m/s).

    bool grounded = false;                   ///< Runtime: standing on walkable ground.
    physics::GroundState groundState = physics::GroundState::InAir; ///< Runtime: what it stands on.
    UnitVec groundNormal{0.0, 1.0, 0.0};     ///< Runtime: normal of the ground below.
    EntityRef ground;                        ///< Runtime: the entity below (none: in the air or not an entity).
    physics::CharacterId character;          ///< Runtime character. Managed by Scene.
    u64 appliedKey = 0;                      ///< Runtime: what it was built from.
    u64 appliedPose = 0;                     ///< Runtime: the position last written (teleports are detected).
};

/// @brief Scene-wide physics settings (saved with the scene).
struct PhysicsSettings {
    AccelerationVec gravity{0.0, -9.81, 0.0}; ///< Gravity acceleration (m/s²).
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
