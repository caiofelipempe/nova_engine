#include "nova/physics/skeleton/SkeletonPhysicsWorld.h"

#include "nova/core/Log.h"
#include "nova/math/WorldSpace.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Layout of a step (each phase is where a full engine replaces the skeleton's version):
//   1. velocities  += gravity, forces, torques; damping          (fixed point, exact)
//   2. contacts     = broad phase (int64) + narrow phase (pair-local floats)
//   3. solve        sequential impulses on the contacts           TODO(engine): fixed point
//   4. positions   += velocity · dt                               (exact 128-bit product)
//   5. events       pairs that started touching

namespace nova::physics {

namespace {

constexpr f32 kSpeculativeMargin = 0.01f; // contacts this far apart (plus the step's motion) are solved
constexpr f32 kTouchSlop = 0.005f;        // a pair this close after the step counts as touching
constexpr f32 kPenetrationSlop = 0.002f;  // penetration left uncorrected (avoids jitter)
constexpr f32 kBaumgarte = 0.2f;          // fraction of the penetration corrected per step
constexpr f32 kBounceThreshold = 1.0f;    // restitution only above this approach speed (m/s)
constexpr int kSolverIterations = 8;
constexpr f32 kQueryEpsilon = 1e-4f;      // sweeps: touching
constexpr int kMaxCastIterations = 64;    // sweeps: conservative advancement steps

const glm::quat kIdentity{1.0f, 0.0f, 0.0f, 0.0f};

f64 worldUnit() { return WorldSpace::active().unit(); }

// m/s (or m/s²) -> world steps · 2^-kVelocityBits per second (saturating).
i64 toRaw(f64 metersPerSecond) {
    const f64 x = metersPerSecond / worldUnit() * static_cast<f64>(1ll << SkeletonPhysicsWorld::kVelocityBits);
    if (!(x == x)) return 0;
    if (x >= 9.2e18) return detail::kI64Max;
    if (x <= -9.2e18) return detail::kI64Min;
    return std::llround(x);
}
glm::i64vec3 toRaw(const glm::vec3& v) { return {toRaw(v.x), toRaw(v.y), toRaw(v.z)}; }
glm::vec3 fromRaw(const glm::i64vec3& v) {
    const f64 k = worldUnit() / static_cast<f64>(1ll << SkeletonPhysicsWorld::kVelocityBits);
    return glm::vec3(glm::dvec3(v) * k);
}

// The step as a Q32 fraction of a second, from its exact flicks (rounded down).
i64 secondsQ32(Duration dt) {
    const i64 f = dt.flicks();
    if (f <= 0) return 0;
    const i64 whole = f / kFlicksPerSecond, rem = f % kFlicksPerSecond; // rem < 2^30
    return detail::saturatingAdd(detail::saturatingMul(whole, 1ll << 32), (rem << 32) / kFlicksPerSecond);
}

// Q32 factor (1 - c·dt, clamped at 0), as linear damping in Jolt.
i64 dampingQ32(f32 coefficient, f32 dt) {
    const f64 factor = std::max(0.0, 1.0 - static_cast<f64>(coefficient) * dt);
    return std::llround(factor * 4294967296.0);
}

// Radius of a sphere around the body origin that contains the shape (m).
f64 shapeReach(const ShapeDesc& s) {
    const f64 offset = glm::length(s.offset);
    switch (s.type) {
        case ShapeType::Box: return glm::length(s.halfExtents) + offset;
        case ShapeType::Sphere: return s.radius + offset;
        case ShapeType::Capsule: return s.radius + s.halfHeight + offset;
        case ShapeType::Cylinder: return std::sqrt(s.radius * s.radius + s.halfHeight * s.halfHeight) + offset;
        case ShapeType::ConvexHull:
        case ShapeType::Mesh: {
            f64 r = 0.0;
            if (s.geometry)
                for (const glm::vec3& p : s.geometry->points) r = std::max(r, static_cast<f64>(glm::length(p)));
            return r + offset;
        }
    }
    return offset;
}

// Inverse principal inertia (local axes). TODO(engine): exact tensors for hulls; shape offsets.
glm::vec3 inverseInertia(const ShapeDesc& s, f32 mass) {
    glm::vec3 i(0.0f);
    switch (s.type) {
        case ShapeType::Box: {
            const glm::vec3 h2 = s.halfExtents * s.halfExtents;
            i = mass / 3.0f * glm::vec3(h2.y + h2.z, h2.x + h2.z, h2.x + h2.y);
            break;
        }
        case ShapeType::Sphere: i = glm::vec3(0.4f * mass * s.radius * s.radius); break;
        case ShapeType::Capsule:
        case ShapeType::Cylinder: {
            const f32 r2 = s.radius * s.radius;
            const f32 h = 2.0f * s.halfHeight + (s.type == ShapeType::Capsule ? s.radius : 0.0f);
            const f32 side = mass / 12.0f * (3.0f * r2 + h * h);
            i = {side, 0.5f * mass * r2, side};
            break;
        }
        default: {
            const f32 r = static_cast<f32>(shapeReach(s));
            i = glm::vec3(0.4f * mass * r * r);
            break;
        }
    }
    return {i.x > 0 ? 1.0f / i.x : 0.0f, i.y > 0 ? 1.0f / i.y : 0.0f, i.z > 0 ? 1.0f / i.z : 0.0f};
}

// Extent of a shape along world axis `axis`, from its center.
f32 extentAlong(const ShapeDesc& s, const glm::quat& rotation, int axis) {
    switch (s.type) {
        case ShapeType::Sphere: return s.radius;
        case ShapeType::Box: {
            const glm::mat3 r = glm::mat3_cast(rotation);
            f32 e = 0.0f;
            for (int j = 0; j < 3; ++j) e += std::abs(r[j][axis]) * s.halfExtents[j];
            return e;
        }
        default: return static_cast<f32>(shapeReach(s) - glm::length(s.offset));
    }
}

// ------------------------------------------------------------- narrow phase --
// Everything below runs in a small local frame (meters, floats) chosen by the caller.

struct Manifold {
    f32 separation = 0.0f;          // < 0: penetrating
    glm::vec3 normal{0, 1, 0};      // from A towards B
    glm::vec3 pointA{0.0f}, pointB{0.0f};
    glm::vec3 surfaceA{0.0f}, surfaceB{0.0f};
};

Manifold sphereSphere(const glm::vec3& ca, f32 ra, const glm::vec3& cb, f32 rb) {
    Manifold m;
    const glm::vec3 d = cb - ca;
    const f32 len = glm::length(d);
    m.normal = len > 1e-6f ? d / len : glm::vec3(0, 1, 0);
    m.separation = len - ra - rb;
    m.pointA = ca + m.normal * ra;
    m.pointB = cb - m.normal * rb;
    m.surfaceA = m.normal;
    m.surfaceB = -m.normal;
    return m;
}

// A = box (center, rotation, half extents), B = sphere.
Manifold boxSphere(const glm::vec3& cb, const glm::quat& q, const glm::vec3& h, const glm::vec3& cs, f32 r) {
    Manifold m;
    const glm::vec3 p = glm::conjugate(q) * (cs - cb);
    const glm::vec3 c = glm::clamp(p, -h, h);
    glm::vec3 nLocal, pointLocal, faceLocal(0.0f);
    if (c != p) { // outside: closest point on the surface
        const glm::vec3 diff = p - c;
        const f32 dist = glm::length(diff);
        nLocal = diff / dist;
        m.separation = dist - r;
        pointLocal = c;
        int axis = 0; // the face the sphere is over (edges and corners: the most aligned one)
        for (int i = 1; i < 3; ++i)
            if (std::abs(diff[i]) > std::abs(diff[axis])) axis = i;
        faceLocal[axis] = diff[axis] > 0 ? 1.0f : -1.0f;
    } else { // center inside: push out through the nearest face
        int axis = 0;
        for (int i = 1; i < 3; ++i)
            if (h[i] - std::abs(p[i]) < h[axis] - std::abs(p[axis])) axis = i;
        const f32 sign = p[axis] >= 0 ? 1.0f : -1.0f;
        nLocal = glm::vec3(0.0f);
        nLocal[axis] = sign;
        faceLocal = nLocal;
        m.separation = -(h[axis] - std::abs(p[axis])) - r;
        pointLocal = p;
        pointLocal[axis] = sign * h[axis];
    }
    m.normal = q * nLocal;
    m.pointA = cb + q * pointLocal;
    m.pointB = cs - m.normal * r;
    m.surfaceA = q * faceLocal;
    m.surfaceB = -m.normal;
    return m;
}

Manifold flipped(const Manifold& m) {
    Manifold f;
    f.separation = m.separation;
    f.normal = -m.normal;
    f.pointA = m.pointB;
    f.pointB = m.pointA;
    f.surfaceA = m.surfaceB;
    f.surfaceB = m.surfaceA;
    return f;
}

// Shape A at (posA, qA) and B at (posB, qB) in the same local frame. False if the pair is unsupported.
// TODO(engine): GJK/EPA (or SAT) for every other pair; this is where it plugs in.
bool collide(const ShapeDesc& a, const glm::vec3& posA, const glm::quat& qA, const ShapeDesc& b,
             const glm::vec3& posB, const glm::quat& qB, Manifold& m) {
    const glm::vec3 ca = posA + qA * a.offset, cb = posB + qB * b.offset;
    if (a.type == ShapeType::Sphere && b.type == ShapeType::Sphere) {
        m = sphereSphere(ca, a.radius, cb, b.radius);
        return true;
    }
    if (a.type == ShapeType::Box && b.type == ShapeType::Sphere) {
        m = boxSphere(ca, qA, a.halfExtents, cb, b.radius);
        return true;
    }
    if (a.type == ShapeType::Sphere && b.type == ShapeType::Box) {
        m = flipped(boxSphere(cb, qB, b.halfExtents, ca, a.radius));
        return true;
    }
    return false;
}

// Ray from the frame origin along unit `d` against a sphere / box. `t` = 0 when starting inside.
bool raySphere(const glm::vec3& d, const glm::vec3& center, f32 r, f32& t, glm::vec3& normal) {
    const glm::vec3 oc = -center;
    const f32 c = glm::dot(oc, oc) - r * r;
    if (c <= 0.0f) {
        t = 0.0f;
        normal = -d;
        return true;
    }
    const f32 b = glm::dot(oc, d);
    const f32 disc = b * b - c;
    if (b > 0.0f || disc < 0.0f) return false;
    t = -b - std::sqrt(disc);
    if (t < 0.0f) return false;
    normal = glm::normalize(d * t - center);
    return true;
}

bool rayBox(const glm::vec3& d, const glm::vec3& center, const glm::quat& q, const glm::vec3& h, f32& t,
            glm::vec3& normal) {
    const glm::quat inv = glm::conjugate(q);
    const glm::vec3 o = inv * -center, dl = inv * d;
    f32 tmin = -std::numeric_limits<f32>::infinity(), tmax = std::numeric_limits<f32>::infinity();
    glm::vec3 n(0.0f);
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dl[i]) < 1e-12f) {
            if (std::abs(o[i]) > h[i]) return false;
            continue;
        }
        f32 t1 = (-h[i] - o[i]) / dl[i], t2 = (h[i] - o[i]) / dl[i];
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > tmin) {
            tmin = t1;
            n = glm::vec3(0.0f);
            n[i] = dl[i] > 0 ? -1.0f : 1.0f;
        }
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return false;
    }
    if (tmax < 0.0f) return false;
    if (tmin < 0.0f) {
        t = 0.0f;
        normal = -d;
        return true;
    }
    t = tmin;
    normal = q * n;
    return true;
}

QueryHit toQueryHit(const GlobalPosition& frame, const glm::vec3& point, const glm::vec3& normal,
                    const glm::vec3& surface, const glm::vec3& sweptPoint, const glm::vec3& sweptSurface, f32 t,
                    f32 maxDistance) {
    QueryHit h;
    h.point = frame + point;
    h.normal = normal;
    h.hitSurfaceNormal = surface;
    h.sweptPoint = frame + sweptPoint;
    h.sweptSurfaceNormal = sweptSurface;
    h.distance = t;
    h.fraction = maxDistance > 0 ? t / maxDistance : 0.0f;
    return h;
}

// Distance (m) from a position to the low/high edge of barrier axis `i`.
f64 edgeDistance(const GlobalPosition& p, int i, bool high) {
    const WorldSpace::Axis& a = WorldSpace::active().axis(i);
    const u64 steps = high ? static_cast<u64>(a.max) - static_cast<u64>(p.raw(i))
                           : static_cast<u64>(p.raw(i)) - static_cast<u64>(a.min);
    return static_cast<f64>(steps) * worldUnit();
}

u64 wallCode(int axis, bool high) { return kWorldBoundaryUserData | static_cast<u64>(axis * 2 + (high ? 1 : 0)); }

} // namespace

// -------------------------------------------------------------------- world --
struct SkeletonPhysicsWorld::Contact {
    i64 a = -1;                  // body index; -1 = barrier edge
    i64 b = -1;
    u64 wall = 0;                // barrier edge userData when a == -1
    GlobalPosition frame;        // the local frame's origin (a body's position)
    Manifold m;                  // in that frame
    f32 restitution = 0.0f;
    bool sensor = false;
    f32 target = 0.0f;           // wanted normal velocity (m/s)
    f32 impulse = 0.0f;          // accumulated, >= 0
};

SkeletonPhysicsWorld::SkeletonPhysicsWorld(const WorldDesc& desc) : m_desc(desc), m_gravity(desc.gravity) {}
SkeletonPhysicsWorld::~SkeletonPhysicsWorld() = default;

PhysicsCaps SkeletonPhysicsWorld::caps() const {
    PhysicsCaps c;
    c.name = "Skeleton";
    c.globalPositions = true;
    c.crossPlatformDeterministic = false; // TODO(engine): fixed-point rotations and solver
    c.dormancy = true;
    const u32 sphere = shapeBit(ShapeType::Sphere), box = shapeBit(ShapeType::Box);
    c.collides[static_cast<u32>(ShapeType::Sphere)] = sphere | box;
    c.collides[static_cast<u32>(ShapeType::Box)] = sphere;
    c.raycastShapes = sphere | box;
    c.sweepShapes = sphere;
    c.sweepTargets = sphere | box;
    return c;
}

SkeletonPhysicsWorld::Body* SkeletonPhysicsWorld::body(BodyId id) {
    return id.valid() && id.value < m_bodies.size() && m_bodies[id.value].alive ? &m_bodies[id.value] : nullptr;
}
const SkeletonPhysicsWorld::Body* SkeletonPhysicsWorld::body(BodyId id) const {
    return id.valid() && id.value < m_bodies.size() && m_bodies[id.value].alive ? &m_bodies[id.value] : nullptr;
}

BodyId SkeletonPhysicsWorld::createBody(const BodyDesc& desc) {
    if (!isValidBody(desc)) return {};
    if (m_alive >= m_desc.maxBodies) {
        NOVA_CORE_ERROR("[physics] body limit reached");
        return {};
    }
    u32 index;
    if (!m_free.empty()) {
        index = m_free.back();
        m_free.pop_back();
    } else {
        index = static_cast<u32>(m_bodies.size());
        m_bodies.emplace_back();
    }
    Body& b = m_bodies[index];
    b = Body{};
    b.alive = true;
    b.desc = desc;
    b.desc.rotation = glm::normalize(desc.rotation);
    b.reach = shapeReach(desc.shape);
    if (desc.type == BodyType::Dynamic) {
        const f32 mass = std::max(desc.mass, 0.001f);
        b.desc.mass = mass;
        b.invMass = 1.0f / mass;
        b.invInertia = inverseInertia(desc.shape, mass);
    }
    if (desc.type != BodyType::Static) b.velocity = toRaw(desc.linearVelocity);
    updateDormancy(b);
    ++m_alive;
    return {index};
}

void SkeletonPhysicsWorld::destroyBody(BodyId id) {
    Body* b = body(id);
    if (!b) return;
    b->alive = false;
    b->desc.shape.geometry.reset();
    m_free.push_back(id.value);
    --m_alive;
    std::erase_if(m_touching, [&](const auto& p) { return p.first == id.value || p.second == id.value; });
}

void SkeletonPhysicsWorld::setTransform(BodyId id, const GlobalPosition& position, const glm::quat& rotation) {
    if (Body* b = body(id)) {
        b->desc.position = position;
        b->desc.rotation = glm::normalize(rotation);
        b->hasTarget = false;
        updateDormancy(*b);
    }
}

void SkeletonPhysicsWorld::getTransform(BodyId id, GlobalPosition& position, glm::quat& rotation) const {
    if (const Body* b = body(id)) {
        position = b->desc.position;
        rotation = b->desc.rotation;
    }
}

void SkeletonPhysicsWorld::moveKinematic(BodyId id, const GlobalPosition& position, const glm::quat& rotation,
                                         Duration dt) {
    Body* b = body(id);
    if (!b || b->desc.type != BodyType::Kinematic) return;
    const f64 seconds = dt.seconds();
    if (seconds <= 0.0) {
        setTransform(id, position, rotation);
        return;
    }
    // The velocity that gets there in `dt` (pushes dynamic bodies); the pose itself is set exactly.
    const glm::dvec3 offset = glm::dvec3((position - b->desc.position).raw());
    const f64 k = static_cast<f64>(1ll << kVelocityBits) / seconds;
    b->velocity = {std::llround(offset.x * k), std::llround(offset.y * k), std::llround(offset.z * k)};
    glm::quat delta = glm::normalize(rotation) * glm::conjugate(b->desc.rotation);
    if (delta.w < 0.0f) delta = -delta;
    const f32 angle = 2.0f * std::acos(std::min(delta.w, 1.0f));
    const glm::vec3 axis(delta.x, delta.y, delta.z);
    const f32 len = glm::length(axis);
    b->angular = len > 1e-7f ? axis / len * (angle / static_cast<f32>(seconds)) : glm::vec3(0.0f);
    b->hasTarget = true;
    b->targetPosition = position;
    b->targetRotation = glm::normalize(rotation);
}

glm::vec3 SkeletonPhysicsWorld::linearVelocity(BodyId id) const {
    const Body* b = body(id);
    return b ? fromRaw(b->velocity) : glm::vec3(0.0f);
}
glm::i64vec3 SkeletonPhysicsWorld::rawVelocity(BodyId id) const {
    const Body* b = body(id);
    return b ? b->velocity : glm::i64vec3(0);
}
void SkeletonPhysicsWorld::setLinearVelocity(BodyId id, const glm::vec3& v) {
    if (Body* b = body(id); b && b->desc.type != BodyType::Static) b->velocity = toRaw(v);
}
glm::vec3 SkeletonPhysicsWorld::angularVelocity(BodyId id) const {
    const Body* b = body(id);
    return b ? b->angular : glm::vec3(0.0f);
}
void SkeletonPhysicsWorld::setAngularVelocity(BodyId id, const glm::vec3& v) {
    if (Body* b = body(id); b && b->desc.type != BodyType::Static) b->angular = v;
}
void SkeletonPhysicsWorld::addForce(BodyId id, const glm::vec3& f) {
    if (Body* b = body(id); b && b->desc.type == BodyType::Dynamic) b->force += f;
}
void SkeletonPhysicsWorld::addImpulse(BodyId id, const glm::vec3& j) {
    if (Body* b = body(id); b && b->desc.type == BodyType::Dynamic) b->velocity += toRaw(j * b->invMass);
}
void SkeletonPhysicsWorld::addTorque(BodyId id, const glm::vec3& t) {
    if (Body* b = body(id); b && b->desc.type == BodyType::Dynamic) b->torque += t;
}
bool SkeletonPhysicsWorld::isActive(BodyId id) const {
    const Body* b = body(id);
    return b && b->desc.type != BodyType::Static && !b->dormant; // TODO(engine): sleeping
}

// ------------------------------------------------------------------- focus --
void SkeletonPhysicsWorld::updateDormancy(Body& b) const {
    b.dormant = false;
    if (b.desc.type == BodyType::Static || m_focus.radius <= 0.0 || m_focus.foci.empty()) return;
    b.dormant = std::none_of(m_focus.foci.begin(), m_focus.foci.end(), [&](const GlobalPosition& f) {
        return isWithin(b.desc.position, f, m_focus.radius);
    });
}

void SkeletonPhysicsWorld::setFocus(const SimulationFocus& focus) {
    m_focus = focus; // one region at the world origin: rebase distance and wall range do not apply
    for (Body& b : m_bodies)
        if (b.alive) {
            const bool was = b.dormant;
            updateDormancy(b);
            if (b.dormant && !was) // a frozen body touches nothing: its pairs start over when it wakes
                std::erase_if(m_touching, [&](const auto& p) {
                    const u64 index = static_cast<u64>(&b - m_bodies.data());
                    return p.first == index || p.second == index;
                });
        }
}

bool SkeletonPhysicsWorld::isDormant(BodyId id) const {
    const Body* b = body(id);
    return b && b->dormant;
}

// -------------------------------------------------------------------- step --
void SkeletonPhysicsWorld::findContacts(f32 dt, std::vector<Contact>& out) const {
    const PhysicsCaps c = caps();
    auto moving = [](const Body& b) { return b.alive && !b.dormant && b.desc.type == BodyType::Dynamic; };
    auto present = [](const Body& b) { return b.alive && !b.dormant; };

    // Body pairs. TODO(engine): sort-and-sweep or a grid instead of all pairs.
    for (size_t i = 0; i < m_bodies.size(); ++i) {
        const Body& a = m_bodies[i];
        if (!present(a)) continue;
        for (size_t j = i + 1; j < m_bodies.size(); ++j) {
            const Body& b = m_bodies[j];
            if (!present(b) || !(moving(a) || moving(b))) continue;
            if (a.desc.isSensor && b.desc.isSensor) continue;
            if (!c.collidesWith(a.desc.shape.type, b.desc.shape.type)) continue;
            // Broad phase, exact: the int64 offset against the bounding spheres plus the step's motion.
            const f32 motion = (glm::length(fromRaw(a.velocity)) + glm::length(fromRaw(b.velocity))) * dt;
            const GlobalOffset offset = b.desc.position - a.desc.position;
            if (!offset.isShorterThan(a.reach + b.reach + kSpeculativeMargin + motion)) continue;
            // Narrow phase in a frame anchored on A: the offset is small now, floats are precise.
            Contact ct;
            ct.a = static_cast<i64>(i);
            ct.b = static_cast<i64>(j);
            ct.frame = a.desc.position;
            if (!collide(a.desc.shape, glm::vec3(0.0f), a.desc.rotation, b.desc.shape, offset.toVec3(),
                         b.desc.rotation, ct.m))
                continue;
            if (ct.m.separation > kSpeculativeMargin + motion) continue;
            ct.restitution = std::max(a.desc.restitution, b.desc.restitution);
            ct.sensor = a.desc.isSensor || b.desc.isSensor;
            out.push_back(ct);
        }
    }

    // Barrier edges: static planes, in a frame anchored on the body.
    const WorldSpace& space = WorldSpace::active();
    for (size_t j = 0; j < m_bodies.size(); ++j) {
        const Body& b = m_bodies[j];
        if (!moving(b) || b.desc.isSensor) continue;
        const f32 motion = glm::length(fromRaw(b.velocity)) * dt;
        const glm::vec3 center = b.desc.rotation * b.desc.shape.offset;
        for (int axis = 0; axis < 3; ++axis) {
            if (space.axis(axis).boundary != WorldBoundary::Barrier) continue;
            const f32 extent = extentAlong(b.desc.shape, b.desc.rotation, axis);
            for (bool high : {false, true}) {
                const f64 edge = edgeDistance(b.desc.position, axis, high); // >= 0
                if (edge > b.reach + kSpeculativeMargin + motion) continue;
                const f32 sign = high ? 1.0f : -1.0f;
                Contact ct;
                ct.b = static_cast<i64>(j);
                ct.wall = wallCode(axis, high);
                ct.frame = b.desc.position;
                Manifold& m = ct.m;
                m.normal = glm::vec3(0.0f);
                m.normal[axis] = -sign; // from the wall into the world
                m.separation = static_cast<f32>(edge) - sign * center[axis] - extent;
                m.pointA = center;
                m.pointA[axis] = sign * static_cast<f32>(edge);
                m.pointB = center;
                m.pointB[axis] += sign * extent;
                m.surfaceA = m.normal;
                m.surfaceB = -m.normal;
                if (m.separation > kSpeculativeMargin + motion) continue;
                ct.restitution = b.desc.restitution;
                out.push_back(ct);
            }
        }
    }
}

void SkeletonPhysicsWorld::solve(std::vector<Contact>& contacts, f32 dt) {
    // Working velocities (m/s) of the bodies in contact. TODO(engine): fixed point.
    std::vector<glm::vec3> v(m_bodies.size(), glm::vec3(0.0f));
    std::vector<char> used(m_bodies.size(), 0);
    auto velocityOf = [&](i64 i) -> glm::vec3& {
        if (!used[i]) {
            used[i] = 1;
            v[i] = m_bodies[i].desc.type == BodyType::Static ? glm::vec3(0.0f) : fromRaw(m_bodies[i].velocity);
        }
        return v[i];
    };
    auto invMass = [&](i64 i) { return i < 0 ? 0.0f : m_bodies[i].invMass; };
    auto relative = [&](const Contact& c) {
        const glm::vec3 va = c.a < 0 ? glm::vec3(0.0f) : velocityOf(c.a);
        return glm::dot(velocityOf(c.b) - va, c.m.normal);
    };

    for (Contact& c : contacts) {
        if (c.sensor) continue;
        const f32 s = c.m.separation;
        const f32 vn = relative(c);
        // Speculative: close the gap but not more; penetrating: push apart a fraction per step.
        c.target = s > 0.0f ? -s / dt : (s < -kPenetrationSlop ? (-s - kPenetrationSlop) * kBaumgarte / dt : 0.0f);
        // Bouncy and closing this step: leave with the restitution speed (a gap of up to one step's
        // motion may remain). TODO(engine): a relax pass to close the gap and then bounce.
        if (c.restitution > 0.0f && vn < -kBounceThreshold && s + vn * dt <= 0.0f)
            c.target = std::max(c.target, -c.restitution * vn);
    }
    for (int it = 0; it < kSolverIterations; ++it)
        for (Contact& c : contacts) {
            if (c.sensor) continue;
            const f32 wa = invMass(c.a), wb = invMass(c.b);
            if (wa + wb <= 0.0f) continue;
            const f32 vn = relative(c);
            const f32 next = std::max(c.impulse + (c.target - vn) / (wa + wb), 0.0f);
            const f32 delta = next - c.impulse;
            c.impulse = next;
            if (c.a >= 0) velocityOf(c.a) -= c.m.normal * (delta * wa);
            velocityOf(c.b) += c.m.normal * (delta * wb);
        }
    for (size_t i = 0; i < m_bodies.size(); ++i)
        if (used[i] && m_bodies[i].desc.type == BodyType::Dynamic) m_bodies[i].velocity = toRaw(v[i]);

    // Events: pairs whose gap at the end of the step is (about) closed and were not touching before.
    std::set<std::pair<u64, u64>> touching;
    for (const Contact& c : contacts) {
        const f32 vn = relative(c);
        const f32 gap = c.m.separation + vn * dt;
        if (gap > (c.sensor ? 0.0f : kTouchSlop)) continue;
        const std::pair<u64, u64> key{c.a < 0 ? c.wall : static_cast<u64>(c.a), static_cast<u64>(c.b)};
        touching.insert(key);
        if (m_touching.contains(key)) continue;
        ContactEvent e;
        if (c.a >= 0) {
            e.bodyA = {static_cast<u32>(c.a)};
            e.userDataA = m_bodies[c.a].desc.userData;
        } else {
            e.userDataA = c.wall;
        }
        e.bodyB = {static_cast<u32>(c.b)};
        e.userDataB = m_bodies[c.b].desc.userData;
        e.normal = c.m.normal;
        const glm::vec3 va = c.a < 0 ? glm::vec3(0.0f) : velocityOf(c.a); // points at the end of the step
        e.pointA = c.frame + (c.m.pointA + va * dt);
        e.pointB = c.frame + (c.m.pointB + velocityOf(c.b) * dt);
        e.surfaceNormalA = c.m.surfaceA;
        e.surfaceNormalB = c.m.surfaceB;
        m_events.push_back(e);
    }
    m_touching = std::move(touching);
}

void SkeletonPhysicsWorld::step(Duration duration) {
    const i64 dtq = secondsQ32(duration);
    if (dtq <= 0) return;
    const f32 dt = duration.secondsF();

    // 1. Velocities (fixed point, exact): gravity and forces as accelerations, then damping.
    for (Body& b : m_bodies) {
        if (!b.alive || b.dormant || b.desc.type != BodyType::Dynamic) continue;
        const glm::i64vec3 accel = toRaw(m_gravity * b.desc.gravityFactor + b.force * b.invMass);
        for (int i = 0; i < 3; ++i) b.velocity[i] = detail::saturatingAdd(b.velocity[i], detail::mulShift(accel[i], dtq, 32));
        const i64 damping = dampingQ32(b.desc.linearDamping, dt);
        for (int i = 0; i < 3; ++i) b.velocity[i] = detail::mulShift(b.velocity[i], damping, 32);
        // Angular motion in floats. TODO(engine): fixed point.
        const glm::mat3 r = glm::mat3_cast(b.desc.rotation);
        b.angular += r * (b.invInertia * (glm::transpose(r) * b.torque)) * dt;
        b.angular *= std::max(0.0f, 1.0f - b.desc.angularDamping * dt);
        b.force = b.torque = glm::vec3(0.0f);
    }

    // 2–3. Contacts and impulses (also emits the contact events).
    std::vector<Contact> contacts;
    findContacts(dt, contacts);
    solve(contacts, dt);

    // 4. Positions: the exact product velocity · dt, in world steps; rotations from the spin.
    for (Body& b : m_bodies) {
        if (!b.alive || b.dormant || b.desc.type == BodyType::Static) continue;
        if (b.hasTarget) {
            b.desc.position = b.targetPosition;
            b.desc.rotation = b.targetRotation;
            b.hasTarget = false;
            continue;
        }
        const GlobalOffset move(detail::mulShift(b.velocity.x, dtq, 32 + kVelocityBits),
                                detail::mulShift(b.velocity.y, dtq, 32 + kVelocityBits),
                                detail::mulShift(b.velocity.z, dtq, 32 + kVelocityBits));
        const u8 blocked = b.desc.position.move(move); // wraps across seams; stops at barriers
        for (int i = 0; i < 3; ++i)
            if (blocked & (1u << i)) b.velocity[i] = 0;
        if (b.angular != glm::vec3(0.0f)) {
            const glm::quat spin(0.0f, b.angular.x, b.angular.y, b.angular.z);
            b.desc.rotation = glm::normalize(b.desc.rotation + (spin * b.desc.rotation) * (0.5f * dt));
        }
    }
}

std::vector<ContactEvent> SkeletonPhysicsWorld::drainContactEvents() { return std::exchange(m_events, {}); }

// ----------------------------------------------------------------- queries --
// A query runs in a frame anchored on its origin; each target is brought in by its exact offset.
bool SkeletonPhysicsWorld::castAgainst(const Body& target, u32 index, const ShapeDesc* shape,
                                       const GlobalPosition& origin, const glm::quat& rotation, const glm::vec3& d,
                                       f32 maxDistance, QueryHit& hit) const {
    const ShapeDesc& ts = target.desc.shape;
    const PhysicsCaps c = caps();
    if (!((shape ? c.sweepTargets : c.raycastShapes) & shapeBit(ts.type))) return false;
    const GlobalOffset offset = target.desc.position - origin;
    if (!offset.isShorterThan(maxDistance + target.reach + (shape ? shapeReach(*shape) : 0.0))) return false;
    const glm::vec3 pos = offset.toVec3();
    const glm::vec3 center = pos + target.desc.rotation * ts.offset;
    f32 t = 0.0f;

    if (!shape) { // ray
        glm::vec3 n;
        const bool ok = ts.type == ShapeType::Sphere ? raySphere(d, center, ts.radius, t, n)
                                                     : rayBox(d, center, target.desc.rotation, ts.halfExtents, t, n);
        if (!ok || t > maxDistance) return false;
        hit = toQueryHit(origin, d * t, n, n, glm::vec3(0.0f), glm::vec3(0.0f), t, maxDistance);
        hit.sweptPoint = origin;
    } else { // sphere sweep
        const f32 r = shape->radius;
        const glm::vec3 start = rotation * shape->offset;
        Manifold m;
        if (ts.type == ShapeType::Sphere) {
            glm::vec3 n;
            if (!raySphere(d, center - start, ts.radius + r, t, n) || t > maxDistance) return false;
            m = flipped(sphereSphere(start + d * t, r, center, ts.radius)); // A = target, B = the swept sphere
        } else {
            // Conservative advancement: move by the current gap until touching.
            f32 previous = std::numeric_limits<f32>::infinity();
            int it = 0;
            for (; it < kMaxCastIterations; ++it) {
                m = boxSphere(center, target.desc.rotation, ts.halfExtents, start + d * t, r);
                if (m.separation <= kQueryEpsilon) break;
                if (m.separation >= previous) return false; // moving away: missed
                previous = m.separation;
                t += m.separation;
                if (t > maxDistance) return false;
            }
            if (it == kMaxCastIterations) return false;
        }
        hit = toQueryHit(origin, m.pointA, m.normal, m.surfaceA, m.pointB, m.surfaceB, t, maxDistance);
    }
    hit.body = {index};
    hit.userData = target.desc.userData;
    return true;
}

void SkeletonPhysicsWorld::castAgainstBarriers(const ShapeDesc* shape, const GlobalPosition& origin,
                                               const glm::quat& rotation, const glm::vec3& d, f32 maxDistance,
                                               std::vector<QueryHit>& hits) const {
    const WorldSpace& space = WorldSpace::active();
    const glm::vec3 start = shape ? rotation * shape->offset : glm::vec3(0.0f);
    const f32 r = shape ? shape->radius : 0.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (space.axis(axis).boundary != WorldBoundary::Barrier || std::abs(d[axis]) < 1e-12f) continue;
        const bool high = d[axis] > 0;
        const f64 edge = edgeDistance(origin, axis, high);
        const f64 gap = edge - (high ? 1.0 : -1.0) * start[axis] - r; // along the axis
        const f64 t = std::max(0.0, gap / std::abs(d[axis]));
        if (t > maxDistance) continue;
        glm::vec3 n(0.0f);
        n[axis] = high ? -1.0f : 1.0f;
        const glm::vec3 center = start + d * static_cast<f32>(t);
        glm::vec3 point = center;
        point[axis] = (high ? 1.0f : -1.0f) * static_cast<f32>(edge);
        QueryHit h = toQueryHit(origin, point, n, n, shape ? center - n * r : glm::vec3(0.0f),
                                shape ? -n : glm::vec3(0.0f), static_cast<f32>(t), maxDistance);
        if (!shape) h.sweptPoint = origin;
        h.userData = wallCode(axis, high);
        hits.push_back(h);
    }
}

std::vector<QueryHit> SkeletonPhysicsWorld::castAll(const ShapeDesc* shape, const GlobalPosition& origin,
                                                    const glm::quat& rotation, const glm::vec3& direction,
                                                    f32 maxDistance, const QueryFilter& filter) const {
    std::vector<QueryHit> hits;
    const f32 len = glm::length(direction);
    if (len <= 0.0f || maxDistance <= 0.0f) return hits;
    if (shape && !(caps().sweepShapes & shapeBit(shape->type))) {
        static bool warned = false;
        if (!warned) NOVA_CORE_WARN("[physics] Skeleton: sweeps support spheres only");
        warned = true;
        return hits;
    }
    const glm::vec3 d = direction / len;
    for (u32 i = 0; i < m_bodies.size(); ++i) {
        const Body& b = m_bodies[i];
        if (!b.alive || b.dormant || BodyId{i} == filter.ignoreBody) continue;
        if (b.desc.isSensor && !filter.includeSensors) continue;
        QueryHit h;
        if (castAgainst(b, i, shape, origin, rotation, d, maxDistance, h)) hits.push_back(h);
    }
    castAgainstBarriers(shape, origin, rotation, d, maxDistance, hits);
    std::stable_sort(hits.begin(), hits.end(), [](const QueryHit& a, const QueryHit& b) { return a.distance < b.distance; });
    return hits;
}

bool SkeletonPhysicsWorld::raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance,
                                   RaycastHit& hit, const QueryFilter& filter) const {
    const auto hits = castAll(nullptr, origin, kIdentity, direction, maxDistance, filter);
    if (hits.empty()) return false;
    hit = hits.front();
    return true;
}

bool SkeletonPhysicsWorld::sweep(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                                 const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                                 const QueryFilter& filter) const {
    const auto hits = castAll(&shape, position, rotation, direction, maxDistance, filter);
    if (hits.empty()) return false;
    hit = hits.front();
    return true;
}

std::vector<SweepHit> SkeletonPhysicsWorld::sweepAll(const ShapeDesc& shape, const GlobalPosition& position,
                                                     const glm::quat& rotation, const glm::vec3& direction,
                                                     f32 maxDistance, const QueryFilter& filter) const {
    return castAll(&shape, position, rotation, direction, maxDistance, filter);
}

bool SkeletonPhysicsWorld::sweepBody(BodyId id, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                                     const QueryFilter& filter) const {
    const Body* b = body(id);
    if (!b) return false;
    QueryFilter f = filter;
    f.ignoreBody = id;
    return sweep(b->desc.shape, b->desc.position, b->desc.rotation, direction, maxDistance, hit, f);
}

} // namespace nova::physics
