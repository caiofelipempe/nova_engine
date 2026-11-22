#pragma once

/// @file
/// @brief Jolt Physics implementation of physics::local::World (float, origin-relative).

#include "nova/physics/backend/LocalPhysicsWorld.h"

namespace nova::physics::local {

/// @brief local::World backed by Jolt Physics (wrapped in a FloatingOriginWorld by PhysicsWorld::create).
///
/// Created through `local::World::create(Backend::Jolt)`. Jolt headers are confined to the .cpp
/// (pimpl), so they never leak into engine or game code. Uses two object layers (static /
/// moving), a job-system thread pool and a 16 MB temp allocator. Jolt's global state (factory,
/// type registry) is reference-counted across worlds. See local::World for member documentation.
class JoltPhysicsWorld final : public World {
public:
    /// @param desc World parameters.
    explicit JoltPhysicsWorld(const WorldDesc& desc);
    /// @brief Destroys every remaining body, then the world.
    ~JoltPhysicsWorld() override;

    BodyId createBody(const BodyDesc& desc) override;
    void destroyBody(BodyId body) override;
    u32 bodyCount() const override;

    void setTransform(BodyId body, const glm::vec3& position, const glm::quat& rotation) override;
    void getTransform(BodyId body, glm::vec3& position, glm::quat& rotation) const override;
    void moveKinematic(BodyId body, const glm::vec3& position, const glm::quat& rotation, f32 dt) override;
    void setBodyProperties(BodyId body, const BodyDesc& desc) override;

    glm::vec3 linearVelocity(BodyId body) const override;
    glm::vec3 angularVelocity(BodyId body) const override;
    void setLinearVelocity(BodyId body, const glm::vec3& velocity) override;
    void setAngularVelocity(BodyId body, const glm::vec3& velocity) override;
    void addForce(BodyId body, const glm::vec3& force) override;
    void addImpulse(BodyId body, const glm::vec3& impulse) override;
    void addTorque(BodyId body, const glm::vec3& torque) override;
    bool isActive(BodyId body) const override;

    JointId createJoint(const JointDesc& desc) override;
    void destroyJoint(JointId joint) override;
    void setJointMotor(JointId joint, const JointMotor& motor) override;
    f32 jointPosition(JointId joint) const override;
    JointLoad jointLoad(JointId joint) const override;

    void setGravity(const glm::vec3& gravity) override;
    void setCollisionLayers(const CollisionLayers& layers) override;
    glm::vec3 gravity() const override;

    void step(f32 dt) override;
    void shiftOrigin(const glm::vec3& newOrigin) override;
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                 const QueryFilter& filter) const override;
    bool sweep(const ShapeDesc& shape, const glm::vec3& position, const glm::quat& rotation,
               const glm::vec3& direction, f32 maxDistance, SweepHit& hit, const QueryFilter& filter) const override;
    std::vector<SweepHit> sweepAll(const ShapeDesc& shape, const glm::vec3& position, const glm::quat& rotation,
                                   const glm::vec3& direction, f32 maxDistance,
                                   const QueryFilter& filter) const override;
    bool sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                   const QueryFilter& filter) const override;
    std::vector<OverlapHit> overlap(const ShapeDesc& shape, const glm::vec3& position, const glm::quat& rotation,
                                    const QueryFilter& filter) const override;
    std::vector<ContactEvent> drainContactEvents() override;

private:
    struct Impl;
    Scope<Impl> m_impl;
};

} // namespace nova::physics::local
