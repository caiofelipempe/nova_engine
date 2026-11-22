#pragma once

/// @file
/// @brief A minimal physics backend in native global positions: the skeleton a custom engine grows from.

#include "nova/physics/PhysicsWorld.h"

#include <set>
#include <utility>
#include <vector>

namespace nova::physics {

/// @brief Minimal rigid-body physics working in GlobalPosition natively (no floating origin).
///
/// The starting point for a custom integer engine, already plugged into the engine
/// (`Backend::Skeleton`) and passing the PhysicsWorld contract tests
/// (tests/PhysicsContractTests.cpp) for everything caps() declares:
/// - **Positions** are the bodies' GlobalPosition (int64 steps of the world unit). One region, at
///   the world origin, whatever the distance; wrap seams and barrier edges come from the active
///   WorldSpace (offsets between bodies are taken the shortest way around).
/// - **Linear motion is exact integer math:** velocities are fixed point (world steps · 2^-16 per
///   second), the step is a Q32 fraction of a second derived from the Duration's flicks, and
///   positions advance by the exact 128-bit product. Gravity, forces, impulses and damping enter
///   that fixed point once per step.
/// - **Collision** follows "global body, local shape": an exact int64 broad phase on bounding
///   spheres, then each pair's narrow phase runs in a frame anchored on one of its bodies
///   (`posB − posA` exact, then small floats). Pairs: sphere–sphere and sphere–box; barrier edges
///   are static planes. Speculative contacts and sequential impulses (restitution; no friction or
///   spin from contacts). Contact events when a pair starts touching.
/// - **Queries:** raycasts against spheres, boxes and barriers; sweeps of spheres against them.
/// - **Dormancy** follows SimulationFocus::radius, like the floating-origin adapter.
/// - Other shapes (capsule, cylinder, hull, mesh) are created and moved, but do not collide yet.
///
/// What a full engine adds is marked `TODO(engine)` in the source: fixed-point rotations and
/// solver (for `crossPlatformDeterministic`), GJK/EPA or SAT for the other shape pairs, friction
/// and angular response, sleeping, a sort-and-sweep or grid broad phase.
class SkeletonPhysicsWorld final : public PhysicsWorld {
public:
    /// @brief Fraction bits of a velocity, in world steps per second. With the default unit
    ///        (2^-32 m) speeds up to ±32 km/s are representable (they saturate beyond).
    static constexpr int kVelocityBits = 16;

    /// @param desc Gravity and body capacity (no worker threads: the skeleton is single-threaded).
    explicit SkeletonPhysicsWorld(const WorldDesc& desc = {});
    ~SkeletonPhysicsWorld() override;

    PhysicsCaps caps() const override;

    BodyId createBody(const BodyDesc& desc) override;
    void destroyBody(BodyId body) override;
    u32 bodyCount() const override { return m_alive; }
    void setTransform(BodyId body, const GlobalPosition& position, const glm::quat& rotation) override;
    void getTransform(BodyId body, GlobalPosition& position, glm::quat& rotation) const override;
    void moveKinematic(BodyId body, const GlobalPosition& position, const glm::quat& rotation, Duration dt) override;

    glm::vec3 linearVelocity(BodyId body) const override;
    void setLinearVelocity(BodyId body, const glm::vec3& velocity) override;
    glm::vec3 angularVelocity(BodyId body) const override;
    void setAngularVelocity(BodyId body, const glm::vec3& velocity) override;
    void addForce(BodyId body, const glm::vec3& force) override;
    void addImpulse(BodyId body, const glm::vec3& impulse) override;
    void addTorque(BodyId body, const glm::vec3& torque) override;
    bool isActive(BodyId body) const override;

    void setGravity(const glm::vec3& gravity) override { m_gravity = gravity; }
    glm::vec3 gravity() const override { return m_gravity; }
    void step(Duration dt) override;

    void setFocus(const SimulationFocus& focus) override;
    bool isDormant(BodyId body) const override;

    bool raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                 const QueryFilter& filter) const override;
    bool sweep(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
               const glm::vec3& direction, f32 maxDistance, SweepHit& hit, const QueryFilter& filter) const override;
    std::vector<SweepHit> sweepAll(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                                   const glm::vec3& direction, f32 maxDistance,
                                   const QueryFilter& filter) const override;
    bool sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                   const QueryFilter& filter) const override;
    std::vector<ContactEvent> drainContactEvents() override;

    /// @brief Fixed-point velocity of a body (world steps · 2^-kVelocityBits per second): exact state.
    glm::i64vec3 rawVelocity(BodyId body) const;

private:
    struct Body {
        bool alive = false;
        BodyDesc desc;                 // position/rotation: the current pose
        glm::i64vec3 velocity{0};      // fixed point, see kVelocityBits
        glm::vec3 angular{0.0f};       // rad/s, world space. TODO(engine): fixed point
        glm::vec3 force{0.0f};         // accumulated until the next step
        glm::vec3 torque{0.0f};
        f32 invMass = 0.0f;            // 0 for static/kinematic
        glm::vec3 invInertia{0.0f};    // local principal axes
        f64 reach = 0.0;               // bounding sphere radius around the body origin (m)
        bool dormant = false;
        bool hasTarget = false;        // kinematic: pose to reach by the end of the next step
        GlobalPosition targetPosition;
        glm::quat targetRotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    struct Contact; // one touching (or about to touch) pair, solved in a frame local to the pair

    Body* body(BodyId id);
    const Body* body(BodyId id) const;
    void updateDormancy(Body& b) const;
    void findContacts(f32 dt, std::vector<Contact>& out) const;
    void solve(std::vector<Contact>& contacts, f32 dt);
    bool castAgainst(const Body& target, u32 index, const ShapeDesc* shape, const GlobalPosition& origin,
                     const glm::quat& rotation, const glm::vec3& dir, f32 maxDistance, QueryHit& hit) const;
    void castAgainstBarriers(const ShapeDesc* shape, const GlobalPosition& origin, const glm::quat& rotation,
                             const glm::vec3& dir, f32 maxDistance, std::vector<QueryHit>& hits) const;
    std::vector<QueryHit> castAll(const ShapeDesc* shape, const GlobalPosition& origin, const glm::quat& rotation,
                                  const glm::vec3& direction, f32 maxDistance, const QueryFilter& filter) const;

    WorldDesc m_desc;
    glm::vec3 m_gravity;
    SimulationFocus m_focus;
    std::vector<Body> m_bodies;
    std::vector<u32> m_free;
    u32 m_alive = 0;
    std::set<std::pair<u64, u64>> m_touching; // pairs touching after the last step (walls: their userData)
    std::vector<ContactEvent> m_events;
};

} // namespace nova::physics
