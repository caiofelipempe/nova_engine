#pragma once

/// @file
/// @brief Backend-agnostic 3D rigid body physics interface.

#include "nova/core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

/// @brief 3D rigid body physics.
///
/// The Jolt implementation lives in physics/jolt/; a PhysX or Bullet backend would implement the
/// same PhysicsWorld interface. Games normally use it through Scene (RigidBodyComponent,
/// colliders, Scene::raycast, Scene::sweep...) rather than directly.
namespace nova::physics {

/// @brief Available physics backends.
enum class Backend {
    Jolt ///< Jolt Physics (multi-threaded).
};

/// @brief How a body moves.
enum class BodyType : u8 {
    Static,    ///< Never moves (level geometry).
    Kinematic, ///< Moved by code/animation; pushes dynamic bodies but is not pushed back.
    Dynamic,   ///< Fully simulated (gravity, forces, collisions).
};

/// @brief Collision shape primitives.
enum class ShapeType : u8 {
    Box,     ///< ShapeDesc::halfExtents.
    Sphere,  ///< ShapeDesc::radius.
    Capsule, ///< ShapeDesc::radius and ShapeDesc::halfHeight, aligned with local Y.
};

/// @brief Collision shape description (sizes are final, already scaled).
struct ShapeDesc {
    ShapeType type = ShapeType::Box; ///< Primitive.
    glm::vec3 halfExtents{0.5f};     ///< Box: half size on each axis.
    f32 radius = 0.5f;               ///< Sphere / Capsule radius.
    f32 halfHeight = 0.5f;           ///< Capsule: half length of the cylindrical part (along local Y).
    glm::vec3 offset{0.0f};          ///< Shape center relative to the body origin (local space).
};

/// @brief Rigid body creation parameters.
struct BodyDesc {
    ShapeDesc shape;                            ///< Collision shape.
    BodyType type = BodyType::Dynamic;          ///< Motion type.
    glm::vec3 position{0.0f};                   ///< Initial world position.
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< Initial world rotation.
    glm::vec3 linearVelocity{0.0f};             ///< Initial velocity (m/s).
    f32 mass = 1.0f;                            ///< Mass in kg (dynamic bodies only); inertia is derived from the shape.
    f32 friction = 0.5f;                        ///< Friction coefficient (typically 0..1).
    f32 restitution = 0.1f;                     ///< Bounciness (0 = no bounce, 1 = elastic).
    f32 linearDamping = 0.05f;                  ///< Linear velocity damping.
    f32 angularDamping = 0.05f;                 ///< Angular velocity damping.
    f32 gravityFactor = 1.0f;                   ///< Gravity multiplier (0 = floats).
    bool isSensor = false;                      ///< Reports contacts but has no collision response (trigger).
    u64 userData = 0;                           ///< Opaque value returned in hits/contacts (the Scene stores the entity).
};

/// @brief Identifier of a body inside a PhysicsWorld.
struct BodyId {
    u32 value = ~0u; ///< Backend-specific id; `~0u` = invalid.
    /// @brief True unless default-constructed. A valid id may refer to a destroyed body.
    bool valid() const { return value != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const BodyId&) const = default;
};

/// @brief Result of a raycast or a sweep (a shape moved along a direction, a.k.a. shape cast).
///
/// **Normals.** For smooth-vs-smooth contacts (a sphere on a face) all three agree; on edges and
/// corners they differ — e.g. a sphere grazing a box edge has a diagonal contact @ref normal,
/// while the box face there (@ref hitSurfaceNormal) is axis-aligned.
/// Surface normals are the normal of the face turned towards the other shape, so they stay well
/// defined when the contact point lies exactly on an edge or vertex.
struct QueryHit {
    BodyId body;                        ///< Hit body.
    u64 userData = 0;                   ///< BodyDesc::userData of the hit body.
    glm::vec3 point{0.0f};              ///< Contact point on the hit body (world space).
    glm::vec3 normal{0.0f};             ///< Contact normal (separation direction), pointing towards the query.
    glm::vec3 hitSurfaceNormal{0.0f};   ///< Surface normal of the hit body at @ref point.
    glm::vec3 sweptPoint{0.0f};         ///< Sweeps only: contact point on the swept shape at the moment of impact (zero for raycasts).
    glm::vec3 sweptSurfaceNormal{0.0f}; ///< Sweeps only: swept shape's surface normal at @ref sweptPoint (zero for raycasts).
    f32 distance = 0.0f;                ///< Distance travelled along the direction until contact.
    f32 fraction = 0.0f;                ///< `distance / maxDistance`; 0 = already overlapping at the start.
};
using RaycastHit = QueryHit; ///< Result of PhysicsWorld::raycast.
using SweepHit = QueryHit;   ///< Result of the PhysicsWorld::sweep family.

/// @brief Which bodies a query may report.
struct QueryFilter {
    BodyId ignoreBody;           ///< Body to skip, e.g. the caller itself.
    bool includeSensors = false; ///< Report sensor bodies too.
};

/// @brief A pair of bodies that started touching during the last step.
struct ContactEvent {
    BodyId bodyA;                   ///< First body.
    BodyId bodyB;                   ///< Second body.
    u64 userDataA = 0;              ///< BodyDesc::userData of A.
    u64 userDataB = 0;              ///< BodyDesc::userData of B.
    glm::vec3 normal{0.0f};         ///< Contact normal, from A towards B (direction to push B out of A).
    glm::vec3 pointA{0.0f};         ///< A contact point on A's surface.
    glm::vec3 pointB{0.0f};         ///< The matching contact point on B's surface.
    glm::vec3 surfaceNormalA{0.0f}; ///< A's surface normal at pointA (its face towards B).
    glm::vec3 surfaceNormalB{0.0f}; ///< B's surface normal at pointB (its face towards A).
};

/// @brief World creation parameters.
struct WorldDesc {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f}; ///< Gravity acceleration (m/s²).
    u32 maxBodies = 65536;                 ///< Body capacity.
    u32 workerThreads = 0;                 ///< Simulation threads; 0 = hardware threads − 1.
};

/// @brief A physics simulation: bodies, stepping, queries and contact events.
///
/// All body functions take a BodyId returned by createBody(); passing a destroyed id is a
/// programming error. Thread-safety: call from one thread (the simulation itself is multi-threaded).
class PhysicsWorld : NonCopyable {
public:
    /// @brief Creates a world with the given backend (must be compiled in, see `NOVA_PHYSICS_*`).
    static Scope<PhysicsWorld> create(Backend backend, const WorldDesc& desc = {});
    virtual ~PhysicsWorld() = default;

    /// @name Bodies
    /// @{
    /// @brief Creates a body and adds it to the world (dynamic/kinematic ones start active).
    /// @return The new body, or an invalid id if the shape is invalid or the world is full.
    virtual BodyId createBody(const BodyDesc& desc) = 0;
    /// @brief Removes and destroys a body. Invalid or already destroyed ids are ignored.
    virtual void destroyBody(BodyId body) = 0;
    /// @brief Number of bodies in the world.
    virtual u32 bodyCount() const = 0;

    /// @brief Teleports a body (no velocity is derived from the move). Wakes it up.
    virtual void setTransform(BodyId body, const glm::vec3& position, const glm::quat& rotation) = 0;
    /// @brief Current world position and rotation of a body.
    virtual void getTransform(BodyId body, glm::vec3& position, glm::quat& rotation) const = 0;
    /// @brief Kinematic bodies: reach this pose by the end of the next step of length `dt`.
    ///
    /// Unlike setTransform(), this derives the velocity needed, so the body pushes dynamic bodies correctly.
    virtual void moveKinematic(BodyId body, const glm::vec3& position, const glm::quat& rotation, f32 dt) = 0;
    /// @}

    /// @name Motion
    /// @{
    /// @brief Linear velocity in m/s.
    virtual glm::vec3 linearVelocity(BodyId body) const = 0;
    /// @brief Sets the linear velocity (m/s).
    virtual void setLinearVelocity(BodyId body, const glm::vec3& velocity) = 0;
    /// @brief Angular velocity in rad/s (world space).
    virtual glm::vec3 angularVelocity(BodyId body) const = 0;
    /// @brief Sets the angular velocity (rad/s, world space).
    virtual void setAngularVelocity(BodyId body, const glm::vec3& velocity) = 0;
    /// @brief Applies a force (N) at the center of mass during the next step.
    virtual void addForce(BodyId body, const glm::vec3& force) = 0;
    /// @brief Applies an instantaneous impulse (N·s) at the center of mass.
    virtual void addImpulse(BodyId body, const glm::vec3& impulse) = 0;
    /// @brief Applies a torque (N·m) during the next step.
    virtual void addTorque(BodyId body, const glm::vec3& torque) = 0;
    /// @brief False when the body is asleep (at rest) or static.
    virtual bool isActive(BodyId body) const = 0;
    /// @}

    /// @brief Sets the world gravity (m/s²).
    virtual void setGravity(const glm::vec3& gravity) = 0;
    /// @brief Current world gravity.
    virtual glm::vec3 gravity() const = 0;

    /// @brief Advances the simulation by exactly `dt` seconds (callers should use a fixed timestep).
    virtual void step(f32 dt) = 0;
    /// @brief Moves the world's origin to `newOrigin` (in current coordinates): every body is
    ///        translated by `-newOrigin`, keeping velocities and sleep state.
    ///
    /// Used for floating origins: keep the simulated region near (0, 0, 0), where floats are precise.
    virtual void shiftOrigin(const glm::vec3& newOrigin) = 0;

    /// @name Queries
    /// @{
    /// @brief Casts a ray and returns the closest hit.
    /// @param origin Ray start (world space).
    /// @param direction Ray direction (need not be normalized; zero returns false).
    /// @param maxDistance Maximum ray length.
    /// @param[out] hit Filled on success; the swept* fields are zero.
    /// @param filter Bodies to skip.
    /// @return True if something was hit.
    virtual bool raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                         const QueryFilter& filter = {}) const = 0;

    /// @brief Sweep test: moves `shape` along `direction` and returns the first body it would touch.
    /// @param shape Shape to sweep.
    /// @param position Start position of the shape.
    /// @param rotation Orientation of the shape (constant during the sweep).
    /// @param direction Sweep direction (need not be normalized; zero returns false).
    /// @param maxDistance Maximum travel distance.
    /// @param[out] hit Filled on success. `fraction == 0` means the shape already overlaps at the start.
    /// @param filter Bodies to skip.
    /// @return True if something was hit.
    virtual bool sweep(const ShapeDesc& shape, const glm::vec3& position, const glm::quat& rotation,
                       const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                       const QueryFilter& filter = {}) const = 0;
    /// @brief Same as sweep(), but returns every body along the way (one hit per body), closest first.
    virtual std::vector<SweepHit> sweepAll(const ShapeDesc& shape, const glm::vec3& position,
                                           const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                           const QueryFilter& filter = {}) const = 0;
    /// @brief Sweeps an existing body's own shape from its current pose, ignoring the body itself.
    ///
    /// Answers "if this body moves that way, what does it hit first?".
    /// @param body Body whose shape and pose are used.
    /// @param direction Sweep direction.
    /// @param maxDistance Maximum travel distance.
    /// @param[out] hit Filled on success.
    /// @param filter Extra bodies to skip (the body itself is always skipped).
    /// @return True if something was hit.
    virtual bool sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                           const QueryFilter& filter = {}) const = 0;
    /// @}

    /// @brief Returns (and clears) the contacts that began during the steps since the last call.
    virtual std::vector<ContactEvent> drainContactEvents() = 0;
};

} // namespace nova::physics
