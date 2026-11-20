#pragma once

#include "nova/core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <span>
#include <vector>

// Backend-agnostic 3D rigid body physics. The Jolt implementation lives in physics/jolt/;
// a PhysX or Bullet backend would implement the same interface.
namespace nova::physics {

enum class Backend { Jolt };

enum class BodyType : u8 {
    Static,    // never moves (level geometry)
    Kinematic, // moved by code/animation, pushes dynamic bodies
    Dynamic,   // fully simulated
};

enum class ShapeType : u8 { Box, Sphere, Capsule };

struct ShapeDesc {
    ShapeType type = ShapeType::Box;
    glm::vec3 halfExtents{0.5f}; // Box
    f32 radius = 0.5f;           // Sphere / Capsule
    f32 halfHeight = 0.5f;       // Capsule: half length of the cylindrical part (along local Y)
    glm::vec3 offset{0.0f};      // shape center relative to the body origin
};

struct BodyDesc {
    ShapeDesc shape;
    BodyType type = BodyType::Dynamic;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 linearVelocity{0.0f};
    f32 mass = 1.0f; // dynamic only (kg)
    f32 friction = 0.5f;
    f32 restitution = 0.1f;
    f32 linearDamping = 0.05f;
    f32 angularDamping = 0.05f;
    f32 gravityFactor = 1.0f;
    bool isSensor = false; // reports contacts, no collision response
    u64 userData = 0;      // the engine stores the ECS entity here
};

struct BodyId {
    u32 value = ~0u;
    bool valid() const { return value != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const BodyId&) const = default;
};

// Result of a raycast or a sweep (a shape moved along a direction, a.k.a. shape cast).
struct QueryHit {
    BodyId body;
    u64 userData = 0;
    glm::vec3 point{0.0f};  // contact point on the hit body (world space)
    glm::vec3 normal{0.0f}; // surface normal of the hit body, pointing back towards the query
    f32 distance = 0.0f;    // distance travelled along the direction until contact
    f32 fraction = 0.0f;    // distance / maxDistance; 0 = already overlapping at the start
};
using RaycastHit = QueryHit;
using SweepHit = QueryHit;

struct QueryFilter {
    BodyId ignoreBody;        // e.g. the caster itself
    bool includeSensors = false;
};

// A pair of bodies that started touching during the last step.
struct ContactEvent {
    u64 userDataA = 0;
    u64 userDataB = 0;
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f}; // from A towards B
};

struct WorldDesc {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    u32 maxBodies = 65536;
    u32 workerThreads = 0; // 0 = hardware threads - 1
};

class PhysicsWorld : NonCopyable {
public:
    static Scope<PhysicsWorld> create(Backend backend, const WorldDesc& desc = {});
    virtual ~PhysicsWorld() = default;

    virtual BodyId createBody(const BodyDesc& desc) = 0;
    virtual void destroyBody(BodyId body) = 0;
    virtual u32 bodyCount() const = 0;

    virtual void setTransform(BodyId body, const glm::vec3& position, const glm::quat& rotation) = 0;
    virtual void getTransform(BodyId body, glm::vec3& position, glm::quat& rotation) const = 0;
    // Kinematic bodies: reach this pose by the end of the next step (gives correct velocities).
    virtual void moveKinematic(BodyId body, const glm::vec3& position, const glm::quat& rotation, f32 dt) = 0;

    virtual glm::vec3 linearVelocity(BodyId body) const = 0;
    virtual void setLinearVelocity(BodyId body, const glm::vec3& velocity) = 0;
    virtual void setAngularVelocity(BodyId body, const glm::vec3& velocity) = 0;
    virtual void addForce(BodyId body, const glm::vec3& force) = 0;
    virtual void addImpulse(BodyId body, const glm::vec3& impulse) = 0;
    virtual void addTorque(BodyId body, const glm::vec3& torque) = 0;
    virtual bool isActive(BodyId body) const = 0;

    virtual void setGravity(const glm::vec3& gravity) = 0;
    virtual glm::vec3 gravity() const = 0;

    // Advances the simulation by exactly `dt` (callers use a fixed timestep).
    virtual void step(f32 dt) = 0;

    // ---- queries
    virtual bool raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                         const QueryFilter& filter = {}) const = 0;

    // Sweep test: moves `shape` (posed at position/rotation) along `direction` up to `maxDistance`
    // against the whole world and returns the closest hit.
    virtual bool sweep(const ShapeDesc& shape, const glm::vec3& position, const glm::quat& rotation,
                       const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                       const QueryFilter& filter = {}) const = 0;
    // Same sweep, every body hit along the way (one hit per body), closest first.
    virtual std::vector<SweepHit> sweepAll(const ShapeDesc& shape, const glm::vec3& position,
                                           const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                           const QueryFilter& filter = {}) const = 0;
    // Sweeps an existing body's own shape from its current pose, ignoring itself
    // ("will this body hit something if it moves there?").
    virtual bool sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                           const QueryFilter& filter = {}) const = 0;

    // Sweep tested only against the listed bodies (no broadphase over the rest of the world).
    // Every listed body is tested, sensors included; invalid/removed ids are skipped.
    virtual bool sweepAgainst(const ShapeDesc& shape, const glm::vec3& position, const glm::quat& rotation,
                              const glm::vec3& direction, f32 maxDistance, std::span<const BodyId> bodies,
                              SweepHit& hit) const = 0;
    virtual std::vector<SweepHit> sweepAllAgainst(const ShapeDesc& shape, const glm::vec3& position,
                                                  const glm::quat& rotation, const glm::vec3& direction,
                                                  f32 maxDistance, std::span<const BodyId> bodies) const = 0;

    // Contacts that began during the steps since the last call.
    virtual std::vector<ContactEvent> drainContactEvents() = 0;
};

} // namespace nova::physics
