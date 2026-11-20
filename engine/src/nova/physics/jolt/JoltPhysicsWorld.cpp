#include "nova/physics/jolt/JoltPhysicsWorld.h"

#include "nova/core/Log.h"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>

namespace nova::physics {

namespace {

// ---------------------------------------------------------------- layers --
// Two object layers: static geometry never needs to test against itself.
namespace Layers {
constexpr JPH::ObjectLayer NonMoving = 0;
constexpr JPH::ObjectLayer Moving = 1;
constexpr JPH::ObjectLayer Count = 2;
} // namespace Layers

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer NonMoving(0);
constexpr JPH::BroadPhaseLayer Moving(1);
constexpr JPH::uint Count = 2;
} // namespace BroadPhaseLayers

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == Layers::Moving || b == Layers::Moving;
    }
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::NonMoving ? BroadPhaseLayers::NonMoving : BroadPhaseLayers::Moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::NonMoving ? "NonMoving" : "Moving";
    }
#endif
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        return layer == Layers::Moving || bp == BroadPhaseLayers::Moving;
    }
};

// Collects "contact began" events. Called from Jolt worker threads.
class ContactListener final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        ContactEvent e;
        e.userDataA = a.GetUserData();
        e.userDataB = b.GetUserData();
        JPH::RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
        e.point = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()), static_cast<f32>(p.GetZ())};
        e.normal = {manifold.mWorldSpaceNormal.GetX(), manifold.mWorldSpaceNormal.GetY(),
                    manifold.mWorldSpaceNormal.GetZ()};
        std::lock_guard lock(mutex);
        events.push_back(e);
    }

    std::mutex mutex;
    std::vector<ContactEvent> events;
};

// ------------------------------------------------------- global Jolt init --
int g_joltRefCount = 0;

void joltTrace(const char* fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    NOVA_CORE_TRACE("[jolt] {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssert(const char* expr, const char* msg, const char* file, JPH::uint line) {
    NOVA_CORE_ERROR("[jolt] assert '{}' {} ({}:{})", expr, msg ? msg : "", file, line);
    return true; // break into the debugger
}
#endif

void acquireJolt() {
    if (g_joltRefCount++ > 0) return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = joltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssert;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void releaseJolt() {
    if (--g_joltRefCount > 0) return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// ----------------------------------------------------------- conversions --
JPH::Vec3 toJ(const glm::vec3& v) { return {v.x, v.y, v.z}; }
JPH::RVec3 toJR(const glm::vec3& v) { return {v.x, v.y, v.z}; }
JPH::Quat toJ(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w).Normalized(); }
glm::vec3 toG(const JPH::Vec3& v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
#ifdef JPH_DOUBLE_PRECISION
glm::vec3 toG(const JPH::RVec3& v) {
    return {static_cast<f32>(v.GetX()), static_cast<f32>(v.GetY()), static_cast<f32>(v.GetZ())};
}
#endif
glm::quat toG(const JPH::Quat& q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }

JPH::BodyID toJ(BodyId id) { return JPH::BodyID(id.value); }

JPH::EMotionType toJ(BodyType t) {
    switch (t) {
        case BodyType::Static: return JPH::EMotionType::Static;
        case BodyType::Kinematic: return JPH::EMotionType::Kinematic;
        case BodyType::Dynamic: return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Static;
}

JPH::ShapeRefC buildShape(const ShapeDesc& s) {
    JPH::ShapeSettings::ShapeResult result;
    constexpr f32 kMin = 0.001f;
    switch (s.type) {
        case ShapeType::Box: {
            glm::vec3 he = glm::max(s.halfExtents, glm::vec3(kMin));
            // Convex radius must not exceed the smallest half extent.
            f32 convex = std::min(JPH::cDefaultConvexRadius, std::min({he.x, he.y, he.z}) * 0.5f);
            result = JPH::BoxShapeSettings(toJ(he), convex).Create();
            break;
        }
        case ShapeType::Sphere: result = JPH::SphereShapeSettings(std::max(s.radius, kMin)).Create(); break;
        case ShapeType::Capsule:
            result = JPH::CapsuleShapeSettings(std::max(s.halfHeight, kMin), std::max(s.radius, kMin)).Create();
            break;
    }
    if (result.HasError()) {
        NOVA_CORE_ERROR("[physics] shape creation failed: {}", result.GetError().c_str());
        return nullptr;
    }
    JPH::ShapeRefC shape = result.Get();
    if (s.offset != glm::vec3(0.0f)) {
        auto offsetResult = JPH::RotatedTranslatedShapeSettings(toJ(s.offset), JPH::Quat::sIdentity(), shape).Create();
        if (!offsetResult.HasError()) shape = offsetResult.Get();
    }
    return shape;
}

} // namespace

// ------------------------------------------------------------------ Impl --
struct JoltPhysicsWorld::Impl {
    BroadPhaseLayerInterface broadPhaseLayers;
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhase;
    ObjectLayerPairFilter objectPairs;
    ContactListener contacts;
    Scope<JPH::TempAllocatorImpl> tempAllocator;
    Scope<JPH::JobSystemThreadPool> jobSystem;
    JPH::PhysicsSystem system;
    bool broadPhaseDirty = false;

    JPH::BodyInterface& bodies() { return system.GetBodyInterface(); }
    const JPH::BodyInterface& bodies() const { return system.GetBodyInterface(); }
};

JoltPhysicsWorld::JoltPhysicsWorld(const WorldDesc& desc) {
    acquireJolt();
    m_impl = makeScope<Impl>();

    u32 threads = desc.workerThreads;
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    m_impl->tempAllocator = makeScope<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    m_impl->jobSystem = makeScope<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                                             static_cast<int>(threads));

    m_impl->system.Init(desc.maxBodies, 0, desc.maxBodies, desc.maxBodies / 4, m_impl->broadPhaseLayers,
                        m_impl->objectVsBroadPhase, m_impl->objectPairs);
    m_impl->system.SetContactListener(&m_impl->contacts);
    m_impl->system.SetGravity(toJ(desc.gravity));
    NOVA_CORE_INFO("Physics: Jolt {}.{}.{} ({} worker threads)", JPH_VERSION_MAJOR, JPH_VERSION_MINOR,
                   JPH_VERSION_PATCH, threads);
}

JoltPhysicsWorld::~JoltPhysicsWorld() {
    // Remove and destroy every body before the system goes away.
    JPH::BodyIDVector ids;
    m_impl->system.GetBodies(ids);
    if (!ids.empty()) {
        m_impl->bodies().RemoveBodies(ids.data(), static_cast<int>(ids.size()));
        m_impl->bodies().DestroyBodies(ids.data(), static_cast<int>(ids.size()));
    }
    m_impl.reset();
    releaseJolt();
}

BodyId JoltPhysicsWorld::createBody(const BodyDesc& desc) {
    JPH::ShapeRefC shape = buildShape(desc.shape);
    if (!shape) return {};

    const bool moving = desc.type != BodyType::Static;
    JPH::BodyCreationSettings bcs(shape, toJR(desc.position), toJ(desc.rotation), toJ(desc.type),
                                  moving ? Layers::Moving : Layers::NonMoving);
    bcs.mFriction = desc.friction;
    bcs.mRestitution = desc.restitution;
    bcs.mLinearDamping = desc.linearDamping;
    bcs.mAngularDamping = desc.angularDamping;
    bcs.mGravityFactor = desc.gravityFactor;
    bcs.mIsSensor = desc.isSensor;
    bcs.mUserData = desc.userData;
    bcs.mLinearVelocity = toJ(desc.linearVelocity);
    if (desc.type == BodyType::Dynamic) {
        bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bcs.mMassPropertiesOverride.mMass = std::max(desc.mass, 0.001f);
    }
    if (desc.type == BodyType::Kinematic && desc.isSensor) bcs.mCollideKinematicVsNonDynamic = true;

    JPH::BodyID id = m_impl->bodies().CreateAndAddBody(
        bcs, moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (id.IsInvalid()) {
        NOVA_CORE_ERROR("[physics] body limit reached");
        return {};
    }
    m_impl->broadPhaseDirty = true;
    return {id.GetIndexAndSequenceNumber()};
}

void JoltPhysicsWorld::destroyBody(BodyId body) {
    if (!body) return;
    JPH::BodyID id = toJ(body);
    if (!m_impl->bodies().IsAdded(id)) return;
    m_impl->bodies().RemoveBody(id);
    m_impl->bodies().DestroyBody(id);
}

u32 JoltPhysicsWorld::bodyCount() const { return m_impl->system.GetNumBodies(); }

void JoltPhysicsWorld::setTransform(BodyId body, const glm::vec3& position, const glm::quat& rotation) {
    m_impl->bodies().SetPositionAndRotation(toJ(body), toJR(position), toJ(rotation), JPH::EActivation::Activate);
}

void JoltPhysicsWorld::getTransform(BodyId body, glm::vec3& position, glm::quat& rotation) const {
    JPH::RVec3 p;
    JPH::Quat q;
    m_impl->bodies().GetPositionAndRotation(toJ(body), p, q);
    position = toG(p);
    rotation = toG(q);
}

void JoltPhysicsWorld::moveKinematic(BodyId body, const glm::vec3& position, const glm::quat& rotation, f32 dt) {
    m_impl->bodies().MoveKinematic(toJ(body), toJR(position), toJ(rotation), dt);
}

glm::vec3 JoltPhysicsWorld::linearVelocity(BodyId body) const {
    return toG(m_impl->bodies().GetLinearVelocity(toJ(body)));
}
void JoltPhysicsWorld::setLinearVelocity(BodyId body, const glm::vec3& v) {
    m_impl->bodies().SetLinearVelocity(toJ(body), toJ(v));
}
void JoltPhysicsWorld::setAngularVelocity(BodyId body, const glm::vec3& v) {
    m_impl->bodies().SetAngularVelocity(toJ(body), toJ(v));
}
void JoltPhysicsWorld::addForce(BodyId body, const glm::vec3& f) { m_impl->bodies().AddForce(toJ(body), toJ(f)); }
void JoltPhysicsWorld::addImpulse(BodyId body, const glm::vec3& i) { m_impl->bodies().AddImpulse(toJ(body), toJ(i)); }
void JoltPhysicsWorld::addTorque(BodyId body, const glm::vec3& t) { m_impl->bodies().AddTorque(toJ(body), toJ(t)); }
bool JoltPhysicsWorld::isActive(BodyId body) const { return m_impl->bodies().IsActive(toJ(body)); }

void JoltPhysicsWorld::setGravity(const glm::vec3& g) { m_impl->system.SetGravity(toJ(g)); }
glm::vec3 JoltPhysicsWorld::gravity() const { return toG(m_impl->system.GetGravity()); }

void JoltPhysicsWorld::step(f32 dt) {
    if (m_impl->broadPhaseDirty) {
        m_impl->system.OptimizeBroadPhase();
        m_impl->broadPhaseDirty = false;
    }
    JPH::EPhysicsUpdateError err =
        m_impl->system.Update(dt, 1, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
    if (err != JPH::EPhysicsUpdateError::None)
        NOVA_CORE_WARN("[physics] update error flags 0x{:x} (increase world limits)", static_cast<u32>(err));
}

bool JoltPhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance,
                               RaycastHit& hit) const {
    glm::vec3 dir = glm::normalize(direction);
    JPH::RRayCast ray{toJR(origin), toJ(dir * maxDistance)};
    JPH::RayCastResult result;
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result)) return false;

    JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    hit.body = {result.mBodyID.GetIndexAndSequenceNumber()};
    hit.point = toG(point);
    hit.distance = result.mFraction * maxDistance;
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        hit.userData = body.GetUserData();
        hit.normal = toG(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
    }
    return true;
}

std::vector<ContactEvent> JoltPhysicsWorld::drainContactEvents() {
    std::lock_guard lock(m_impl->contacts.mutex);
    std::vector<ContactEvent> out;
    out.swap(m_impl->contacts.events);
    return out;
}

} // namespace nova::physics
