#include "nova/physics/backend/jolt/JoltPhysicsWorld.h"

#include "nova/core/Log.h"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <memory>
#include <unordered_map>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <map>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

namespace nova::physics::local {

namespace {

// ---------------------------------------------------------------- layers --
// Object layer = collision layer × moving: `layer * 2 + (moving ? 1 : 0)`. Static geometry never
// tests against static geometry; other pairs follow the CollisionLayers matrix.
JPH::ObjectLayer objectLayer(u32 layer, bool moving) {
    return static_cast<JPH::ObjectLayer>((layer & (kCollisionLayerCount - 1)) * 2 + (moving ? 1 : 0));
}
u32 collisionLayerOf(JPH::ObjectLayer l) { return static_cast<u32>(l) >> 1; }
bool isMovingLayer(JPH::ObjectLayer l) { return (l & 1) != 0; }

namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer NonMoving(0);
constexpr JPH::BroadPhaseLayer Moving(1);
constexpr JPH::uint Count = 2;
} // namespace BroadPhaseLayers

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    explicit ObjectLayerPairFilter(const CollisionLayers& layers) : m_layers(layers) {}
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (!isMovingLayer(a) && !isMovingLayer(b)) return false;
        return m_layers.collides(collisionLayerOf(a), collisionLayerOf(b));
    }

private:
    const CollisionLayers& m_layers; // owned by the world; changed only between steps
};

// Queries: only bodies whose collision layer is in the mask.
class QueryLayerFilter final : public JPH::ObjectLayerFilter {
public:
    explicit QueryLayerFilter(u32 mask) : m_mask(mask) {}
    bool ShouldCollide(JPH::ObjectLayer l) const override { return (m_mask & layerBit(collisionLayerOf(l))) != 0; }

private:
    u32 m_mask;
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return isMovingLayer(layer) ? BroadPhaseLayers::Moving : BroadPhaseLayers::NonMoving;
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
        return isMovingLayer(layer) || bp == BroadPhaseLayers::Moving;
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

// Collects contact events, one Began / Ended per pair of bodies. Jolt reports contacts per pair
// of sub-shapes (mesh triangles, compound parts): they are counted, so a body sliding across
// triangles of a mesh stays one continuous touch. Called from Jolt worker threads.
class ContactListener final : public JPH::ContactListener {
public:
    using Key = std::pair<u32, u32>; // body ids, smaller first
    static Key key(const JPH::BodyID& a, const JPH::BodyID& b) {
        const u32 x = a.GetIndexAndSequenceNumber(), y = b.GetIndexAndSequenceNumber();
        return x < y ? Key{x, y} : Key{y, x};
    }

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        auto vec = [](const auto& v) {
            return glm::vec3(static_cast<f32>(v.GetX()), static_cast<f32>(v.GetY()), static_cast<f32>(v.GetZ()));
        };
        {
            std::lock_guard lock(mutex);
            Pair& p = pairs[key(a.GetID(), b.GetID())];
            if (p.asleep > 0) { // a sub-shape pair coming back after sleeping: it never stopped touching
                --p.asleep;
                return;
            }
            if (p.count++ > 0) return; // another sub-shape of a pair already touching
            p.ended.type = ContactEventType::Ended;
            p.ended.sensor = a.IsSensor() || b.IsSensor();
            p.ended.bodyA = {a.GetID().GetIndexAndSequenceNumber()};
            p.ended.bodyB = {b.GetID().GetIndexAndSequenceNumber()};
            p.ended.userDataA = a.GetUserData();
            p.ended.userDataB = b.GetUserData();
        }
        ContactEvent e;
        e.sensor = a.IsSensor() || b.IsSensor();
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

    // Jolt also removes contacts when bodies fall asleep (and adds them back when they wake), and
    // bodies cannot be read here: removals are resolved after the step (resolveRemovals).
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        std::lock_guard lock(mutex);
        removed.push_back(key(pair.GetBody1ID(), pair.GetBody2ID()));
    }

    // After PhysicsSystem::Update, single-threaded. A removal where neither body is active (and
    // both still exist) is a pair falling asleep: still touching. A sleeping pair that woke up
    // without its contacts coming back has separated.
    void resolveRemovals(const JPH::BodyInterface& bodies) {
        auto awake = [&](u32 id) { return bodies.IsActive(JPH::BodyID(id)); };
        auto exists = [&](u32 id) { return bodies.IsAdded(JPH::BodyID(id)); };
        std::lock_guard lock(mutex);
        for (const Key& k : removed) {
            auto it = pairs.find(k);
            if (it == pairs.end()) continue; // already ended (destroyed body)
            Pair& p = it->second;
            if (exists(k.first) && exists(k.second) && !awake(k.first) && !awake(k.second)) {
                ++p.asleep;
            } else if (--p.count <= 0) {
                events.push_back(p.ended);
                pairs.erase(it);
            }
        }
        removed.clear();
        for (auto it = pairs.begin(); it != pairs.end();) {
            Pair& p = it->second;
            if (p.asleep > 0 && (awake(it->first.first) || awake(it->first.second))) {
                p.count -= p.asleep; // woke up, and these did not come back
                p.asleep = 0;
                if (p.count <= 0) {
                    events.push_back(p.ended);
                    it = pairs.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // A destroyed body ends every touch it had (whether or not Jolt reports it later).
    void bodyRemoved(const JPH::BodyID& id) {
        const u32 v = id.GetIndexAndSequenceNumber();
        std::lock_guard lock(mutex);
        for (auto it = pairs.begin(); it != pairs.end();) {
            if (it->first.first == v || it->first.second == v) {
                events.push_back(it->second.ended);
                it = pairs.erase(it);
            } else {
                ++it;
            }
        }
    }

    struct Pair {
        int count = 0;      // touching sub-shape pairs (asleep ones included)
        int asleep = 0;     // of those, removed by Jolt because the pair fell asleep
        ContactEvent ended; // the event to send when the last one goes
    };
    std::mutex mutex;
    std::vector<ContactEvent> events;
    std::map<Key, Pair> pairs;
    std::vector<Key> removed; // reported during the step, resolved after it
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

// Wakes whatever rests on or against a body (its bounds, grown a little): Jolt keeps sleeping
// bodies asleep when what supports them changes, moves or disappears.
void wakeAround(JPH::PhysicsSystem& system, const JPH::BodyID& id) {
    JPH::AABox bounds;
    {
        JPH::BodyLockRead lock(system.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return;
        // World-edge walls are kilometres wide and rebuilt right away on every rebase: waking
        // everything near them each time would be all cost and no effect.
        if (isWorldBoundary(lock.GetBody().GetUserData())) return;
        bounds = lock.GetBody().GetWorldSpaceBounds();
    }
    bounds.ExpandBy(JPH::Vec3::sReplicate(0.05f));
    system.GetBodyInterface().ActivateBodiesInAABox(bounds, {}, {});
}

JPH::EMotionType toJ(BodyType t) {
    switch (t) {
        case BodyType::Static: return JPH::EMotionType::Static;
        case BodyType::Kinematic: return JPH::EMotionType::Kinematic;
        case BodyType::Dynamic: return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Static;
}

// Hulls and meshes are expensive to build (hull faces, mesh BVH) and Jolt shapes are shared
// immutable objects: build once per geometry and reuse for every body and every world.
struct GeometryShape {
    std::weak_ptr<const ShapeGeometry> geometry; // expired = the address may have been reused
    JPH::ShapeRefC shape;
};
std::unordered_map<u64, GeometryShape>& geometryCache() {
    static std::unordered_map<u64, GeometryShape> cache;
    return cache;
}

JPH::ShapeSettings::ShapeResult buildGeometryShape(const ShapeDesc& s) {
    const ShapeGeometry& g = *s.geometry;
    if (s.type == ShapeType::ConvexHull) {
        JPH::Array<JPH::Vec3> points;
        points.reserve(g.points.size());
        for (const glm::vec3& p : g.points) points.push_back(toJ(p));
        return JPH::ConvexHullShapeSettings(points, JPH::cDefaultConvexRadius).Create();
    }
    JPH::VertexList vertices;
    vertices.reserve(g.points.size());
    for (const glm::vec3& p : g.points) vertices.push_back(JPH::Float3(p.x, p.y, p.z));
    JPH::IndexedTriangleList triangles;
    triangles.reserve(g.indices.size() / 3);
    for (size_t i = 0; i + 2 < g.indices.size(); i += 3)
        triangles.push_back(JPH::IndexedTriangle(g.indices[i], g.indices[i + 1], g.indices[i + 2], 0));
    return JPH::MeshShapeSettings(std::move(vertices), std::move(triangles)).Create();
}

JPH::ShapeRefC geometryShape(const ShapeDesc& s) {
    if (!s.geometry || s.geometry->points.empty()) {
        NOVA_CORE_ERROR("[physics] {} shape without points", s.type == ShapeType::Mesh ? "mesh" : "convex hull");
        return nullptr;
    }
    auto& cache = geometryCache();
    const u64 key = reinterpret_cast<u64>(s.geometry.get()) ^ (s.type == ShapeType::Mesh ? 1ull : 0ull);
    if (auto it = cache.find(key); it != cache.end() && !it->second.geometry.expired()) return it->second.shape;
    JPH::ShapeSettings::ShapeResult result = buildGeometryShape(s);
    if (result.HasError()) {
        NOVA_CORE_ERROR("[physics] shape creation failed: {}", result.GetError().c_str());
        return nullptr;
    }
    if (cache.size() > 4096) // drop entries whose geometry is gone
        std::erase_if(cache, [](const auto& e) { return e.second.geometry.expired(); });
    cache[key] = {s.geometry, result.Get()};
    return result.Get();
}

JPH::ShapeRefC buildShape(const ShapeDesc& s);

// The shape itself, without its offset / rotation.
JPH::ShapeRefC buildBaseShape(const ShapeDesc& s) {
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
        case ShapeType::Cylinder: {
            const f32 h = std::max(s.halfHeight, kMin), r = std::max(s.radius, kMin);
            result = JPH::CylinderShapeSettings(h, r, std::min(JPH::cDefaultConvexRadius, std::min(h, r) * 0.5f)).Create();
            break;
        }
        case ShapeType::ConvexHull:
        case ShapeType::Mesh: return geometryShape(s); // cached, shared
        case ShapeType::Compound: {
            if (!s.children || s.children->empty()) return nullptr;
            JPH::StaticCompoundShapeSettings compound;
            for (const ShapeDesc& part : *s.children) {
                JPH::ShapeRefC base = buildBaseShape(part);
                if (!base) return nullptr;
                compound.AddShape(toJ(part.offset), toJ(part.rotation), base);
            }
            result = compound.Create();
            break;
        }
    }
    if (result.HasError()) {
        NOVA_CORE_ERROR("[physics] shape creation failed: {}", result.GetError().c_str());
        return nullptr;
    }
    return result.Get();
}

// The shape placed in the body: its offset and rotation applied.
JPH::ShapeRefC buildShape(const ShapeDesc& s) {
    JPH::ShapeRefC shape = buildBaseShape(s);
    const bool rotated = s.rotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (!shape || (s.offset == glm::vec3(0.0f) && !rotated)) return shape;
    auto placed = JPH::RotatedTranslatedShapeSettings(toJ(s.offset), toJ(s.rotation), shape).Create();
    return placed.HasError() ? shape : placed.Get();
}

JPH::EAllowedDOFs allowedDOFs(u8 locked) {
    u8 allowed = static_cast<u8>(JPH::EAllowedDOFs::All) & ~locked;
    if (allowed == 0) allowed = static_cast<u8>(JPH::EAllowedDOFs::TranslationY); // Jolt needs one; keep falling
    return static_cast<JPH::EAllowedDOFs>(allowed);
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
struct JoltJoint {
    JPH::Ref<JPH::TwoBodyConstraint> constraint; // null = free slot
    JointType type = JointType::Point;
    JPH::BodyID bodyA, bodyB;                     // bodyB invalid = the world
    glm::vec3 localA{0.0f}, localB{0.0f};         // anchors relative to each body's pose (B: world when no body)
};

struct JoltPhysicsWorld::Impl {
    CollisionLayers layers;
    std::vector<JoltJoint> joints;
    std::vector<u32> freeJoints;
    f32 lastStep = 1.0f / 60.0f;
    BroadPhaseLayerInterface broadPhaseLayers;
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhase;
    ObjectLayerPairFilter objectPairs{layers};
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
    m_impl->layers = desc.layers;

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
    if (desc.shape.type == ShapeType::Mesh && desc.type == BodyType::Dynamic) {
        NOVA_CORE_ERROR("[physics] mesh shapes cannot be dynamic (use a convex hull)");
        return {};
    }

    const bool moving = desc.type != BodyType::Static;
    JPH::BodyCreationSettings bcs(shape, toJR(desc.position), toJ(desc.rotation), toJ(desc.type),
                                  objectLayer(desc.layer, moving));
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
        // No bumps on the internal edges of meshes (a ball rolling across triangles stays down).
        bcs.mEnhancedInternalEdgeRemoval = true;
        bcs.mAllowedDOFs = allowedDOFs(desc.lockedAxes);
    }
    if (desc.continuousCollision) bcs.mMotionQuality = JPH::EMotionQuality::LinearCast;
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
    for (u32 j = 0; j < m_impl->joints.size(); ++j) // never leave a joint pointing at a removed body
        if (const JoltJoint& joint = m_impl->joints[j]; joint.constraint && (joint.bodyA == id || joint.bodyB == id))
            destroyJoint({j});
    wakeAround(m_impl->system, id); // what rested on it falls
    m_impl->bodies().RemoveBody(id);
    m_impl->bodies().DestroyBody(id);
    m_impl->contacts.bodyRemoved(id);
}

u32 JoltPhysicsWorld::bodyCount() const { return m_impl->system.GetNumBodies(); }

void JoltPhysicsWorld::setTransform(BodyId body, const glm::vec3& position, const glm::quat& rotation) {
    wakeAround(m_impl->system, toJ(body)); // where it was...
    m_impl->bodies().SetPositionAndRotation(toJ(body), toJR(position), toJ(rotation), JPH::EActivation::Activate);
    wakeAround(m_impl->system, toJ(body)); // ...and where it is now
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

void JoltPhysicsWorld::setBodyProperties(BodyId body, const BodyDesc& d) {
    const JPH::BodyID id = toJ(body);
    JPH::BodyInterface& bi = m_impl->bodies();
    if (!bi.IsAdded(id)) return;
    bi.SetFriction(id, d.friction);
    bi.SetRestitution(id, d.restitution);
    bi.SetGravityFactor(id, d.gravityFactor);
    const bool moving = bi.GetMotionType(id) != JPH::EMotionType::Static;
    bi.SetObjectLayer(id, objectLayer(d.layer, moving));
    if (moving) bi.SetMotionQuality(id, d.continuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete);
    {
        JPH::BodyLockWrite lock(m_impl->system.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) return;
        JPH::Body& b = lock.GetBody();
        b.SetIsSensor(d.isSensor);
        if (JPH::MotionProperties* mp = b.GetMotionPropertiesUnchecked(); mp && !b.IsStatic()) {
            mp->SetLinearDamping(d.linearDamping);
            mp->SetAngularDamping(d.angularDamping);
        }
    }
    if (moving) bi.ActivateBody(id); // whatever changed shows on the next step
    wakeAround(m_impl->system, id);  // and on what touches it (a floor turned into a sensor...)
}

glm::vec3 JoltPhysicsWorld::linearVelocity(BodyId body) const {
    return toG(m_impl->bodies().GetLinearVelocity(toJ(body)));
}
glm::vec3 JoltPhysicsWorld::angularVelocity(BodyId body) const {
    return toG(m_impl->bodies().GetAngularVelocity(toJ(body)));
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
void JoltPhysicsWorld::setCollisionLayers(const CollisionLayers& layers) { m_impl->layers = layers; }
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
    m_impl->lastStep = dt;
    JPH::EPhysicsUpdateError err =
        m_impl->system.Update(dt, 1, m_impl->tempAllocator.get(), m_impl->jobSystem.get());
    if (err != JPH::EPhysicsUpdateError::None)
        NOVA_CORE_WARN("[physics] update error flags 0x{:x} (increase world limits)", static_cast<u32>(err));
    m_impl->contacts.resolveRemovals(m_impl->bodies());
}

bool JoltPhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, f32 maxDistance,
                               RaycastHit& hit, const QueryFilter& filter) const {
    if (glm::dot(direction, direction) < 1e-12f || maxDistance <= 0.0f) return false;
    glm::vec3 dir = glm::normalize(direction);
    JPH::RRayCast ray{toJR(origin), toJ(dir * maxDistance)};
    JPH::RayCastResult result;
    QueryBodyFilter bodyFilter(filter);
    QueryLayerFilter layerFilter(filter.layerMask);
    if (!m_impl->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, layerFilter, bodyFilter)) return false;

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
    QueryLayerFilter layerFilter(filter.layerMask);
    system.GetNarrowPhaseQuery().CastShape(s.cast, s.settings, s.baseOffset, collector, {}, layerFilter, bodyFilter);
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

// ------------------------------------------------------------------ joints --
namespace {
glm::vec3 anyPerpendicular(const glm::vec3& a) {
    const glm::vec3 t = std::abs(a.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    return glm::normalize(glm::cross(a, t));
}

void applyMotor(JPH::TwoBodyConstraint& c, JointType type, const JointMotor& m) {
    const f32 limit = m.maxForce > 0.0f ? m.maxForce : FLT_MAX;
    const JPH::EMotorState state = m.mode == JointMotorMode::Velocity   ? JPH::EMotorState::Velocity
                                   : m.mode == JointMotorMode::Position ? JPH::EMotorState::Position
                                                                        : JPH::EMotorState::Off;
    if (type == JointType::Hinge) {
        auto& h = static_cast<JPH::HingeConstraint&>(c);
        h.GetMotorSettings().SetTorqueLimit(limit);
        h.SetMotorState(state);
        if (state == JPH::EMotorState::Velocity) h.SetTargetAngularVelocity(m.target);
        if (state == JPH::EMotorState::Position) h.SetTargetAngle(m.target);
    } else if (type == JointType::Slider) {
        auto& s = static_cast<JPH::SliderConstraint&>(c);
        s.GetMotorSettings().SetForceLimit(limit);
        s.SetMotorState(state);
        if (state == JPH::EMotorState::Velocity) s.SetTargetVelocity(m.target);
        if (state == JPH::EMotorState::Position) s.SetTargetPosition(m.target);
    }
}
} // namespace

JointId JoltPhysicsWorld::createJoint(const JointDesc& d) {
    JPH::BodyLockInterfaceNoLock& locks = const_cast<JPH::BodyLockInterfaceNoLock&>(m_impl->system.GetBodyLockInterfaceNoLock());
    JPH::Body* a = d.bodyA ? locks.TryGetBody(toJ(d.bodyA)) : nullptr;
    JPH::Body* b = d.bodyB ? locks.TryGetBody(toJ(d.bodyB)) : &JPH::Body::sFixedToWorld;
    if (!a || !b || a == b) {
        NOVA_CORE_ERROR("[physics] joint between invalid bodies");
        return {};
    }
    const JPH::RVec3 anchor = toJR(d.anchor);
    const glm::vec3 axisG = glm::length(d.axis) > 1e-6f ? glm::normalize(d.axis) : glm::vec3(0, 1, 0);
    const JPH::Vec3 axis = toJ(axisG), normal = toJ(anyPerpendicular(axisG));
    JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
    switch (d.type) {
        case JointType::Fixed: {
            auto s = new JPH::FixedConstraintSettings();
            s->mSpace = JPH::EConstraintSpace::WorldSpace;
            s->mAutoDetectPoint = false;
            s->mPoint1 = s->mPoint2 = anchor; // same axes for both: keeps today's relative rotation
            settings = s;
            break;
        }
        case JointType::Point: {
            auto s = new JPH::PointConstraintSettings();
            s->mSpace = JPH::EConstraintSpace::WorldSpace;
            s->mPoint1 = s->mPoint2 = anchor;
            settings = s;
            break;
        }
        case JointType::Hinge: {
            auto s = new JPH::HingeConstraintSettings();
            s->mSpace = JPH::EConstraintSpace::WorldSpace;
            s->mPoint1 = s->mPoint2 = anchor;
            s->mHingeAxis1 = s->mHingeAxis2 = axis;
            s->mNormalAxis1 = s->mNormalAxis2 = normal;
            if (d.limitsEnabled) { // Jolt: min in [-pi, 0], max in [0, pi]
                s->mLimitsMin = glm::clamp(std::min(d.limitMin, d.limitMax), -JPH::JPH_PI, 0.0f);
                s->mLimitsMax = glm::clamp(std::max(d.limitMin, d.limitMax), 0.0f, JPH::JPH_PI);
            }
            settings = s;
            break;
        }
        case JointType::Slider: {
            auto s = new JPH::SliderConstraintSettings();
            s->mSpace = JPH::EConstraintSpace::WorldSpace;
            s->mPoint1 = s->mPoint2 = anchor;
            s->mSliderAxis1 = s->mSliderAxis2 = axis;
            s->mNormalAxis1 = s->mNormalAxis2 = normal;
            if (d.limitsEnabled) {
                s->mLimitsMin = std::min(d.limitMin, d.limitMax);
                s->mLimitsMax = std::max(d.limitMin, d.limitMax);
            }
            settings = s;
            break;
        }
        case JointType::Distance: {
            auto s = new JPH::DistanceConstraintSettings();
            s->mSpace = JPH::EConstraintSpace::WorldSpace;
            s->mPoint1 = toJR(d.anchorB); // body 1 is B (see below)
            s->mPoint2 = anchor;
            s->mMinDistance = d.minDistance;
            s->mMaxDistance = d.maxDistance;
            if (d.springFrequency > 0.0f)
                s->mLimitsSpringSettings = JPH::SpringSettings(JPH::ESpringMode::FrequencyAndDamping, d.springFrequency,
                                                               d.springDamping);
            settings = s;
            break;
        }
    }
    // Body 1 = B (or the world), body 2 = A: Jolt measures angles, positions and motors of body 2
    // relative to body 1, so they read naturally from A's side (a positive motor turns A positively).
    JPH::Ref<JPH::TwoBodyConstraint> c = settings->Create(*b, *a);
    applyMotor(*c, d.type, d.motor);
    m_impl->system.AddConstraint(c);

    u32 index;
    if (!m_impl->freeJoints.empty()) {
        index = m_impl->freeJoints.back();
        m_impl->freeJoints.pop_back();
    } else {
        index = static_cast<u32>(m_impl->joints.size());
        m_impl->joints.emplace_back();
    }
    JoltJoint& j = m_impl->joints[index];
    j.constraint = c;
    j.type = d.type;
    j.bodyA = a->GetID();
    j.bodyB = d.bodyB ? b->GetID() : JPH::BodyID();
    const glm::quat qa = toG(a->GetRotation());
    j.localA = glm::conjugate(qa) * (d.anchor - toG(a->GetPosition()));
    const glm::vec3 attachB = d.type == JointType::Distance ? d.anchorB : d.anchor;
    j.localB = d.bodyB ? glm::conjugate(toG(b->GetRotation())) * (attachB - toG(b->GetPosition())) : attachB;
    for (const JPH::BodyID& id : {j.bodyA, j.bodyB})
        if (!id.IsInvalid()) m_impl->bodies().ActivateBody(id);
    return {index};
}

void JoltPhysicsWorld::destroyJoint(JointId joint) {
    if (!joint || joint.value >= m_impl->joints.size() || !m_impl->joints[joint.value].constraint) return;
    JoltJoint& j = m_impl->joints[joint.value];
    m_impl->system.RemoveConstraint(j.constraint);
    for (const JPH::BodyID& id : {j.bodyA, j.bodyB}) // they move freely now
        if (!id.IsInvalid() && m_impl->bodies().IsAdded(id)) m_impl->bodies().ActivateBody(id);
    j = JoltJoint{};
    m_impl->freeJoints.push_back(joint.value);
}

void JoltPhysicsWorld::setJointMotor(JointId joint, const JointMotor& motor) {
    if (!joint || joint.value >= m_impl->joints.size() || !m_impl->joints[joint.value].constraint) return;
    JoltJoint& j = m_impl->joints[joint.value];
    applyMotor(*j.constraint, j.type, motor);
    for (const JPH::BodyID& id : {j.bodyA, j.bodyB})
        if (!id.IsInvalid()) m_impl->bodies().ActivateBody(id);
}

f32 JoltPhysicsWorld::jointPosition(JointId joint) const {
    if (!joint || joint.value >= m_impl->joints.size() || !m_impl->joints[joint.value].constraint) return 0.0f;
    const JoltJoint& j = m_impl->joints[joint.value];
    switch (j.type) {
        case JointType::Hinge: return static_cast<const JPH::HingeConstraint&>(*j.constraint).GetCurrentAngle();
        case JointType::Slider: return static_cast<const JPH::SliderConstraint&>(*j.constraint).GetCurrentPosition();
        case JointType::Distance: {
            glm::vec3 pa, pb;
            glm::quat qa, qb;
            getTransform({j.bodyA.GetIndexAndSequenceNumber()}, pa, qa);
            const glm::vec3 a = pa + qa * j.localA;
            if (j.bodyB.IsInvalid()) return glm::length(j.localB - a);
            getTransform({j.bodyB.GetIndexAndSequenceNumber()}, pb, qb);
            return glm::length(pb + qb * j.localB - a);
        }
        default: return 0.0f;
    }
}

JointLoad JoltPhysicsWorld::jointLoad(JointId joint) const {
    if (!joint || joint.value >= m_impl->joints.size() || !m_impl->joints[joint.value].constraint) return {};
    const JoltJoint& j = m_impl->joints[joint.value];
    const f32 dt = std::max(m_impl->lastStep, 1e-6f);
    auto len2 = [](const JPH::Vector<2>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1]); };
    JointLoad l;
    switch (j.type) { // lambdas are impulses over the last step
        case JointType::Fixed: {
            const auto& c = static_cast<const JPH::FixedConstraint&>(*j.constraint);
            l.force = c.GetTotalLambdaPosition().Length() / dt;
            l.torque = c.GetTotalLambdaRotation().Length() / dt;
            break;
        }
        case JointType::Point:
            l.force = static_cast<const JPH::PointConstraint&>(*j.constraint).GetTotalLambdaPosition().Length() / dt;
            break;
        case JointType::Hinge: {
            const auto& c = static_cast<const JPH::HingeConstraint&>(*j.constraint);
            l.force = c.GetTotalLambdaPosition().Length() / dt;
            l.torque = (len2(c.GetTotalLambdaRotation()) + std::abs(c.GetTotalLambdaRotationLimits())) / dt;
            break;
        }
        case JointType::Slider: {
            const auto& c = static_cast<const JPH::SliderConstraint&>(*j.constraint);
            l.force = (len2(c.GetTotalLambdaPosition()) + std::abs(c.GetTotalLambdaPositionLimits())) / dt;
            l.torque = c.GetTotalLambdaRotation().Length() / dt;
            break;
        }
        case JointType::Distance:
            l.force = std::abs(static_cast<const JPH::DistanceConstraint&>(*j.constraint).GetTotalLambdaPosition()) / dt;
            break;
    }
    return l;
}

std::vector<OverlapHit> JoltPhysicsWorld::overlap(const ShapeDesc& shapeDesc, const glm::vec3& position,
                                                  const glm::quat& rotation, const QueryFilter& filter) const {
    std::vector<OverlapHit> out;
    JPH::ShapeRefC shape = buildShape(shapeDesc);
    if (!shape) return out;
    // CollideShape takes the shape's center-of-mass transform (offset shapes move it).
    const JPH::RMat44 com = poseMatrix(position, rotation).PreTranslated(shape->GetCenterOfMass());
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces; // inside a mesh's triangles too
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    QueryBodyFilter bodyFilter(filter);
    QueryLayerFilter layerFilter(filter.layerMask);
    m_impl->system.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sOne(), com, settings, com.GetTranslation(),
                                                      collector, {}, layerFilter, bodyFilter);
    for (const JPH::CollideShapeResult& r : collector.mHits) {
        const BodyId id{r.mBodyID2.GetIndexAndSequenceNumber()};
        if (std::any_of(out.begin(), out.end(), [&](const OverlapHit& h) { return h.body == id; })) continue;
        OverlapHit h;
        h.body = id;
        JPH::BodyLockRead lock(m_impl->system.GetBodyLockInterface(), r.mBodyID2);
        if (lock.Succeeded()) h.userData = lock.GetBody().GetUserData();
        out.push_back(h);
    }
    // Deterministic order whatever the broad phase visited first.
    std::sort(out.begin(), out.end(), [](const OverlapHit& a, const OverlapHit& b) { return a.body.value < b.body.value; });
    return out;
}

std::vector<ContactEvent> JoltPhysicsWorld::drainContactEvents() {
    std::lock_guard lock(m_impl->contacts.mutex);
    std::vector<ContactEvent> out;
    out.swap(m_impl->contacts.events);
    return out;
}

} // namespace nova::physics::local
