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
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
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
#include <optional>
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

// Surface normal of a shape at a contact, robust on edges and corners.
//
// A point-based normal (Shape::GetSurfaceNormal) is ambiguous when the contact point lies on an edge
// or vertex — e.g. it can return the side face of a box that landed on its bottom edge. Instead we
// take the face that faces the other body the most (the "supporting face" along `towardOther`) and
// use its plane normal. Curved shapes have no such polygon (spheres return none, capsules a
// segment), and there the point-based `pointNormal` is exact.
JPH::Vec3 contactSurfaceNormal(const JPH::Shape::SupportingFace& face, JPH::Vec3Arg towardOther,
                               JPH::Vec3Arg pointNormal) {
    if (face.size() >= 3) {
        // Newell's method: robust plane normal for any (possibly slightly non-planar) polygon.
        JPH::Vec3 n = JPH::Vec3::sZero();
        for (JPH::uint i = 0; i < face.size(); ++i) {
            JPH::Vec3 a = face[i], b = face[(i + 1) % face.size()];
            n += JPH::Vec3((a.GetY() - b.GetY()) * (a.GetZ() + b.GetZ()), (a.GetZ() - b.GetZ()) * (a.GetX() + b.GetX()),
                           (a.GetX() - b.GetX()) * (a.GetY() + b.GetY()));
        }
        if (n.LengthSq() > 1e-12f) {
            n = n.Normalized();
            return n.Dot(towardOther) < 0.0f ? -n : n; // outward = towards the other body
        }
    }
    return pointNormal.LengthSq() > 1e-12f ? pointNormal.Normalized() : pointNormal;
}

// Same, for a body (must be locked by the caller or by Jolt's callback).
JPH::Vec3 bodyContactSurfaceNormal(const JPH::Body& body, const JPH::SubShapeID& subShape, JPH::RVec3Arg point,
                                   JPH::Vec3Arg towardOther) {
    JPH::Shape::SupportingFace face;
    body.GetTransformedShape().GetSupportingFace(subShape, towardOther, body.GetCenterOfMassPosition(), face);
    return contactSurfaceNormal(face, towardOther, body.GetWorldSpaceSurfaceNormal(subShape, point));
}

// Collects "contact began" events. Called from Jolt worker threads.
class ContactListener final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        auto vec = [](const auto& v) {
            return glm::vec3(static_cast<f32>(v.GetX()), static_cast<f32>(v.GetY()), static_cast<f32>(v.GetZ()));
        };
        ContactEvent e;
        e.bodyA = {a.GetID().GetIndexAndSequenceNumber()};
        e.bodyB = {b.GetID().GetIndexAndSequenceNumber()};
        e.userDataA = a.GetUserData();
        e.userDataB = b.GetUserData();
        e.normal = vec(manifold.mWorldSpaceNormal);
        // Both bodies are locked during this callback, so their shapes can be queried directly.
        JPH::RVec3 pa = manifold.GetWorldSpaceContactPointOn1(0);
        JPH::RVec3 pb = manifold.GetWorldSpaceContactPointOn2(0);
        e.pointA = vec(pa);
        e.pointB = vec(pb);
        // mWorldSpaceNormal points from A to B: A's touching face faces +normal, B's faces -normal.
        e.surfaceNormalA = vec(bodyContactSurfaceNormal(a, manifold.mSubShapeID1, pa, manifold.mWorldSpaceNormal));
        e.surfaceNormalB = vec(bodyContactSurfaceNormal(b, manifold.mSubShapeID2, pb, -manifold.mWorldSpaceNormal));
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

// Applies QueryFilter (ignored body, sensors). Runs with the body already locked by Jolt.
class QueryBodyFilter final : public JPH::BodyFilter {
public:
    explicit QueryBodyFilter(const QueryFilter& f) : m_filter(f) {}
    bool ShouldCollide(const JPH::BodyID& id) const override {
        return !m_filter.ignoreBody || id.GetIndexAndSequenceNumber() != m_filter.ignoreBody.value;
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return m_filter.includeSensors || !body.IsSensor();
    }

private:
    QueryFilter m_filter;
};

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

void JoltPhysicsWorld::shiftOrigin(const glm::vec3& newOrigin) {
    if (newOrigin == glm::vec3(0.0f)) return;
    JPH::BodyIDVector ids;
    m_impl->system.GetBodies(ids);
    const JPH::RVec3 shift = toJR(newOrigin);
    for (const JPH::BodyID& id : ids)
        // Same translation for every body: relative poses (and so cached contacts) are unchanged.
        m_impl->bodies().SetPosition(id, m_impl->bodies().GetPosition(id) - shift, JPH::EActivation::DontActivate);
    m_impl->broadPhaseDirty = true;
}

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
                               RaycastHit& hit, const QueryFilter& filter) const {
    if (glm::dot(direction, direction) < 1e-12f || maxDistance <= 0.0f) return false;
    glm::vec3 dir = glm::normalize(direction);
    JPH::RRayCast ray{toJR(origin), toJ(dir * maxDistance)};
    JPH::RayCastResult result;
    QueryBodyFilter bodyFilter(filter);
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, bodyFilter)) return false;

    JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    hit = {};
    hit.body = {result.mBodyID.GetIndexAndSequenceNumber()};
    hit.point = toG(point);
    hit.fraction = result.mFraction;
    hit.distance = result.mFraction * maxDistance;
    JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        hit.userData = body.GetUserData();
        // A ray has no surface of its own: the contact normal is the hit surface normal.
        hit.hitSurfaceNormal = toG(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
        hit.normal = hit.hitSurfaceNormal;
    }
    return true;
}

namespace {

// A sweep ready to run against the world: the moving shape, its path and settings.
struct PreparedSweep {
    JPH::RShapeCast cast;
    JPH::ShapeCastSettings settings;
    JPH::RVec3 baseOffset; // hit positions are reported relative to this (large-world precision)
    f32 maxDistance;
};

std::optional<PreparedSweep> prepareSweep(const JPH::Shape* shape, const JPH::RMat44& transform,
                                          const glm::vec3& direction, f32 maxDistance) {
    if (!shape || glm::dot(direction, direction) < 1e-12f || maxDistance <= 0.0f) return std::nullopt;
    JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(shape, JPH::Vec3::sOne(), transform,
                                                                toJ(glm::normalize(direction) * maxDistance));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mReturnDeepestPoint = true; // meaningful point/normal when starting in contact
    JPH::RVec3 base = cast.mCenterOfMassStart.GetTranslation();
    return PreparedSweep{cast, settings, base, maxDistance};
}

template <typename Collector>
void sweepWorld(const JPH::PhysicsSystem& system, const PreparedSweep& s, const QueryFilter& filter,
                Collector& collector) {
    QueryBodyFilter bodyFilter(filter);
    system.GetNarrowPhaseQuery().CastShape(s.cast, s.settings, s.baseOffset, collector, {}, {}, bodyFilter);
}

QueryHit toQueryHit(const JPH::PhysicsSystem& system, const JPH::ShapeCastResult& r, const PreparedSweep& s) {
    QueryHit hit;
    hit.body = {r.mBodyID2.GetIndexAndSequenceNumber()};
    hit.fraction = r.mFraction;
    hit.distance = r.mFraction * s.maxDistance;
    const JPH::RVec3 pointOn2 = s.baseOffset + r.mContactPointOn2;
    const JPH::RVec3 pointOn1 = s.baseOffset + r.mContactPointOn1;
    hit.point = toG(pointOn2);
    hit.sweptPoint = toG(pointOn1);

    // Contact normal: the penetration axis points from the swept shape into the hit body.
    JPH::Vec3 axis = r.mPenetrationAxis;
    const JPH::Vec3 towardQuery = axis.LengthSq() > 1e-12f ? -axis.Normalized() : JPH::Vec3::sZero();
    hit.normal = toG(towardQuery);

    // Hit body's own surface normal (its face turned towards the swept shape).
    {
        JPH::BodyLockRead lock(system.GetBodyLockInterface(), r.mBodyID2);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            hit.userData = body.GetUserData();
            hit.hitSurfaceNormal = toG(bodyContactSurfaceNormal(body, r.mSubShapeID2, pointOn2, towardQuery));
        }
    }

    // Swept shape's surface normal at its pose at the moment of impact (its face turned towards the hit body).
    const JPH::RMat44 comAtImpact = s.cast.mCenterOfMassStart.PostTranslated(s.cast.mDirection * r.mFraction);
    const JPH::Vec3 local = JPH::Vec3(comAtImpact.InversedRotationTranslation() * pointOn1);
    const JPH::Vec3 pointNormal = comAtImpact.Multiply3x3(s.cast.mShape->GetSurfaceNormal(r.mSubShapeID1, local));
    // Vertices relative to the impact COM (only their differences matter for the normal).
    const JPH::Mat44 comLocal = comAtImpact.PostTranslated(-comAtImpact.GetTranslation()).ToMat44();
    JPH::Shape::SupportingFace face;
    s.cast.mShape->GetSupportingFace(r.mSubShapeID1, comLocal.Multiply3x3Transposed(-towardQuery), JPH::Vec3::sOne(),
                                     comLocal, face);
    hit.sweptSurfaceNormal = toG(contactSurfaceNormal(face, -towardQuery, pointNormal));
    return hit;
}

// Sorted, one hit per body (compound/mesh bodies can report several sub-shape hits).
std::vector<SweepHit> collectAll(const JPH::PhysicsSystem& system,
                                 JPH::AllHitCollisionCollector<JPH::CastShapeCollector>& collector,
                                 const PreparedSweep& s) {
    collector.Sort();
    std::vector<SweepHit> hits;
    for (const JPH::ShapeCastResult& r : collector.mHits) {
        BodyId id{r.mBodyID2.GetIndexAndSequenceNumber()};
        if (std::any_of(hits.begin(), hits.end(), [&](const SweepHit& h) { return h.body == id; })) continue;
        hits.push_back(toQueryHit(system, r, s));
    }
    return hits;
}

JPH::RMat44 poseMatrix(const glm::vec3& position, const glm::quat& rotation) {
    return JPH::RMat44::sRotationTranslation(toJ(rotation), toJR(position));
}

} // namespace

bool JoltPhysicsWorld::sweep(const ShapeDesc& shapeDesc, const glm::vec3& position, const glm::quat& rotation,
                             const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                             const QueryFilter& filter) const {
    JPH::ShapeRefC shape = buildShape(shapeDesc);
    auto s = prepareSweep(shape, poseMatrix(position, rotation), direction, maxDistance);
    if (!s) return false;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    sweepWorld(m_impl->system, *s, filter, collector);
    if (!collector.HadHit()) return false;
    hit = toQueryHit(m_impl->system, collector.mHit, *s);
    return true;
}

std::vector<SweepHit> JoltPhysicsWorld::sweepAll(const ShapeDesc& shapeDesc, const glm::vec3& position,
                                                 const glm::quat& rotation, const glm::vec3& direction,
                                                 f32 maxDistance, const QueryFilter& filter) const {
    JPH::ShapeRefC shape = buildShape(shapeDesc);
    auto s = prepareSweep(shape, poseMatrix(position, rotation), direction, maxDistance);
    if (!s) return {};
    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    sweepWorld(m_impl->system, *s, filter, collector);
    return collectAll(m_impl->system, collector, *s);
}

bool JoltPhysicsWorld::sweepBody(BodyId body, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                                 const QueryFilter& filter) const {
    if (!body) return false;
    // Copy shape + pose under the lock, then query unlocked (the query takes its own body locks).
    JPH::ShapeRefC shape;
    JPH::RMat44 transform;
    {
        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), toJ(body));
        if (!lock.Succeeded()) return false;
        shape = lock.GetBody().GetShape();
        transform = lock.GetBody().GetWorldTransform();
    }
    auto s = prepareSweep(shape, transform, direction, maxDistance);
    if (!s) return false;

    QueryFilter f = filter;
    f.ignoreBody = body;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    sweepWorld(m_impl->system, *s, f, collector);
    if (!collector.HadHit()) return false;
    hit = toQueryHit(m_impl->system, collector.mHit, *s);
    return true;
}

std::vector<ContactEvent> JoltPhysicsWorld::drainContactEvents() {
    std::lock_guard lock(m_impl->contacts.mutex);
    std::vector<ContactEvent> out;
    out.swap(m_impl->contacts.events);
    return out;
}

} // namespace nova::physics
