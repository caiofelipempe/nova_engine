#pragma once

/// @file
/// @brief Jolt Physics implementation of physics::PhysicsWorld.

#include "nova/physics/PhysicsWorld.h"

namespace nova::physics {

/// @brief PhysicsWorld backed by Jolt Physics.
///
/// Created through `PhysicsWorld::create(Backend::Jolt)`. Jolt headers are confined to the .cpp
/// (pimpl), so they never leak into engine or game code. Uses two object layers (static /
/// moving), a job-system thread pool and a 16 MB temp allocator. Jolt's global state (factory,
/// type registry) is reference-counted across worlds. See PhysicsWorld for member documentation.
class JoltPhysicsWorld final : public PhysicsWorld {
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

    glm::vec3 linearVelocity(BodyId body) const override;
    void setLinearVelocity(BodyId body, const glm::vec3& velocity) override;
    void setAngularVelocity(BodyId body, const glm::vec3& velocity) override;
    void addForce(BodyId body, const glm::vec3& force) override;
    void addImpulse(BodyId body, const glm::vec3& impulse) override;
    void addTorque(BodyId body, const glm::vec3& torque) override;
    bool isActive(BodyId body) const override;

    void setGravity(const glm::vec3& gravity) override;
    glm::vec3 gravity() const override;

    void step(f32 dt) override;
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                 const QueryFilter& filter) const override;
    bool sweep(const ShapeDesc& shape, const glm::vec3& position, const glm::quat& rotation,
               const glm::vec3& direction, f32 maxDistance, SweepHit& hit, const QueryFilter& filter) const override;
    std::vector<SweepHit> sweepAll(const ShapeDesc& shape, const glm::vec3& position, const glm::quat& rotation,
                                   const glm::vec3& direction, f32 maxDistance,
                                   const QueryFilter& filter) const override;
    bool sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                   const QueryFilter& filter) const override;
    std::vector<ContactEvent> drainContactEvents() override;

private:
    struct Impl;
    Scope<Impl> m_impl;
};

} // namespace nova::physics
