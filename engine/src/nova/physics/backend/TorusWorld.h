#pragma once

/// @file
/// @brief Global-position physics on the 16 km torus, over one float world (Jolt).

#include "nova/physics/PhysicsWorld.h"
#include "nova/physics/backend/LocalPhysicsWorld.h"

#include <map>
#include <set>
#include <vector>

namespace nova::physics {

/// @brief Implements PhysicsWorld over a single local::World (Jolt), directly.
///
/// The whole world (16 384 m per axis, WorldSpace) fits the range where float physics is
/// precise, so bodies, joints and characters are handed to the float engine as they are: their
/// ids and user data are the engine's own. Positions are float meters from a window **origin**,
/// and the only thing this layer adds is the torus:
/// - the origin follows the focus (recentered when it is more than 2 km away, by an amount
///   floats represent exactly), so what is near the focus is never on the far side of the seam;
/// - bodies and characters more than half a lap from the origin are moved one lap, to their
///   nearest copy (same place on the torus, velocity kept);
/// - joints to the world are placed again after a recenter (Jolt keeps their anchor in world
///   coordinates); breakable joints break here, from the load of the last step.
class TorusWorld final : public PhysicsWorld {
public:
    /// @param backend Float backend creating the local world.
    /// @param desc Parameters of the local world.
    TorusWorld(Backend backend, const WorldDesc& desc);
    ~TorusWorld() override;

    BodyId createBody(const BodyDesc& desc) override;
    void destroyBody(BodyId body) override;
    u32 bodyCount() const override;
    void setTransform(BodyId body, const GlobalPosition& position, const glm::quat& rotation) override;
    void getTransform(BodyId body, GlobalPosition& position, glm::quat& rotation) const override;
    void moveKinematic(BodyId body, const GlobalPosition& position, const glm::quat& rotation, Duration dt) override;
    void setBodyProperties(BodyId body, const BodyDesc& desc) override;

    glm::vec3 linearVelocity(BodyId body) const override;
    void setLinearVelocity(BodyId body, const glm::vec3& velocity) override;
    glm::vec3 angularVelocity(BodyId body) const override;
    void setAngularVelocity(BodyId body, const glm::vec3& velocity) override;
    void addForce(BodyId body, const glm::vec3& force) override;
    void addImpulse(BodyId body, const glm::vec3& impulse) override;
    void addTorque(BodyId body, const glm::vec3& torque) override;
    bool isActive(BodyId body) const override;

    JointId createJoint(const JointDesc& desc) override;
    void destroyJoint(JointId joint) override;
    void setJointMotor(JointId joint, const JointMotor& motor) override;
    f32 jointPosition(JointId joint) const override;
    bool isJointBroken(JointId joint) const override;
    std::vector<JointBreakEvent> drainJointBreaks() override;

    CharacterId createCharacter(const CharacterDesc& desc) override;
    void destroyCharacter(CharacterId character) override;
    void setCharacterVelocity(CharacterId character, const glm::vec3& velocity) override;
    void setCharacterTransform(CharacterId character, const GlobalPosition& position, const glm::quat& rotation) override;
    CharacterState characterState(CharacterId character) const override;

    void setGravity(const glm::vec3& gravity) override;
    glm::vec3 gravity() const override;
    void setCollisionLayers(const CollisionLayers& layers) override;
    void step(Duration dt) override;
    void setFocus(const GlobalPosition& focus) override;
    GlobalPosition origin() const override { return m_origin; }

    bool raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                 const QueryFilter& filter) const override;
    bool sweep(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
               const glm::vec3& direction, f32 maxDistance, SweepHit& hit, const QueryFilter& filter) const override;
    std::vector<SweepHit> sweepAll(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                                   const glm::vec3& direction, f32 maxDistance,
                                   const QueryFilter& filter) const override;
    bool sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                   const QueryFilter& filter) const override;
    std::vector<OverlapHit> overlap(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                                    const QueryFilter& filter) const override;
    std::vector<ContactEvent> drainContactEvents() override;

private:
    struct Joint {
        bool alive = false;
        bool broken = false;
        JointDesc desc;                    // global description (motor kept current)
        glm::vec3 localA{0.0f};            // anchor in A's frame
        glm::vec3 localB{0.0f};            // B's attachment in B's frame (bodies only)
        GlobalPosition worldAttach;        // the attachment when B is the world
        glm::vec3 axisA{0.0f, 1.0f, 0.0f}; // axis in A's frame
        JointId local;                     // in the float world (invalid once broken)
    };

    glm::vec3 toLocal(const GlobalPosition& p) const { return (p - m_origin).toVec3(); }
    GlobalPosition toGlobal(const glm::vec3& p) const { return m_origin + p; }
    QueryHit toGlobal(const local::QueryHit& hit, const GlobalPosition& queryOrigin, bool ray) const;
    void recenter(const GlobalPosition& target);
    void rewrap(bool everything);
    void placeJoint(u32 index);

    Scope<local::World> m_world;
    GlobalPosition m_origin;          // what float (0, 0, 0) is on the torus
    std::map<u32, BodyType> m_bodies; // live bodies by id (ordered: re-wrapping is deterministic)
    std::set<u32> m_characters;       // live characters by id
    std::vector<Joint> m_joints;
    std::vector<u32> m_freeJoints;
    std::vector<JointBreakEvent> m_jointBreaks;
};

} // namespace nova::physics
