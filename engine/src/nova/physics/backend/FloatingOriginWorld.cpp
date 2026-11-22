#include "nova/physics/backend/FloatingOriginWorld.h"

#include <algorithm>
#include <cmath>

namespace nova::physics {

namespace {

// Local userData of a character's capsule: this bit plus the character's index.
constexpr u64 kCharacterUserData = 1ull << 62;
bool isCharacter(u64 localUser) { return !isWorldBoundary(localUser) && (localUser & kCharacterUserData) != 0; }

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
        case ShapeType::Compound: {
            f64 r = 0.0;
            if (s.children)
                for (const ShapeDesc& c : *s.children) r = std::max(r, shapeReach(c));
            return r + offset;
        }
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
    l.layer = d.layer;
    l.continuousCollision = d.continuousCollision;
    l.lockedAxes = d.lockedAxes;
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
    r.world->setCollisionLayers(m_desc.layers);
    r.origin = origin;
    m_regions.push_back(std::move(r));
    rebuildWalls(m_regions.back());
    return m_regions.back();
}

void FloatingOriginWorld::destroyRegion(size_t index) {
    const u32 id = m_regions[index].id;
    for (u32 i = 0; i < m_characters.size(); ++i) // characters there go dormant
        if (m_characters[i].alive && m_characters[i].local && m_characters[i].region == id) removeCharacterInstance(i);
    for (u32 i = 0; i < m_bodies.size(); ++i) // bodies leave it (dynamic ones keep their motion)
        for (size_t k = m_bodies[i].instances.size(); k-- > 0;)
            if (m_bodies[i].instances[k].region == id) removeInstance(i, k);
    m_regions.erase(m_regions.begin() + static_cast<std::ptrdiff_t>(index));
}

namespace {
// Smallest span (m) of the wrapping axes of the active world (0 = no axis wraps within int64).
f64 smallestWrapSpan() {
    const WorldSpace& w = WorldSpace::active();
    f64 smallest = 0.0;
    for (int i = 0; i < 3; ++i) {
        const WorldSpace::Axis& a = w.axis(i);
        if (a.boundary != WorldBoundary::Wrap || a.full) continue;
        const f64 size = w.sizeMeters(i);
        smallest = smallest == 0.0 ? size : std::min(smallest, size);
    }
    return smallest;
}
} // namespace

void FloatingOriginWorld::rebaseRegion(Region& r, const GlobalPosition& target) {
    // In a world that wraps, the float window around the origin is one lap wide: follow the foci
    // closely enough that what is near them is never on the far side of the window.
    f64 distance = m_focus.rebaseDistance;
    if (const f64 span = smallestWrapSpan(); span > 0.0) distance = std::min(distance, span / 8.0);
    if (isWithin(target, r.origin, distance)) return;
    // Shift by an amount floats represent exactly: the integer origin and the float engine agree
    // to the bit after the move, however often it happens.
    const glm::vec3 shift = (target - r.origin).toVec3();
    r.origin += GlobalOffset::fromMeters(shift);
    r.world->shiftOrigin(shift);
    rebuildWalls(r);
    // Joints to the world hold a point in the region's coordinates: place them again.
    for (u32 j = 0; j < m_joints.size(); ++j)
        if (m_joints[j].alive && m_joints[j].local && m_joints[j].region == r.id && !m_joints[j].desc.bodyB) {
            uninstantiateJoint(j);
            instantiateJoint(j);
        }
}

void FloatingOriginWorld::rewrapBodies() {
    // A local world sees one lap of a wrapping axis around its origin. A body that moved more than
    // half a lap away (or a static one placed when the origin was elsewhere) is put back at the
    // same world place, the short way: velocity and contacts with its neighbours are unaffected.
    const f64 span = smallestWrapSpan();
    if (span <= 0.0) return;
    const f32 jump = static_cast<f32>(span * 0.25); // a real re-wrap moves by about a lap
    for (Body& b : m_bodies) {
        if (!b.alive) continue;
        for (const Instance& inst : b.instances) {
            Region* r = findRegion(inst.region);
            if (!r) continue;
            glm::vec3 p;
            glm::quat q;
            r->world->getTransform(inst.local, p, q);
            const GlobalPosition where = b.desc.type == BodyType::Dynamic ? r->toGlobal(p) : b.desc.position;
            const glm::vec3 shortest = r->toLocal(where);
            if (glm::length(shortest - p) > jump) r->world->setTransform(inst.local, shortest, q);
        }
    }
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
            d.layer = kWorldBoundaryLayer; // collides with every layer
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
    for (u32 i = 0; i < m_characters.size(); ++i)
        if (m_characters[i].alive) assignCharacter(i);
    rewrapBodies();
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
    endPairs(index, inst.region, false);
    std::vector<u32> joints;
    jointsOf(index, joints);
    for (u32 j : joints) // a joint must go before its body (it waits until both are back)
        if (m_joints[j].local && m_joints[j].region == inst.region) uninstantiateJoint(j);
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
    std::vector<u32> joints;
    jointsOf(index, joints);
    for (u32 j : joints) instantiateJoint(j);
}

BodyId FloatingOriginWorld::createBody(const BodyDesc& desc) {
    if (!isValidBody(desc)) return {}; // refused now, not later in whichever region it lands
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
    endPairs(id.value, 0, true);
    std::vector<u32> joints;
    jointsOf(id.value, joints);
    for (u32 j : joints) destroyJoint({j});
    b = body(id);
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

void FloatingOriginWorld::setBodyProperties(BodyId id, const BodyDesc& d) {
    Body* b = body(id);
    if (!b) return;
    b->desc.friction = d.friction;
    b->desc.restitution = d.restitution;
    b->desc.linearDamping = d.linearDamping;
    b->desc.angularDamping = d.angularDamping;
    b->desc.gravityFactor = d.gravityFactor;
    b->desc.isSensor = d.isSensor;
    b->desc.layer = d.layer;
    b->desc.continuousCollision = d.continuousCollision;
    const local::BodyDesc l = toLocalDesc(b->desc, glm::vec3(0.0f), id.value);
    for (const Instance& inst : b->instances)
        if (Region* r = findRegion(inst.region)) r->world->setBodyProperties(inst.local, l);
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

void FloatingOriginWorld::setCollisionLayers(const CollisionLayers& layers) {
    m_desc.layers = layers;
    for (Region& r : m_regions) r.world->setCollisionLayers(layers);
}

void FloatingOriginWorld::step(Duration dt) {
    if (m_regions.empty()) updateRegions();
    const f32 seconds = dt.secondsF();
    for (u32 i = 0; i < m_characters.size(); ++i) // the velocity they move with this step
        if (const Character& c = m_characters[i]; c.alive && c.local)
            if (Region* r = findRegion(c.region)) r->world->setCharacterVelocity(c.local, c.velocity);
    for (Region& r : m_regions) r.world->step(seconds);
    for (u32 i = 0; i < m_characters.size(); ++i)
        if (m_characters[i].alive && m_characters[i].local) readCharacter(i);
    for (Region& r : m_regions)
        for (const local::ContactEvent& c : r.world->drainContactEvents()) onLocalContact(r.id, c);
    // Breakable joints: what they held during the step.
    for (u32 j = 0; j < m_joints.size(); ++j) {
        Joint& joint = m_joints[j];
        if (!joint.alive || !joint.local || (joint.desc.breakForce <= 0.0f && joint.desc.breakTorque <= 0.0f)) continue;
        const Region* r = findRegion(joint.region);
        if (!r) continue;
        const local::JointLoad load = r->world->jointLoad(joint.local);
        const bool force = joint.desc.breakForce > 0.0f && load.force > joint.desc.breakForce;
        const bool torque = joint.desc.breakTorque > 0.0f && load.torque > joint.desc.breakTorque;
        if (!force && !torque) continue;
        uninstantiateJoint(j);
        joint.broken = true;
        m_jointBreaks.push_back({JointId{j}, joint.desc.userData});
    }
}

// -------------------------------------------------------------- characters --
CharacterId FloatingOriginWorld::createCharacter(const CharacterDesc& d) {
    if (m_regions.empty()) updateRegions();
    u32 index;
    if (!m_freeCharacters.empty()) {
        index = m_freeCharacters.back();
        m_freeCharacters.pop_back();
    } else {
        index = static_cast<u32>(m_characters.size());
        m_characters.emplace_back();
    }
    Character& c = m_characters[index];
    c = Character{};
    c.alive = true;
    c.desc = d;
    c.state.position = d.position;
    assignCharacter(index);
    return {index};
}

void FloatingOriginWorld::readCharacter(u32 index) {
    Character& c = m_characters[index];
    const Region* r = findRegion(c.region);
    if (!r) return;
    const local::CharacterState l = r->world->characterState(c.local);
    c.state.position = r->toGlobal(l.position);
    c.state.velocity = l.velocity;
    c.state.ground = l.ground;
    c.state.groundNormal = l.groundNormal;
    c.state.groundVelocity = l.groundVelocity;
    c.state.groundBody = {};
    c.state.groundUserData = 0;
    if (l.ground != GroundState::InAir && l.groundBody) {
        // The local body's user data is our body index (or a code): back to global.
        toGlobalIds(l.groundUserData, c.state.groundBody, c.state.groundUserData);
    }
    c.velocity = l.velocity;
}

void FloatingOriginWorld::removeCharacterInstance(u32 index) {
    Character& c = m_characters[index];
    if (!c.local) return;
    readCharacter(index);
    endPairs(kCharacterUserData | index, c.region, false);
    if (Region* r = findRegion(c.region)) r->world->destroyCharacter(c.local);
    c.local = {};
    c.state.ground = GroundState::InAir; // dormant: not standing on anything simulated
}

void FloatingOriginWorld::assignCharacter(u32 index) {
    Character& c = m_characters[index];
    const Region* wanted = regionAt(c.state.position);
    if (c.local && wanted && wanted->id == c.region) return;
    removeCharacterInstance(index);
    if (!wanted) return; // dormant
    Region& r = *findRegion(wanted->id);
    local::CharacterDesc l;
    l.radius = c.desc.radius;
    l.halfHeight = c.desc.halfHeight;
    l.position = r.toLocal(c.state.position);
    l.rotation = c.desc.rotation;
    l.maxSlopeAngle = c.desc.maxSlopeAngle;
    l.stepHeight = c.desc.stepHeight;
    l.stickToFloor = c.desc.stickToFloor;
    l.mass = c.desc.mass;
    l.maxPushForce = c.desc.maxPushForce;
    l.gravityFactor = c.desc.gravityFactor;
    l.layer = c.desc.layer;
    l.userData = kCharacterUserData | index;
    c.local = r.world->createCharacter(l);
    c.region = r.id;
    if (c.local) r.world->setCharacterVelocity(c.local, c.velocity);
}

void FloatingOriginWorld::destroyCharacter(CharacterId id) {
    if (!id || id.value >= m_characters.size() || !m_characters[id.value].alive) return;
    removeCharacterInstance(id.value);
    endPairs(kCharacterUserData | id.value, 0, true);
    m_characters[id.value] = Character{};
    m_freeCharacters.push_back(id.value);
}

void FloatingOriginWorld::setCharacterVelocity(CharacterId id, const glm::vec3& v) {
    if (!id || id.value >= m_characters.size() || !m_characters[id.value].alive) return;
    m_characters[id.value].velocity = v;
}

void FloatingOriginWorld::setCharacterTransform(CharacterId id, const GlobalPosition& position, const glm::quat& rotation) {
    if (!id || id.value >= m_characters.size() || !m_characters[id.value].alive) return;
    Character& c = m_characters[id.value];
    c.state.position = position;
    c.desc.rotation = rotation;
    if (c.local) {
        const Region* r = findRegion(c.region);
        if (r && regionAt(position) == r) {
            r->world->setCharacterTransform(c.local, r->toLocal(position), rotation);
            readCharacter(id.value);
            return;
        }
    }
    removeCharacterInstance(id.value); // somewhere else: placed again there
    c.state.position = position;
    assignCharacter(id.value);
}

CharacterState FloatingOriginWorld::characterState(CharacterId id) const {
    if (!id || id.value >= m_characters.size() || !m_characters[id.value].alive) return {};
    return m_characters[id.value].state;
}

// ------------------------------------------------------------------ joints --
void FloatingOriginWorld::jointsOf(u32 body, std::vector<u32>& out) const {
    for (u32 j = 0; j < m_joints.size(); ++j)
        if (m_joints[j].alive && (m_joints[j].desc.bodyA.value == body || m_joints[j].desc.bodyB.value == body)) out.push_back(j);
}

JointId FloatingOriginWorld::createJoint(const JointDesc& d) {
    const Body* a = body(d.bodyA);
    const Body* b = d.bodyB ? body(d.bodyB) : nullptr;
    if (!a || (d.bodyB && !b) || d.bodyA == d.bodyB) return {};
    u32 index;
    if (!m_freeJoints.empty()) {
        index = m_freeJoints.back();
        m_freeJoints.pop_back();
    } else {
        index = static_cast<u32>(m_joints.size());
        m_joints.emplace_back();
    }
    Joint& j = m_joints[index];
    j = Joint{};
    j.alive = true;
    j.desc = d;
    // Relative to the bodies as they are now: the joint follows them wherever they go.
    GlobalPosition pa, pb;
    glm::quat qa, qb;
    getTransform(d.bodyA, pa, qa);
    j.localA = glm::conjugate(qa) * (d.anchor - pa).toVec3();
    const glm::vec3 axis = glm::length(d.axis) > 1e-6f ? glm::normalize(d.axis) : glm::vec3(0, 1, 0);
    j.axisA = glm::conjugate(qa) * axis;
    const GlobalPosition attachB = d.type == JointType::Distance ? d.anchorB : d.anchor;
    if (b) {
        getTransform(d.bodyB, pb, qb);
        j.localB = glm::conjugate(qb) * (attachB - pb).toVec3();
    } else {
        j.worldAttach = attachB;
    }
    instantiateJoint(index);
    return {index};
}

void FloatingOriginWorld::instantiateJoint(u32 index) {
    Joint& j = m_joints[index];
    if (!j.alive || j.broken || j.local) return;
    const Body& a = m_bodies[j.desc.bodyA.value];
    const Body* b = j.desc.bodyB ? &m_bodies[j.desc.bodyB.value] : nullptr;
    // The region of A (or of B if A is static), where the other one must be too.
    auto instanceIn = [](const Body& body, u32 region) -> BodyId {
        for (const Instance& i : body.instances)
            if (i.region == region) return i.local;
        return {};
    };
    const Instance* home = a.desc.type != BodyType::Static || !b || b->instances.empty() ? this->home(a) : this->home(*b);
    if (!home) return;
    Region* r = findRegion(home->region);
    if (!r) return;
    const BodyId la = instanceIn(a, r->id);
    const BodyId lb = b ? instanceIn(*b, r->id) : BodyId{};
    if (!la || (b && !lb)) return; // waits until both simulate here

    GlobalPosition pa, pb;
    glm::quat qa, qb;
    getTransform(j.desc.bodyA, pa, qa);
    local::JointDesc l;
    l.type = j.desc.type;
    l.bodyA = la;
    l.bodyB = lb;
    l.anchor = r->toLocal(pa + qa * j.localA);
    if (b) {
        getTransform(j.desc.bodyB, pb, qb);
        l.anchorB = r->toLocal(pb + qb * j.localB);
    } else {
        l.anchorB = r->toLocal(j.worldAttach);
    }
    if (l.type != JointType::Distance) l.anchorB = l.anchor;
    l.axis = qa * j.axisA;
    l.limitsEnabled = j.desc.limitsEnabled;
    l.limitMin = j.desc.limitMin;
    l.limitMax = j.desc.limitMax;
    l.minDistance = j.desc.minDistance;
    l.maxDistance = j.desc.maxDistance;
    l.springFrequency = j.desc.springFrequency;
    l.springDamping = j.desc.springDamping;
    l.motor = j.desc.motor;
    l.userData = index;
    j.local = r->world->createJoint(l);
    j.region = r->id;
}

void FloatingOriginWorld::uninstantiateJoint(u32 index) {
    Joint& j = m_joints[index];
    if (!j.local) return;
    if (Region* r = findRegion(j.region)) r->world->destroyJoint(j.local);
    j.local = {};
}

void FloatingOriginWorld::destroyJoint(JointId id) {
    if (!id || id.value >= m_joints.size() || !m_joints[id.value].alive) return;
    uninstantiateJoint(id.value);
    m_joints[id.value] = Joint{};
    m_freeJoints.push_back(id.value);
}

void FloatingOriginWorld::setJointMotor(JointId id, const JointMotor& motor) {
    if (!id || id.value >= m_joints.size() || !m_joints[id.value].alive) return;
    Joint& j = m_joints[id.value];
    j.desc.motor = motor;
    if (j.local)
        if (Region* r = findRegion(j.region)) r->world->setJointMotor(j.local, motor);
}

f32 FloatingOriginWorld::jointPosition(JointId id) const {
    if (!id || id.value >= m_joints.size() || !m_joints[id.value].alive || !m_joints[id.value].local) return 0.0f;
    const Joint& j = m_joints[id.value];
    const Region* r = findRegion(j.region);
    return r ? r->world->jointPosition(j.local) : 0.0f;
}

bool FloatingOriginWorld::isJointBroken(JointId id) const {
    return id && id.value < m_joints.size() && m_joints[id.value].alive && m_joints[id.value].broken;
}

std::vector<JointBreakEvent> FloatingOriginWorld::drainJointBreaks() { return std::exchange(m_jointBreaks, {}); }

// ---------------------------------------------------------------- contacts --
// Local userData is the global body index (or a world-edge code).
void FloatingOriginWorld::toGlobalIds(u64 localUser, BodyId& id, u64& user) const {
    if (isWorldBoundary(localUser)) {
        id = {};
        user = localUser;
    } else if (isCharacter(localUser)) { // a character's capsule: no body, the character's user data
        const u64 index = localUser & ~kCharacterUserData;
        id = {};
        user = index < m_characters.size() ? m_characters[index].desc.userData : 0;
    } else {
        id = {static_cast<u32>(localUser)};
        user = localUser < m_bodies.size() ? m_bodies[localUser].desc.userData : 0;
    }
}

void FloatingOriginWorld::onLocalContact(u32 region, const local::ContactEvent& c) {
    const PairKey k = std::minmax(c.userDataA, c.userDataB);
    const auto regionKey = std::make_tuple(region, k.first, k.second);
    if (c.type == ContactEventType::Ended) {
        auto it = m_regionPairs.find(regionKey);
        if (it == m_regionPairs.end()) return; // already ended (body destroyed or left the region)
        const ContactEvent ended = it->second;
        m_regionPairs.erase(it);
        if (--m_pairs[k] == 0) {
            m_pairs.erase(k);
            m_events.push_back(ended);
        }
        return;
    }
    const Region* r = findRegion(region);
    if (!r || m_regionPairs.contains(regionKey)) return;
    ContactEvent e;
    e.type = ContactEventType::Began;
    e.sensor = c.sensor;
    toGlobalIds(c.userDataA, e.bodyA, e.userDataA);
    toGlobalIds(c.userDataB, e.bodyB, e.userDataB);
    e.normal = c.normal;
    e.pointA = r->toGlobal(c.pointA);
    e.pointB = r->toGlobal(c.pointB);
    e.surfaceNormalA = c.surfaceNormalA;
    e.surfaceNormalB = c.surfaceNormalB;
    ContactEvent ended;
    ended.type = ContactEventType::Ended;
    ended.sensor = e.sensor;
    ended.bodyA = e.bodyA;
    ended.bodyB = e.bodyB;
    ended.userDataA = e.userDataA;
    ended.userDataB = e.userDataB;
    m_regionPairs.emplace(regionKey, ended);
    if (m_pairs[k]++ == 0) m_events.push_back(e); // first region where the pair touches
}

// Ends the touches of a body (in one region, or everywhere): it was destroyed or left it.
void FloatingOriginWorld::endPairs(u64 index, u32 region, bool anyRegion) {
    for (auto it = m_regionPairs.begin(); it != m_regionPairs.end();) {
        const auto& [r, a, b] = it->first;
        if ((anyRegion || r == region) && (a == index || b == index)) {
            const PairKey k{a, b};
            const ContactEvent ended = it->second;
            it = m_regionPairs.erase(it);
            if (--m_pairs[k] == 0) {
                m_pairs.erase(k);
                m_events.push_back(ended);
            }
        } else {
            ++it;
        }
    }
}

std::vector<ContactEvent> FloatingOriginWorld::drainContactEvents() { return std::exchange(m_events, {}); }

// ----------------------------------------------------------------- queries --
QueryFilter FloatingOriginWorld::toLocal(const QueryFilter& filter, const Region& region) const {
    QueryFilter out;
    out.includeSensors = filter.includeSensors;
    out.layerMask = filter.layerMask;
    if (filter.ignoreCharacter && filter.ignoreCharacter.value < m_characters.size()) {
        const Character& c = m_characters[filter.ignoreCharacter.value];
        if (c.alive && c.local && c.region == region.id) out.ignoreCharacter = c.local;
    }
    if (const Body* b = body(filter.ignoreBody))
        for (const Instance& inst : b->instances)
            if (inst.region == region.id) out.ignoreBody = inst.local;
    return out;
}

QueryHit FloatingOriginWorld::toGlobal(const local::QueryHit& hit, const Region& region,
                                       const GlobalPosition& queryOrigin, bool ray) const {
    QueryHit out;
    toGlobalIds(hit.userData, out.body, out.userData);
    out.point = region.toGlobal(hit.point);
    out.normal = hit.normal;
    out.hitSurfaceNormal = hit.hitSurfaceNormal;
    out.sweptPoint = ray ? queryOrigin : region.toGlobal(hit.sweptPoint); // rays: the origin, exactly
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
    hit = toGlobal(h, *r, origin, true);
    return true;
}

bool FloatingOriginWorld::sweep(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                                const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                                const QueryFilter& filter) const {
    const Region* r = regionAt(position);
    local::SweepHit h;
    if (!r || !r->world->sweep(shape, r->toLocal(position), rotation, direction, maxDistance, h, toLocal(filter, *r)))
        return false;
    hit = toGlobal(h, *r, position, false);
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
        out.push_back(toGlobal(h, *r, position, false));
    return out;
}

std::vector<OverlapHit> FloatingOriginWorld::overlap(const ShapeDesc& shape, const GlobalPosition& position,
                                                     const glm::quat& rotation, const QueryFilter& filter) const {
    std::vector<OverlapHit> out;
    const Region* r = regionAt(position);
    if (!r) return out;
    for (const OverlapHit& h : r->world->overlap(shape, r->toLocal(position), rotation, toLocal(filter, *r))) {
        OverlapHit g;
        toGlobalIds(h.userData, g.body, g.userData);
        out.push_back(g);
    }
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
    hit = toGlobal(lh, *r, start, false);
    return true;
}

} // namespace nova::physics
