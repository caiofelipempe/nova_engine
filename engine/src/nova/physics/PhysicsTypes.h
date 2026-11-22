#pragma once

/// @file
/// @brief Physics types shared by the global contract (PhysicsWorld) and the float-local one
///        (local::World): backends, body types, body-local shapes, ids, filters.

#include "nova/core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <memory>
#include <vector>

namespace nova::physics {

/// @brief Available physics backends.
enum class Backend {
    Jolt ///< Jolt Physics (multi-threaded, float; used through TorusWorld).
};

/// @brief How a body moves.
enum class BodyType : u8 {
    Static,    ///< Never moves (level geometry).
    Kinematic, ///< Moved by code/animation; pushes dynamic bodies but is not pushed back.
    Dynamic,   ///< Fully simulated (gravity, forces, collisions).
};

/// @brief Collision shape kinds.
enum class ShapeType : u8 {
    Box,        ///< ShapeDesc::halfExtents.
    Sphere,     ///< ShapeDesc::radius.
    Capsule,    ///< ShapeDesc::radius and ShapeDesc::halfHeight, aligned with local Y.
    Cylinder,   ///< ShapeDesc::radius and ShapeDesc::halfHeight, aligned with local Y.
    ConvexHull, ///< Convex polyhedron: the hull of ShapeDesc::geometry->points (any body type).
    Mesh,       ///< Triangle mesh: ShapeDesc::geometry points + indices. Static/kinematic bodies only.
    Compound,   ///< Several shapes rigidly joined: ShapeDesc::children (each with its own offset/rotation).
};

/// @brief Vertices (and triangles) of a ConvexHull or Mesh shape, local to the body.
///
/// A convex polyhedron is given by its **points**: backends derive what their collision needs
/// (Jolt builds its own hull with faces and planes; a custom engine would build a half-edge
/// structure). Shared by pointer: many bodies and regions can use one geometry without copies.
struct ShapeGeometry {
    std::vector<glm::vec3> points; ///< Local vertices.
    std::vector<u32> indices;      ///< Mesh: 3 per triangle, counter-clockwise from outside. Unused by hulls.
};

struct ShapeDesc;

/// @brief Collision shape, in coordinates **local to the body** (sizes final, already scaled).
///
/// Shapes stay small and local whatever the body's world position: only the body itself has a
/// global (integer) position.
struct ShapeDesc {
    ShapeType type = ShapeType::Box; ///< Primitive.
    glm::vec3 halfExtents{0.5f};     ///< Box: half size on each axis.
    f32 radius = 0.5f;               ///< Sphere / Capsule radius.
    f32 halfHeight = 0.5f;           ///< Capsule: half length of the cylindrical part (along local Y).
    glm::vec3 offset{0.0f};          ///< Shape center relative to the body origin (local space).
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< Shape orientation relative to the body (local space).
    std::shared_ptr<const ShapeGeometry> geometry; ///< ConvexHull / Mesh vertices (already scaled).
    std::shared_ptr<const std::vector<ShapeDesc>> children; ///< Compound: the parts (no nested compounds).
};

/// @brief Degrees of freedom a dynamic body may not use (bit mask; see BodyDesc::lockedAxes).
enum AxisLock : u8 {
    LockPositionX = 1 << 0,
    LockPositionY = 1 << 1,
    LockPositionZ = 1 << 2,
    LockRotationX = 1 << 3,
    LockRotationY = 1 << 4,
    LockRotationZ = 1 << 5,
    LockAllRotation = LockRotationX | LockRotationY | LockRotationZ,
};

/// @brief Identifier of a body inside a world.
struct BodyId {
    u32 value = ~0u; ///< Backend-specific id; `~0u` = invalid.
    /// @brief True unless default-constructed. A valid id may refer to a destroyed body.
    bool valid() const { return value != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const BodyId&) const = default;
};

/// @name Collision layers
/// @{
/// @brief Number of collision layers (a body is in one: BodyDesc::layer).
constexpr u32 kCollisionLayerCount = 32;
/// @brief Layer mask with every layer (queries: QueryFilter::layerMask).
constexpr u32 kAllLayers = ~0u;
/// @brief Mask of one layer.
constexpr u32 layerBit(u32 layer) { return 1u << (layer & (kCollisionLayerCount - 1)); }

/// @brief Which layers collide with which: a symmetric matrix of bits (default: all of them).
struct CollisionLayers {
    std::array<u32, kCollisionLayerCount> mask; ///< `mask[a]` bit `b`: layers `a` and `b` collide.

    CollisionLayers() { mask.fill(kAllLayers); }
    /// @brief True if bodies of layers `a` and `b` collide.
    bool collides(u32 a, u32 b) const { return (mask[a & (kCollisionLayerCount - 1)] & layerBit(b)) != 0; }
    /// @brief Makes layers `a` and `b` collide or not (both directions).
    CollisionLayers& set(u32 a, u32 b, bool collide) {
        a &= kCollisionLayerCount - 1;
        b &= kCollisionLayerCount - 1;
        if (collide) {
            mask[a] |= layerBit(b);
            mask[b] |= layerBit(a);
        } else {
            mask[a] &= ~layerBit(b);
            mask[b] &= ~layerBit(a);
        }
        return *this;
    }
    bool operator==(const CollisionLayers&) const = default;
};
/// @}

/// @name Joints
/// @{
/// @brief Identifier of a joint inside a world.
struct JointId {
    u32 value = ~0u; ///< Backend-specific id; `~0u` = invalid.
    bool valid() const { return value != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const JointId&) const = default;
};

/// @brief How a joint connects two bodies (or a body and the world).
enum class JointType : u8 {
    Fixed,    ///< Glued: no relative motion at all.
    Point,    ///< Ball and socket: shared anchor, free rotation.
    Hinge,    ///< Door: rotation around the axis only.
    Slider,   ///< Drawer: translation along the axis only.
    Distance, ///< Rope / rod: the anchors stay between a minimum and a maximum distance.
};

/// @brief What a joint's motor drives.
enum class JointMotorMode : u8 {
    Off,      ///< No motor (free, within the limits).
    Velocity, ///< Reach `target` speed (hinge: rad/s; slider: m/s).
    Position, ///< Reach `target` position (hinge: rad from the start; slider: m from the start).
};

/// @brief Motor of a hinge or slider joint.
struct JointMotor {
    JointMotorMode mode = JointMotorMode::Off;
    f32 target = 0.0f;   ///< Speed or position, see JointMotorMode.
    f32 maxForce = 0.0f; ///< Strongest push (hinge: N·m; slider: N); 0 = unlimited.
    bool operator==(const JointMotor&) const = default;
};

/// @brief Joint creation parameters (anchor and axis in world space, at the bodies' current poses).
template <typename Position>
struct JointDescT {
    JointType type = JointType::Point;
    BodyId bodyA;               ///< First body.
    BodyId bodyB;               ///< Second body; invalid = attached to the world.
    Position anchor{};          ///< Shared anchor point (world); Distance joints: the attachment on A.
    Position anchorB{};         ///< Distance joints only: the attachment on B (or in the world).
    glm::vec3 axis{0.0f, 1.0f, 0.0f}; ///< Hinge axis / slider direction (world).
    bool limitsEnabled = false; ///< Hinge / slider: limit the motion to [limitMin, limitMax].
    f32 limitMin = 0.0f;        ///< Hinge: radians from the start; slider: meters from the start.
    f32 limitMax = 0.0f;        ///< See limitMin.
    f32 minDistance = -1.0f;    ///< Distance joints: shortest (−1 = the distance at creation).
    f32 maxDistance = -1.0f;    ///< Distance joints: longest (−1 = the distance at creation).
    f32 springFrequency = 0.0f; ///< Distance joints: softness of the limits (Hz; 0 = rigid).
    f32 springDamping = 0.0f;   ///< Damping ratio of that spring (0..1).
    JointMotor motor;           ///< Hinge / slider motor.
    f32 breakForce = 0.0f;      ///< Breaks when holding more than this (N); 0 = unbreakable.
    f32 breakTorque = 0.0f;     ///< Breaks when holding more than this torque (N·m); 0 = unbreakable.
    u64 userData = 0;           ///< Returned in JointBreakEvent.
};

/// @brief A joint broke (it held more than its break force or torque). It no longer acts but
///        stays allocated until destroyJoint().
struct JointBreakEvent {
    JointId joint;
    u64 userData = 0;
};
/// @}

/// @name Characters
/// @{
/// @brief Identifier of a character controller inside a world.
struct CharacterId {
    u32 value = ~0u; ///< Backend-specific id; `~0u` = invalid.
    bool valid() const { return value != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const CharacterId&) const = default;
};

/// @brief What a character stands on.
enum class GroundState : u8 {
    OnGround,      ///< On walkable ground.
    OnSteepGround, ///< Touching ground too steep to stand on (it slides).
    NotSupported,  ///< Touching something, but nothing below.
    InAir,         ///< Touching nothing.
};

/// @brief Character controller parameters: an upright capsule moved by velocity, not by forces.
template <typename Position>
struct CharacterDescT {
    f32 radius = 0.3f;              ///< Capsule radius (m).
    f32 halfHeight = 0.6f;          ///< Half length of the capsule's cylinder (total height = 2 × (halfHeight + radius)).
    Position position{};            ///< Capsule center.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< Facing (around the up axis).
    f32 maxSlopeAngle = glm::radians(50.0f); ///< Steepest walkable slope (rad).
    f32 stepHeight = 0.3f;          ///< Highest step it walks up (m); 0 = no stairs.
    f32 stickToFloor = 0.5f;        ///< Down steps it follows instead of flying off (m); 0 = off.
    f32 mass = 70.0f;               ///< Kg: how hard it pushes, and is pushed by, dynamic bodies.
    f32 maxPushForce = 100.0f;      ///< Strongest push on dynamic bodies (N).
    f32 gravityFactor = 1.0f;       ///< Gravity multiplier (0 = no gravity; it still stands on ground).
    u32 layer = 0;                  ///< Collision layer.
    u64 userData = 0;               ///< Returned in hits and contacts (the Scene stores the entity).
};

/// @brief A character after a step.
template <typename Position>
struct CharacterStateT {
    Position position{};                    ///< Capsule center.
    glm::vec3 velocity{0.0f};               ///< After collisions (sliding along walls, landing...).
    GroundState ground = GroundState::InAir; ///< What it stands on.
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f}; ///< Normal of the ground below.
    glm::vec3 groundVelocity{0.0f};         ///< Velocity of the ground (moving platforms).
    BodyId groundBody;                      ///< The body below (invalid in the air).
    u64 groundUserData = 0;                 ///< Its user data.
};
/// @}

/// @brief Which bodies a query may report.
struct QueryFilter {
    BodyId ignoreBody;           ///< Body to skip, e.g. the caller itself.
    CharacterId ignoreCharacter; ///< Character to skip (its capsule is seen by queries and sensors).
    bool includeSensors = false; ///< Report sensor bodies too.
    u32 layerMask = kAllLayers;  ///< Only bodies whose layer is in this mask (layerBit()).
};

/// @brief A body found by an overlap query.
struct OverlapHit {
    BodyId body;      ///< The body (invalid for a character's capsule).
    u64 userData = 0; ///< Its BodyDesc::userData (or the character's).
};

/// @brief Whether a contact event starts or ends a touch.
enum class ContactEventType : u8 {
    Began, ///< The pair started touching (or overlapping, for sensors).
    Ended, ///< The pair stopped touching (also when one of them is destroyed or stops simulating).
};

/// @brief World creation parameters.
struct WorldDesc {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f}; ///< Gravity acceleration (m/s²).
    u32 maxBodies = 65536;                 ///< Body capacity.
    u32 workerThreads = 0;                 ///< Simulation threads; 0 = hardware threads − 1.
    CollisionLayers layers;                ///< Which layers collide.
};


} // namespace nova::physics
