#pragma once

/// @file
/// @brief The physics contract the engine uses: bodies at exact global positions, body-local shapes.

#include "nova/math/GlobalPosition.h"
#include "nova/math/Time.h"
#include "nova/physics/PhysicsTypes.h"

#include <span>
#include <vector>

/// @brief 3D rigid body physics.
///
/// The engine talks to physics only through PhysicsWorld, in **global positions**: a body's
/// position is a GlobalPosition (exact integer), its shape is local to the body (ShapeDesc), and
/// queries and contacts report GlobalPositions. How a backend reaches that precision is its own
/// business:
/// - float engines (Jolt) implement local::World and are wrapped by FloatingOriginWorld, which
///   keeps their origin near the action (regions, rebasing, dormant bodies, barrier walls);
/// - an engine that works in integers natively (a custom one, or a third-party one) implements
///   PhysicsWorld directly, without any floating origin.
///
/// Games normally use it through Scene (RigidBodyComponent, colliders, Scene::raycast...).
namespace nova::physics {

/// @brief Rigid body creation parameters.
struct BodyDesc {
    ShapeDesc shape;                            ///< Collision shape (local to the body).
    BodyType type = BodyType::Dynamic;          ///< Motion type.
    GlobalPosition position;                    ///< Initial world position (exact).
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

/// @brief Result of a raycast or a sweep (a shape moved along a direction).
///
/// For smooth-vs-smooth contacts all normals agree; on edges and corners they differ (see the
/// field docs). Surface normals are the face turned towards the other shape.
struct QueryHit {
    BodyId body;                        ///< Hit body (invalid for a world barrier edge).
    u64 userData = 0;                   ///< BodyDesc::userData of the hit body (kWorldBoundaryUserData... for edges).
    GlobalPosition point;               ///< Contact point on the hit body.
    glm::vec3 normal{0.0f};             ///< Contact normal (separation direction), pointing towards the query.
    glm::vec3 hitSurfaceNormal{0.0f};   ///< Surface normal of the hit body at @ref point.
    GlobalPosition sweptPoint;          ///< Sweeps: contact point on the swept shape (the query origin for raycasts).
    glm::vec3 sweptSurfaceNormal{0.0f}; ///< Sweeps: swept shape's surface normal at @ref sweptPoint (zero for raycasts).
    f32 distance = 0.0f;                ///< Distance travelled along the direction until contact.
    f32 fraction = 0.0f;                ///< `distance / maxDistance`; 0 = already overlapping at the start.
};
using RaycastHit = QueryHit; ///< Result of PhysicsWorld::raycast.
using SweepHit = QueryHit;   ///< Result of the PhysicsWorld::sweep family.

/// @brief A pair of bodies that started touching during the last step.
struct ContactEvent {
    BodyId bodyA;                   ///< First body (invalid for a world barrier edge).
    BodyId bodyB;                   ///< Second body.
    u64 userDataA = 0;              ///< BodyDesc::userData of A.
    u64 userDataB = 0;              ///< BodyDesc::userData of B.
    glm::vec3 normal{0.0f};         ///< Contact normal, from A towards B (direction to push B out of A).
    GlobalPosition pointA;          ///< A contact point on A's surface.
    GlobalPosition pointB;          ///< The matching contact point on B's surface.
    glm::vec3 surfaceNormalA{0.0f}; ///< A's surface normal at pointA (its face towards B).
    glm::vec3 surfaceNormalB{0.0f}; ///< B's surface normal at pointB (its face towards A).
};

/// @brief Where and how far the simulation runs (hints; backends may ignore what they do not need).
struct SimulationFocus {
    std::vector<GlobalPosition> foci; ///< Points of interest (players, camera). Empty = none.
    /// @brief Dynamic bodies farther than this (m) from every focus may stop simulating (dormant,
    ///        velocity kept). 0 = unlimited.
    f64 radius = 4096.0;
    f64 rebaseDistance = 1024.0;   ///< Floating-origin backends: move the origin when foci drift this far (m).
    f64 barrierWallRange = 8192.0; ///< Floating-origin backends: barrier edges closer than this (m) become walls.
};

/// @brief A physics simulation in global positions: bodies, stepping, queries and contact events.
///
/// All body functions take a BodyId returned by createBody(); passing a destroyed id is a
/// programming error. Thread-safety: call from one thread (the simulation itself may be threaded).
/// Barrier edges of the active WorldSpace are enforced by the backend; contacts and hits against
/// them carry `kWorldBoundaryUserData` and an invalid body.
class PhysicsWorld : NonCopyable {
public:
    /// @brief Creates a world with the given backend (must be compiled in, see `NOVA_PHYSICS_*`).
    ///        Float backends come wrapped in a FloatingOriginWorld.
    static Scope<PhysicsWorld> create(Backend backend, const WorldDesc& desc = {});
    virtual ~PhysicsWorld() = default;

    /// @brief True if `desc` describes a body every backend accepts: positive sizes, points for a
    ///        hull, triangles for a mesh, no dynamic mesh. Logs why when it is not.
    static bool isValidBody(const BodyDesc& desc);

    /// @name Bodies
    /// @{
    /// @brief Creates a body (dynamic/kinematic ones start active).
    /// @return The new body, or an invalid id if the shape is invalid (isValidBody) or the world is full.
    virtual BodyId createBody(const BodyDesc& desc) = 0;
    /// @brief Removes and destroys a body. Invalid or already destroyed ids are ignored.
    virtual void destroyBody(BodyId body) = 0;
    /// @brief Number of bodies.
    virtual u32 bodyCount() const = 0;
    /// @brief Teleports a body (no velocity is derived from the move). Wakes it up.
    virtual void setTransform(BodyId body, const GlobalPosition& position, const glm::quat& rotation) = 0;
    /// @brief Current position and rotation of a body.
    virtual void getTransform(BodyId body, GlobalPosition& position, glm::quat& rotation) const = 0;
    /// @brief Kinematic bodies: reach this pose by the end of the next step of length `dt`
    ///        (derives the velocity, so it pushes dynamic bodies correctly).
    virtual void moveKinematic(BodyId body, const GlobalPosition& position, const glm::quat& rotation, Duration dt) = 0;
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
    /// @brief False when the body is asleep, static or dormant.
    virtual bool isActive(BodyId body) const = 0;
    /// @}

    /// @brief Sets the world gravity (m/s²).
    virtual void setGravity(const glm::vec3& gravity) = 0;
    /// @brief Current world gravity.
    virtual glm::vec3 gravity() const = 0;

    /// @brief Advances the simulation by exactly `dt` (callers should use a fixed timestep).
    virtual void step(Duration dt) = 0;

    /// @name Simulation focus
    /// @{
    /// @brief Where the action is (see SimulationFocus). Called by the Scene every update.
    virtual void setFocus(const SimulationFocus& focus) = 0;
    /// @brief True if the body is outside the simulated area (frozen; velocity kept).
    virtual bool isDormant(BodyId body) const { return false; }
    /// @brief Number of independent simulation regions (1 for backends without regions).
    virtual u32 regionCount() const { return 1; }
    /// @brief Origin of a region's local coordinates (the world origin for global backends).
    virtual GlobalPosition regionOrigin(u32 region) const { return {}; }
    /// @}

    /// @name Queries
    /// @{
    /// @brief Casts a ray and returns the closest hit (`direction` need not be normalized).
    virtual bool raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                         const QueryFilter& filter = {}) const = 0;
    /// @brief Moves `shape` along `direction` and returns the first body it would touch.
    virtual bool sweep(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                       const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                       const QueryFilter& filter = {}) const = 0;
    /// @brief Same as sweep(), but every body along the way (one hit each), closest first.
    virtual std::vector<SweepHit> sweepAll(const ShapeDesc& shape, const GlobalPosition& position,
                                           const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                           const QueryFilter& filter = {}) const = 0;
    /// @brief Sweeps a body's own shape from its current pose, ignoring the body itself.
    virtual bool sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                           const QueryFilter& filter = {}) const = 0;
    /// @}

    /// @brief Returns (and clears) the contacts that began during the steps since the last call.
    virtual std::vector<ContactEvent> drainContactEvents() = 0;
};

} // namespace nova::physics
