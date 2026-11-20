#pragma once

#include "nova/physics/PhysicsWorld.h"

namespace nova::physics {

// Jolt Physics backend. Jolt headers are confined to the .cpp (pimpl).
class JoltPhysicsWorld final : public PhysicsWorld {
public:
    explicit JoltPhysicsWorld(const WorldDesc& desc);
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
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance,
                 RaycastHit& hit) const override;
    std::vector<ContactEvent> drainContactEvents() override;

private:
    struct Impl;
    Scope<Impl> m_impl;
};

} // namespace nova::physics
