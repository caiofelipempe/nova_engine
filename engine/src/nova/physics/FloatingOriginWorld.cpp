#include "nova/physics/FloatingOriginWorld.h"

#include <algorithm>
#include <cmath>

namespace nova::physics {

namespace {

// Exact centroid: the first point plus the average offset to the others (integer division).
GlobalPosition centroid(const std::vector<GlobalPosition>& points) {
    if (points.empty()) return {};
    const GlobalPosition& first = points.front();
    i64 sum[3] = {0, 0, 0};
    for (const GlobalPosition& p : points) {
        const GlobalOffset d = p - first;
        for (int i = 0; i < 3; ++i) sum[i] = detail::saturatingAdd(sum[i], d[i]);
    }
    const i64 n = static_cast<i64>(points.size());
    return first + GlobalOffset(sum[0] / n, sum[1] / n, sum[2] / n);
}

// Groups points closer than `distance` (transitively), in a deterministic order.
std::vector<std::vector<GlobalPosition>> cluster(const std::vector<GlobalPosition>& points, f64 distance) {
    std::vector<int> group(points.size(), -1);
    std::vector<std::vector<GlobalPosition>> out;
    for (size_t i = 0; i < points.size(); ++i) {
        if (group[i] >= 0) continue;
        group[i] = static_cast<int>(out.size());
        out.push_back({points[i]});
        for (size_t k = 0; k < out.back().size(); ++k)
            for (size_t j = i + 1; j < points.size(); ++j)
                if (group[j] < 0 && isWithin(out.back()[k], points[j], distance)) {
                    group[j] = group[i];
                    out.back().push_back(points[j]);
                }
    }
    return out;
}

// Radius of a sphere around the body origin that contains its shape (meters).
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

local::BodyDesc toLocalDesc(const BodyDesc& d, const glm::vec3& position, u64 userData) {
    local::BodyDesc l;
    l.shape = d.shape;
    l.type = d.type;
    l.position = position;
    l.rotation = d.rotation;
    l.mass = d.mass;
    l.friction = d.friction;
    l.restitution = d.restitution;
    l.linearDamping = d.linearDamping;
    l.angularDamping = d.angularDamping;
    l.gravityFactor = d.gravityFactor;
    l.isSensor = d.isSensor;
    l.userData = userData;
    return l;
}

} // namespace

FloatingOriginWorld::FloatingOriginWorld(Backend backend, const WorldDesc& desc)
    : m_backend(backend), m_desc(desc), m_gravity(desc.gravity) {}

FloatingOriginWorld::~FloatingOriginWorld() = default;

// ----------------------------------------------------------------- regions --
FloatingOriginWorld::Region* FloatingOriginWorld::findRegion(u32 id) {
    for (Region& r : m_regions)
        if (r.id == id) return &r;
    return nullptr;
}

const FloatingOriginWorld::Region* FloatingOriginWorld::findRegion(u32 id) const {
    for (const Region& r : m_regions)
        if (r.id == id) return &r;
    return nullptr;
}

GlobalPosition FloatingOriginWorld::regionOrigin(u32 region) const {
    return region < m_regions.size() ? m_regions[region].origin : GlobalPosition();
}

FloatingOriginWorld::Region& FloatingOriginWorld::createRegion(const GlobalPosition& origin) {
    Region r;
    r.id = m_nextRegionId++;
    r.world = local::World::create(m_backend, m_desc);
    r.world->setGravity(m_gravity);
    r.origin = origin;
    m_regions.push_back(std::move(r));
    rebuildWalls(m_regions.back());
    return m_regions.back();
}

void FloatingOriginWorld::destroyRegion(size_t index) {
    const u32 id = m_regions[index].id;
    for (u32 i = 0; i < m_bodies.size(); ++i) // bodies leave it (dynamic ones keep their motion)
        for (size_t k = m_bodies[i].instances.size(); k-- > 0;)
            if (m_bodies[i].instances[k].region == id) removeInstance(i, k);
    m_regions.erase(m_regions.begin() + static_cast<std::ptrdiff_t>(index));
}

void FloatingOriginWorld::rebaseRegion(Region& r, const GlobalPosition& target) {
    if (isWithin(target, r.origin, m_focus.rebaseDistance)) return;
    // Shift by an amount floats represent exactly: the integer origin and the float engine agree
    // to the bit after the move, however often it happens.
    const glm::vec3 shift = (target - r.origin).toVec3();
    r.origin += GlobalOffset::fromMeters(shift);
    r.world->shiftOrigin(shift);
    rebuildWalls(r);
}

void FloatingOriginWorld::rebuildWalls(Region& r) {
    for (BodyId wall : r.walls) r.world->destroyBody(wall);
    r.walls.clear();
    const WorldSpace& world = WorldSpace::active();
    const f32 thickness = 1.0f, span = static_cast<f32>(m_focus.barrierWallRange);
    for (int axis = 0; axis < 3; ++axis) {
        const WorldSpace::Axis& a = world.axis(axis);
        if (a.boundary != WorldBoundary::Barrier) continue;
        for (int side : {-1, 1}) {
            const i64 edgeRaw = side < 0 ? a.min : a.max;
            const f64 toEdge = static_cast<f64>(world.difference(axis, edgeRaw, r.origin.raw(axis))) * world.unit();
            if (std::abs(toEdge) > m_focus.barrierWallRange) continue;
            local::BodyDesc d;
            d.type = BodyType::Static;
            d.shape.type = ShapeType::Box;
            d.shape.halfExtents = glm::vec3(span);
            d.shape.halfExtents[axis] = thickness;
            d.position = glm::vec3(0.0f);
            d.position[axis] = static_cast<f32>(toEdge) + side * thickness; // just outside the edge
            d.friction = 0.5f;
            d.restitution = 0.0f; // the edge adds no bounce of its own
            d.userData = kWorldBoundaryUserData | static_cast<u64>(axis * 2 + (side > 0 ? 1 : 0));
            if (BodyId wall = r.world->createBody(d)) r.walls.push_back(wall);
        }
    }
}

void FloatingOriginWorld::setFocus(const SimulationFocus& focus) {
    m_focus = focus;
    updateRegions();
}

void FloatingOriginWorld::updateRegions() {
    const std::vector<GlobalPosition>& foci = m_focus.foci;
    m_bounded = m_focus.radius > 0.0 && !foci.empty();
    if (!m_bounded) {
        // Unlimited: one region holds every body; its origin follows the foci (if any).
        if (m_regions.empty()) createRegion(foci.empty() ? GlobalPosition() : centroid(foci));
        while (m_regions.size() > 1) destroyRegion(m_regions.size() - 1);
        m_regions.front().foci = foci;
        if (!foci.empty()) rebaseRegion(m_regions.front(), centroid(foci));
    } else {
        // Foci closer than two radii share a region (their simulation spheres overlap).
        const auto clusters = cluster(foci, 2.0 * m_focus.radius);
        std::vector<u32> claimed, regionOf(clusters.size(), 0);
        for (size_t c = 0; c < clusters.size(); ++c) {
            const GlobalPosition center = centroid(clusters[c]);
            Region* best = nullptr;
            UInt128 bestDistance;
            for (Region& r : m_regions) { // keep the nearest unclaimed region still around these foci
                if (std::find(claimed.begin(), claimed.end(), r.id) != claimed.end()) continue;
                if (!isWithin(r.origin, center, 4.0 * m_focus.radius)) continue;
                const UInt128 d = distanceSquared(r.origin, center);
                if (!best || d < bestDistance) best = &r, bestDistance = d;
            }
            const u32 id = best ? best->id : createRegion(center).id;
            claimed.push_back(id);
            regionOf[c] = id;
        }
        for (size_t i = m_regions.size(); i-- > 0;)
            if (std::find(claimed.begin(), claimed.end(), m_regions[i].id) == claimed.end()) destroyRegion(i);
        for (size_t c = 0; c < clusters.size(); ++c) {
            Region& r = *findRegion(regionOf[c]);
            r.foci = clusters[c];
            rebaseRegion(r, centroid(clusters[c]));
        }
    }
    for (u32 i = 0; i < m_bodies.size(); ++i)
        if (m_bodies[i].alive) assign(i);
}

const FloatingOriginWorld::Region* FloatingOriginWorld::regionAt(const GlobalPosition& p) const {
    if (m_regions.empty()) return nullptr;
    if (!m_bounded) return &m_regions.front();
    const Region* best = nullptr;
    UInt128 bestDistance;
    for (const Region& r : m_regions)
        for (const GlobalPosition& f : r.foci) {
            const UInt128 d = distanceSquared(f, p);
            if (!best || d < bestDistance) best = &r, bestDistance = d;
        }
    if (!best) return nullptr;
    const f64 limit = m_focus.radius / WorldSpace::active().unit();
    return bestDistance.toDouble() <= limit * limit ? best : nullptr;
}

// ------------------------------------------------------------------ bodies --
const FloatingOriginWorld::Body* FloatingOriginWorld::body(BodyId id) const {
    return id.valid() && id.value < m_bodies.size() && m_bodies[id.value].alive ? &m_bodies[id.value] : nullptr;
}
FloatingOriginWorld::Body* FloatingOriginWorld::body(BodyId id) {
    return id.valid() && id.value < m_bodies.size() && m_bodies[id.value].alive ? &m_bodies[id.value] : nullptr;
}

GlobalPosition FloatingOriginWorld::currentPosition(u32 index) const {
    const Body& b = m_bodies[index];
    if (const Instance* h = home(b); h && b.desc.type == BodyType::Dynamic)
        if (const Region* r = findRegion(h->region)) {
            glm::vec3 p;
            glm::quat q;
            r->world->getTransform(h->local, p, q);
            return r->toGlobal(p);
        }
    return b.desc.position;
}

void FloatingOriginWorld::assign(u32 index) {
    Body& b = m_bodies[index];
    const GlobalPosition position = currentPosition(index);

    std::vector<u32> wanted; // regions that should hold this body
    if (!m_bounded) {
        if (!m_regions.empty()) wanted.push_back(m_regions.front().id);
    } else {
        const f64 reach = m_focus.radius + shapeReach(b.desc.shape);
        if (b.desc.type == BodyType::Static) { // every region it reaches
            for (const Region& r : m_regions)
                for (const GlobalPosition& f : r.foci)
                    if (isWithin(f, position, reach)) {
                        wanted.push_back(r.id);
                        break;
                    }
        } else { // the region of the nearest focus, if within reach; else dormant
            const Region* best = nullptr;
            UInt128 bestDistance;
            for (const Region& r : m_regions)
                for (const GlobalPosition& f : r.foci) {
                    const UInt128 d = distanceSquared(f, position);
                    if (!best || d < bestDistance) best = &r, bestDistance = d;
                }
            const f64 limit = reach / WorldSpace::active().unit();
            if (best && bestDistance.toDouble() <= limit * limit) wanted.push_back(best->id);
        }
    }
    for (size_t k = b.instances.size(); k-- > 0;)
        if (std::find(wanted.begin(), wanted.end(), b.instances[k].region) == wanted.end()) removeInstance(index, k);
    for (u32 id : wanted) {
        const bool present = std::any_of(m_bodies[index].instances.begin(), m_bodies[index].instances.end(),
                                         [id](const Instance& i) { return i.region == id; });
        if (!present)
            if (Region* r = findRegion(id)) createInstance(index, *r);
    }
}

void FloatingOriginWorld::removeInstance(u32 index, size_t k) {
    Body& b = m_bodies[index];
    const Instance inst = b.instances[k];
    if (Region* r = findRegion(inst.region)) {
        if (b.desc.type == BodyType::Dynamic && k == 0) { // keep its pose and motion for later
            glm::vec3 p;
            glm::quat q;
            r->world->getTransform(inst.local, p, q);
            b.desc.position = r->toGlobal(p);
            b.desc.rotation = q;
            b.savedLinear = r->world->linearVelocity(inst.local);
            b.savedAngular = r->world->angularVelocity(inst.local);
        }
        r->world->destroyBody(inst.local);
    }
    b.instances.erase(b.instances.begin() + static_cast<std::ptrdiff_t>(k));
}

void FloatingOriginWorld::createInstance(u32 index, Region& region) {
    Body& b = m_bodies[index];
    local::BodyDesc d = toLocalDesc(b.desc, region.toLocal(b.desc.position), index);
    if (b.desc.type == BodyType::Dynamic) d.linearVelocity = b.savedLinear;
    const BodyId id = region.world->createBody(d);
    if (!id) return;
    if (b.desc.type == BodyType::Dynamic && b.savedAngular != glm::vec3(0.0f))
        region.world->setAngularVelocity(id, b.savedAngular);
    b.savedLinear = b.savedAngular = glm::vec3(0.0f);
    b.instances.push_back({region.id, id});
}

BodyId FloatingOriginWorld::createBody(const BodyDesc& desc) {
    if (m_regions.empty()) updateRegions();
    u32 index;
    if (!m_freeBodies.empty()) {
        index = m_freeBodies.back();
        m_freeBodies.pop_back();
    } else {
        index = static_cast<u32>(m_bodies.size());
        m_bodies.emplace_back();
    }
    Body& b = m_bodies[index];
    b = Body{};
    b.alive = true;
    b.desc = desc;
    b.savedLinear = desc.linearVelocity;
    assign(index);
    return {index};
}

void FloatingOriginWorld::destroyBody(BodyId id) {
    Body* b = body(id);
    if (!b) return;
    for (const Instance& inst : b->instances)
        if (Region* r = findRegion(inst.region)) r->world->destroyBody(inst.local);
    *b = Body{};
    m_freeBodies.push_back(id.value);
}

u32 FloatingOriginWorld::bodyCount() const {
    return static_cast<u32>(std::count_if(m_bodies.begin(), m_bodies.end(), [](const Body& b) { return b.alive; }));
}

bool FloatingOriginWorld::isDormant(BodyId id) const {
    const Body* b = body(id);
    return b && b->instances.empty();
}

void FloatingOriginWorld::setTransform(BodyId id, const GlobalPosition& position, const glm::quat& rotation) {
    Body* b = body(id);
    if (!b) return;
    b->desc.position = position;
    b->desc.rotation = rotation;
    for (const Instance& inst : b->instances)
        if (Region* r = findRegion(inst.region)) r->world->setTransform(inst.local, r->toLocal(position), rotation);
    assign(id.value); // it may now belong to another region
}

void FloatingOriginWorld::getTransform(BodyId id, GlobalPosition& position, glm::quat& rotation) const {
    const Body* b = body(id);
    if (!b) return;
    position = b->desc.position;
    rotation = b->desc.rotation;
    if (const Instance* h = home(*b); h && b->desc.type != BodyType::Static)
        if (const Region* r = findRegion(h->region)) {
            glm::vec3 p;
            r->world->getTransform(h->local, p, rotation);
            position = r->toGlobal(p);
        }
}

void FloatingOriginWorld::moveKinematic(BodyId id, const GlobalPosition& position, const glm::quat& rotation,
                                        Duration dt) {
    Body* b = body(id);
    if (!b) return;
    b->desc.position = position; // the target pose (also where a dormant kinematic body is)
    b->desc.rotation = rotation;
    if (const Instance* h = home(*b))
        if (Region* r = findRegion(h->region)) r->world->moveKinematic(h->local, r->toLocal(position), rotation, dt.secondsF());
}

// ------------------------------------------------------------------ motion --
glm::vec3 FloatingOriginWorld::linearVelocity(BodyId id) const {
    const Body* b = body(id);
    if (!b) return glm::vec3(0.0f);
    if (const Instance* h = home(*b))
        if (const Region* r = findRegion(h->region)) return r->world->linearVelocity(h->local);
    return b->savedLinear;
}
void FloatingOriginWorld::setLinearVelocity(BodyId id, const glm::vec3& v) {
    Body* b = body(id);
    if (!b) return;
    if (const Instance* h = home(*b)) {
        if (Region* r = findRegion(h->region)) r->world->setLinearVelocity(h->local, v);
    } else {
        b->savedLinear = v; // applied when it wakes
    }
}
glm::vec3 FloatingOriginWorld::angularVelocity(BodyId id) const {
    const Body* b = body(id);
    if (!b) return glm::vec3(0.0f);
    if (const Instance* h = home(*b))
        if (const Region* r = findRegion(h->region)) return r->world->angularVelocity(h->local);
    return b->savedAngular;
}
void FloatingOriginWorld::setAngularVelocity(BodyId id, const glm::vec3& v) {
    Body* b = body(id);
    if (!b) return;
    if (const Instance* h = home(*b)) {
        if (Region* r = findRegion(h->region)) r->world->setAngularVelocity(h->local, v);
    } else {
        b->savedAngular = v;
    }
}
void FloatingOriginWorld::addForce(BodyId id, const glm::vec3& force) {
    if (Body* b = body(id))
        if (const Instance* h = home(*b))
            if (Region* r = findRegion(h->region)) r->world->addForce(h->local, force);
}
void FloatingOriginWorld::addImpulse(BodyId id, const glm::vec3& impulse) {
    Body* b = body(id);
    if (!b) return;
    if (const Instance* h = home(*b)) {
        if (Region* r = findRegion(h->region)) r->world->addImpulse(h->local, impulse);
    } else if (b->desc.type == BodyType::Dynamic) {
        b->savedLinear += impulse / std::max(b->desc.mass, 0.001f);
    }
}
void FloatingOriginWorld::addTorque(BodyId id, const glm::vec3& torque) {
    if (Body* b = body(id))
        if (const Instance* h = home(*b))
            if (Region* r = findRegion(h->region)) r->world->addTorque(h->local, torque);
}
bool FloatingOriginWorld::isActive(BodyId id) const {
    const Body* b = body(id);
    if (!b) return false;
    if (const Instance* h = home(*b))
        if (const Region* r = findRegion(h->region)) return r->world->isActive(h->local);
    return false;
}

void FloatingOriginWorld::setGravity(const glm::vec3& gravity) {
    m_gravity = gravity;
    for (Region& r : m_regions) r.world->setGravity(gravity);
}

void FloatingOriginWorld::step(Duration dt) {
    if (m_regions.empty()) updateRegions();
    const f32 seconds = dt.secondsF();
    for (Region& r : m_regions) r.world->step(seconds);
    // Contacts back to global: local userData is the body index (or a world-edge code).
    for (Region& r : m_regions)
        for (const local::ContactEvent& c : r.world->drainContactEvents()) {
            ContactEvent e;
            auto map = [this](u64 localUser, BodyId& id, u64& user) {
                if (isWorldBoundary(localUser)) {
                    id = {};
                    user = localUser;
                } else {
                    id = {static_cast<u32>(localUser)};
                    user = localUser < m_bodies.size() ? m_bodies[localUser].desc.userData : 0;
                }
            };
            map(c.userDataA, e.bodyA, e.userDataA);
            map(c.userDataB, e.bodyB, e.userDataB);
            e.normal = c.normal;
            e.pointA = r.toGlobal(c.pointA);
            e.pointB = r.toGlobal(c.pointB);
            e.surfaceNormalA = c.surfaceNormalA;
            e.surfaceNormalB = c.surfaceNormalB;
            m_events.push_back(e);
        }
}

std::vector<ContactEvent> FloatingOriginWorld::drainContactEvents() { return std::exchange(m_events, {}); }

// ----------------------------------------------------------------- queries --
QueryFilter FloatingOriginWorld::toLocal(const QueryFilter& filter, const Region& region) const {
    QueryFilter out;
    out.includeSensors = filter.includeSensors;
    if (const Body* b = body(filter.ignoreBody))
        for (const Instance& inst : b->instances)
            if (inst.region == region.id) out.ignoreBody = inst.local;
    return out;
}

QueryHit FloatingOriginWorld::toGlobal(const local::QueryHit& hit, const Region& region,
                                       const GlobalPosition& queryOrigin) const {
    QueryHit out;
    if (isWorldBoundary(hit.userData)) {
        out.userData = hit.userData;
    } else {
        out.body = {static_cast<u32>(hit.userData)};
        out.userData = hit.userData < m_bodies.size() ? m_bodies[hit.userData].desc.userData : 0;
    }
    out.point = region.toGlobal(hit.point);
    out.normal = hit.normal;
    out.hitSurfaceNormal = hit.hitSurfaceNormal;
    out.sweptPoint = hit.sweptPoint == glm::vec3(0.0f) ? queryOrigin : region.toGlobal(hit.sweptPoint);
    out.sweptSurfaceNormal = hit.sweptSurfaceNormal;
    out.distance = hit.distance;
    out.fraction = hit.fraction;
    return out;
}

bool FloatingOriginWorld::raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance,
                                  RaycastHit& hit, const QueryFilter& filter) const {
    const Region* r = regionAt(origin);
    local::RaycastHit h;
    if (!r || !r->world->raycast(r->toLocal(origin), direction, maxDistance, h, toLocal(filter, *r))) return false;
    hit = toGlobal(h, *r, origin);
    return true;
}

bool FloatingOriginWorld::sweep(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                                const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                                const QueryFilter& filter) const {
    const Region* r = regionAt(position);
    local::SweepHit h;
    if (!r || !r->world->sweep(shape, r->toLocal(position), rotation, direction, maxDistance, h, toLocal(filter, *r)))
        return false;
    hit = toGlobal(h, *r, position);
    return true;
}

std::vector<SweepHit> FloatingOriginWorld::sweepAll(const ShapeDesc& shape, const GlobalPosition& position,
                                                    const glm::quat& rotation, const glm::vec3& direction,
                                                    f32 maxDistance, const QueryFilter& filter) const {
    std::vector<SweepHit> out;
    const Region* r = regionAt(position);
    if (!r) return out;
    for (const local::SweepHit& h :
         r->world->sweepAll(shape, r->toLocal(position), rotation, direction, maxDistance, toLocal(filter, *r)))
        out.push_back(toGlobal(h, *r, position));
    return out;
}

bool FloatingOriginWorld::sweepBody(BodyId id, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                                    const QueryFilter& filter) const {
    const Body* b = body(id);
    const Instance* h = b ? home(*b) : nullptr;
    const Region* r = h ? findRegion(h->region) : nullptr;
    if (!r) return false;
    local::SweepHit lh;
    if (!r->world->sweepBody(h->local, direction, maxDistance, lh, toLocal(filter, *r))) return false;
    GlobalPosition start;
    glm::quat q;
    getTransform(id, start, q);
    hit = toGlobal(lh, *r, start);
    return true;
}

} // namespace nova::physics
