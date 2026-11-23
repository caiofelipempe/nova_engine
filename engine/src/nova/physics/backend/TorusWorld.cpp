#include "nova/physics/backend/TorusWorld.h"

#include <algorithm>
#include <cmath>

namespace nova::physics {

namespace {

local::BodyDesc toLocalDesc(const BodyDesc& d, const glm::vec3& position) {
    local::BodyDesc l;
    l.shape = d.shape;
    l.type = d.type;
    l.position = position;
    l.rotation = d.rotation;
    l.linearVelocity = d.linearVelocity;
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
    l.userData = d.userData;
    return l;
}

// The copy of `p` (float meters from the origin) on the lap around the origin: each axis of the
// active world with its own size.
glm::vec3 nearestLap(glm::vec3 p) {
    const WorldSize& w = WorldSize::active();
    for (int i = 0; i < 3; ++i) {
        const f32 lap = static_cast<f32>(w.size(i)), half = lap * 0.5f;
        if (p[i] > half) p[i] -= lap;
        else if (p[i] < -half) p[i] += lap;
    }
    return p;
}

} // namespace

TorusWorld::TorusWorld(Backend backend, const WorldDesc& desc) : m_world(local::World::create(backend, desc)) {}

TorusWorld::~TorusWorld() = default;

// ------------------------------------------------------------------ window --
void TorusWorld::setFocus(const GlobalPosition& focus) {
    // The origin moves by an amount floats represent exactly: the integer origin and the float
    // engine agree to the bit after the move, however often it happens.
    if (m_origin.needsMove(focus)) recenter(m_origin.moveTo(WorldOrigin::snapped(focus)));
}

void TorusWorld::recenter(const glm::vec3& shift) {
    m_world->shiftOrigin(shift);
    rewrap(true);
    // Joints to the world hold a point in the float world's coordinates: place them again.
    for (u32 j = 0; j < m_joints.size(); ++j)
        if (m_joints[j].alive && m_joints[j].local && !m_joints[j].desc.bodyB) {
            m_world->destroyJoint(m_joints[j].local);
            m_joints[j].local = {};
            placeJoint(j);
        }
}

void TorusWorld::rewrap(bool everything) {
    // Bodies and characters more than half a lap from the origin go to their nearest copy: the
    // same place on the torus, velocity and contacts with their neighbours unaffected.
    for (const auto& [value, type] : m_bodies) {
        if (!everything && type == BodyType::Static) continue; // static ones only move on a recenter
        const BodyId id{value};
        glm::vec3 p;
        glm::quat q;
        m_world->getTransform(id, p, q);
        const glm::vec3 wrapped = nearestLap(p);
        if (wrapped != p) m_world->setTransform(id, wrapped, q);
    }
    if (everything)
        for (const auto& [value, s] : m_statics) shapeStatic(BodyId{value}); // the world's size may have changed
    for (u32 value : m_characters) {
        const CharacterId id{value};
        const local::CharacterState s = m_world->characterState(id);
        const glm::vec3 wrapped = nearestLap(s.position);
        if (wrapped != s.position) {
            m_world->setCharacterTransform(id, wrapped, glm::quat(1, 0, 0, 0));
            m_world->setCharacterVelocity(id, s.velocity);
        }
    }
}

// ------------------------------------------------------------------ ghosts --
// Offsets (m) of the ghost copies of a static body: every combination of −1, 0, +1 laps on the
// small axes of the world, except no move at all.
std::vector<glm::vec3> TorusWorld::ghostOffsets() const {
    const WorldSize& w = WorldSize::active();
    std::vector<glm::vec3> out{glm::vec3(0.0f)};
    for (int a = 0; a < 3; ++a) {
        if (w.size(a) > kGhostMaxSize) continue;
        const f32 lap = static_cast<f32>(w.size(a));
        std::vector<glm::vec3> next;
        for (const glm::vec3& o : out)
            for (f32 k : {-1.0f, 0.0f, 1.0f}) {
                glm::vec3 p = o;
                p[a] += k * lap;
                next.push_back(p);
            }
        out = std::move(next);
    }
    std::erase(out, glm::vec3(0.0f));
    return out;
}

namespace {

// `shape` and its copies moved by `offsets` (world meters; the body is turned by `rotation`), as
// one compound: the parts of a compound shape are flattened (compounds do not nest).
ShapeDesc withCopies(const ShapeDesc& shape, const glm::quat& rotation, const std::vector<glm::vec3>& offsets) {
    if (offsets.empty()) return shape;
    auto parts = std::make_shared<std::vector<ShapeDesc>>();
    auto add = [&](const glm::vec3& shift) {
        if (shape.type == ShapeType::Compound && shape.children) {
            for (ShapeDesc c : *shape.children) {
                c.offset = shape.offset + shape.rotation * c.offset + shift;
                c.rotation = shape.rotation * c.rotation;
                parts->push_back(c);
            }
            return;
        }
        ShapeDesc c = shape;
        c.offset += shift;
        parts->push_back(c);
    };
    add(glm::vec3(0.0f));
    const glm::quat toBody = glm::conjugate(rotation);
    for (const glm::vec3& o : offsets) add(toBody * o);
    ShapeDesc out;
    out.type = ShapeType::Compound;
    out.children = std::move(parts);
    return out;
}

} // namespace

// Gives a static body the shape it needs now: its own, plus its copies one lap over on the small
// axes of the world (built again when the world's size or the body's rotation changed).
void TorusWorld::shapeStatic(BodyId id) {
    auto it = m_statics.find(id.value);
    if (it == m_statics.end()) return;
    Static& s = it->second;
    std::vector<glm::vec3> offsets = ghostOffsets();
    glm::vec3 p;
    glm::quat q;
    m_world->getTransform(id, p, q);
    if (offsets == s.offsets && (offsets.empty() || q == s.rotation)) return;
    s.offsets = std::move(offsets);
    s.rotation = q;
    m_world->setShape(id, withCopies(s.shape, q, s.offsets));
}

// ------------------------------------------------------------------ bodies --
BodyId TorusWorld::createBody(const BodyDesc& desc) {
    if (!isValidBody(desc)) return {};
    local::BodyDesc l = toLocalDesc(desc, toLocal(desc.position));
    std::vector<glm::vec3> offsets;
    if (desc.type == BodyType::Static) { // in small worlds: with its copies one lap over
        offsets = ghostOffsets();
        l.shape = withCopies(desc.shape, desc.rotation, offsets);
    }
    const BodyId id = m_world->createBody(l);
    if (!id) return id;
    m_bodies[id.value] = desc.type;
    if (desc.type == BodyType::Static) m_statics[id.value] = {desc.shape, desc.rotation, std::move(offsets)};
    return id;
}

void TorusWorld::destroyBody(BodyId id) {
    if (!id || !m_bodies.contains(id.value)) return;
    for (u32 j = 0; j < m_joints.size(); ++j) // its joints go first
        if (m_joints[j].alive && (m_joints[j].desc.bodyA == id || m_joints[j].desc.bodyB == id)) destroyJoint({j});
    m_statics.erase(id.value);
    m_world->destroyBody(id);
    m_bodies.erase(id.value);
}

u32 TorusWorld::bodyCount() const { return m_world->bodyCount(); }

void TorusWorld::setTransform(BodyId id, const GlobalPosition& position, const glm::quat& rotation) {
    m_world->setTransform(id, toLocal(position), rotation);
    shapeStatic(id); // turned: its copies are one lap over in the world, not in the body
}

void TorusWorld::getTransform(BodyId id, GlobalPosition& position, glm::quat& rotation) const {
    glm::vec3 p;
    m_world->getTransform(id, p, rotation);
    position = toGlobal(p);
}

void TorusWorld::moveKinematic(BodyId id, const GlobalPosition& position, const glm::quat& rotation, Duration dt) {
    // The target the shortest way from where the body is (a kinematic body crossing the seam).
    GlobalPosition now;
    glm::quat q;
    getTransform(id, now, q);
    glm::vec3 p;
    m_world->getTransform(id, p, q);
    m_world->moveKinematic(id, p + (position - now).toVec3(), rotation, dt.secondsF());
}

void TorusWorld::setBodyProperties(BodyId id, const BodyDesc& desc) {
    m_world->setBodyProperties(id, toLocalDesc(desc, glm::vec3(0.0f)));
}

glm::vec3 TorusWorld::linearVelocity(BodyId id) const { return m_world->linearVelocity(id); }
void TorusWorld::setLinearVelocity(BodyId id, const glm::vec3& v) { m_world->setLinearVelocity(id, v); }
glm::vec3 TorusWorld::angularVelocity(BodyId id) const { return m_world->angularVelocity(id); }
void TorusWorld::setAngularVelocity(BodyId id, const glm::vec3& v) { m_world->setAngularVelocity(id, v); }
void TorusWorld::addForce(BodyId id, const glm::vec3& f) { m_world->addForce(id, f); }
void TorusWorld::addImpulse(BodyId id, const glm::vec3& i) { m_world->addImpulse(id, i); }
void TorusWorld::addTorque(BodyId id, const glm::vec3& t) { m_world->addTorque(id, t); }
bool TorusWorld::isActive(BodyId id) const { return m_world->isActive(id); }

void TorusWorld::setGravity(const glm::vec3& g) { m_world->setGravity(g); }
glm::vec3 TorusWorld::gravity() const { return m_world->gravity(); }
void TorusWorld::setCollisionLayers(const CollisionLayers& layers) { m_world->setCollisionLayers(layers); }

void TorusWorld::step(Duration dt) {
    m_world->step(dt.secondsF());
    rewrap(false);
    // Breakable joints: what they held during the step.
    for (u32 j = 0; j < m_joints.size(); ++j) {
        Joint& joint = m_joints[j];
        if (!joint.alive || !joint.local || (joint.desc.breakForce <= 0.0f && joint.desc.breakTorque <= 0.0f)) continue;
        const local::JointLoad load = m_world->jointLoad(joint.local);
        const bool force = joint.desc.breakForce > 0.0f && load.force > joint.desc.breakForce;
        const bool torque = joint.desc.breakTorque > 0.0f && load.torque > joint.desc.breakTorque;
        if (!force && !torque) continue;
        m_world->destroyJoint(joint.local);
        joint.local = {};
        joint.broken = true;
        m_jointBreaks.push_back({JointId{j}, joint.desc.userData});
    }
}

std::vector<ContactEvent> TorusWorld::drainContactEvents() {
    std::vector<ContactEvent> out;
    for (const local::ContactEvent& c : m_world->drainContactEvents()) {
        ContactEvent e;
        e.type = c.type;
        e.sensor = c.sensor;
        e.bodyA = c.bodyA;
        e.bodyB = c.bodyB;
        e.userDataA = c.userDataA;
        e.userDataB = c.userDataB;
        e.normal = c.normal;
        e.pointA = toGlobal(c.pointA);
        e.pointB = toGlobal(c.pointB);
        e.surfaceNormalA = c.surfaceNormalA;
        e.surfaceNormalB = c.surfaceNormalB;
        out.push_back(e);
    }
    return out;
}

// ------------------------------------------------------------------ joints --
JointId TorusWorld::createJoint(const JointDesc& d) {
    if (!d.bodyA || !m_bodies.contains(d.bodyA.value) || (d.bodyB && !m_bodies.contains(d.bodyB.value)) ||
        d.bodyA == d.bodyB)
        return {};
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
    // Relative to the bodies as they are now: the joint can be placed again wherever they go.
    GlobalPosition pa, pb;
    glm::quat qa, qb;
    getTransform(d.bodyA, pa, qa);
    j.localA = glm::conjugate(qa) * (d.anchor - pa).toVec3();
    const glm::vec3 axis = glm::length(d.axis) > 1e-6f ? glm::normalize(d.axis) : glm::vec3(0, 1, 0);
    j.axisA = glm::conjugate(qa) * axis;
    const GlobalPosition attachB = d.type == JointType::Distance ? d.anchorB : d.anchor;
    if (d.bodyB) {
        getTransform(d.bodyB, pb, qb);
        j.localB = glm::conjugate(qb) * (attachB - pb).toVec3();
    } else {
        j.worldAttach = attachB;
    }
    placeJoint(index);
    return {index};
}

void TorusWorld::placeJoint(u32 index) {
    Joint& j = m_joints[index];
    GlobalPosition pa, pb;
    glm::quat qa, qb;
    getTransform(j.desc.bodyA, pa, qa);
    local::JointDesc l;
    l.type = j.desc.type;
    l.bodyA = j.desc.bodyA;
    l.bodyB = j.desc.bodyB;
    l.anchor = toLocal(pa + qa * j.localA);
    if (j.desc.bodyB) {
        getTransform(j.desc.bodyB, pb, qb);
        l.anchorB = toLocal(pb + qb * j.localB);
    } else {
        l.anchorB = toLocal(j.worldAttach);
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
    l.userData = j.desc.userData;
    j.local = m_world->createJoint(l);
}

void TorusWorld::destroyJoint(JointId id) {
    if (!id || id.value >= m_joints.size() || !m_joints[id.value].alive) return;
    if (m_joints[id.value].local) m_world->destroyJoint(m_joints[id.value].local);
    m_joints[id.value] = Joint{};
    m_freeJoints.push_back(id.value);
}

void TorusWorld::setJointMotor(JointId id, const JointMotor& motor) {
    if (!id || id.value >= m_joints.size() || !m_joints[id.value].alive) return;
    Joint& j = m_joints[id.value];
    j.desc.motor = motor;
    if (j.local) m_world->setJointMotor(j.local, motor);
}

f32 TorusWorld::jointPosition(JointId id) const {
    if (!id || id.value >= m_joints.size() || !m_joints[id.value].alive || !m_joints[id.value].local) return 0.0f;
    return m_world->jointPosition(m_joints[id.value].local);
}

bool TorusWorld::isJointBroken(JointId id) const {
    return id && id.value < m_joints.size() && m_joints[id.value].alive && m_joints[id.value].broken;
}

std::vector<JointBreakEvent> TorusWorld::drainJointBreaks() { return std::exchange(m_jointBreaks, {}); }

// -------------------------------------------------------------- characters --
CharacterId TorusWorld::createCharacter(const CharacterDesc& d) {
    local::CharacterDesc l;
    l.radius = d.radius;
    l.halfHeight = d.halfHeight;
    l.position = toLocal(d.position);
    l.rotation = d.rotation;
    l.maxSlopeAngle = d.maxSlopeAngle;
    l.stepHeight = d.stepHeight;
    l.stickToFloor = d.stickToFloor;
    l.mass = d.mass;
    l.maxPushForce = d.maxPushForce;
    l.gravityFactor = d.gravityFactor;
    l.layer = d.layer;
    l.userData = d.userData;
    const CharacterId id = m_world->createCharacter(l);
    if (id) m_characters.insert(id.value);
    return id;
}

void TorusWorld::destroyCharacter(CharacterId id) {
    if (!id || !m_characters.contains(id.value)) return;
    m_world->destroyCharacter(id);
    m_characters.erase(id.value);
}

void TorusWorld::setCharacterVelocity(CharacterId id, const glm::vec3& v) { m_world->setCharacterVelocity(id, v); }

void TorusWorld::setCharacterTransform(CharacterId id, const GlobalPosition& position, const glm::quat& rotation) {
    m_world->setCharacterTransform(id, toLocal(position), rotation);
}

CharacterState TorusWorld::characterState(CharacterId id) const {
    const local::CharacterState l = m_world->characterState(id);
    CharacterState s;
    s.position = toGlobal(l.position);
    s.velocity = l.velocity;
    s.ground = l.ground;
    s.groundNormal = l.groundNormal;
    s.groundVelocity = l.groundVelocity;
    s.groundBody = l.groundBody;
    s.groundUserData = l.groundUserData;
    return s;
}

// ----------------------------------------------------------------- queries --
QueryHit TorusWorld::toGlobal(const local::QueryHit& hit, const GlobalPosition& queryOrigin, bool ray) const {
    QueryHit out;
    out.body = hit.body;
    out.userData = hit.userData;
    out.point = toGlobal(hit.point);
    out.normal = hit.normal;
    out.hitSurfaceNormal = hit.hitSurfaceNormal;
    out.sweptPoint = ray ? queryOrigin : toGlobal(hit.sweptPoint); // rays: the origin, exactly
    out.sweptSurfaceNormal = hit.sweptSurfaceNormal;
    out.distance = hit.distance;
    out.fraction = hit.fraction;
    return out;
}

bool TorusWorld::raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance, RaycastHit& hit,
                         const QueryFilter& filter) const {
    local::RaycastHit h;
    if (!m_world->raycast(toLocal(origin), direction, maxDistance, h, filter)) return false;
    hit = toGlobal(h, origin, true);
    return true;
}

bool TorusWorld::sweep(const ShapeDesc& shape, const GlobalPosition& position, const glm::quat& rotation,
                       const glm::vec3& direction, f32 maxDistance, SweepHit& hit, const QueryFilter& filter) const {
    local::SweepHit h;
    if (!m_world->sweep(shape, toLocal(position), rotation, direction, maxDistance, h, filter)) return false;
    hit = toGlobal(h, position, false);
    return true;
}

std::vector<SweepHit> TorusWorld::sweepAll(const ShapeDesc& shape, const GlobalPosition& position,
                                           const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                           const QueryFilter& filter) const {
    std::vector<SweepHit> out;
    for (const local::SweepHit& h : m_world->sweepAll(shape, toLocal(position), rotation, direction, maxDistance, filter))
        out.push_back(toGlobal(h, position, false));
    return out;
}

bool TorusWorld::sweepBody(BodyId id, const glm::vec3& direction, f32 maxDistance, SweepHit& hit,
                           const QueryFilter& filter) const {
    local::SweepHit lh;
    if (!m_world->sweepBody(id, direction, maxDistance, lh, filter)) return false;
    GlobalPosition start;
    glm::quat q;
    getTransform(id, start, q);
    hit = toGlobal(lh, start, false);
    return true;
}

std::vector<OverlapHit> TorusWorld::overlap(const ShapeDesc& shape, const GlobalPosition& position,
                                            const glm::quat& rotation, const QueryFilter& filter) const {
    return m_world->overlap(shape, toLocal(position), rotation, filter);
}

} // namespace nova::physics
