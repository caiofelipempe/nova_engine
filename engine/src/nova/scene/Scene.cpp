#include "nova/scene/Scene.h"

#include <algorithm>
#include <random>

namespace nova {

// ------------------------------------------------------------------ Entity --
bool Entity::valid() const { return m_scene && m_scene->registry().valid(m_handle); }
entt::registry& Entity::registry() const { return m_scene->registry(); }

// ------------------------------------------------------------------- Scene --
Scene::Scene(std::string name) : m_name(std::move(name)), m_worldSpace(WorldSpace().desc()) { connectSignals(); }

void Scene::connectSignals() {
    // Removing the component (or destroying the entity) removes the body from the world.
    m_registry.on_destroy<RigidBodyComponent>().connect<&Scene::onRigidBodyDestroyed>(*this);
}

Scene::~Scene() {
    clear();
    stopSimulation();
}

namespace {
u64 newUuid() {
    static std::mt19937_64 rng{std::random_device{}() ^ (u64(std::random_device{}()) << 32)};
    u64 id = 0;
    while (id == 0) id = rng(); // 0 means "no id"
    return id;
}
} // namespace

Entity Scene::createEntity(const std::string& name) {
    Entity e{m_registry.create(), this};
    e.add<IdComponent>(newUuid());
    e.add<NameComponent>(name);
    e.add<TransformComponent>();
    return e;
}

Entity Scene::findEntityByUuid(u64 uuid) {
    if (uuid == 0) return {};
    for (auto [e, id] : m_registry.view<IdComponent>().each())
        if (id.uuid == uuid) return {e, this};
    return {};
}

void Scene::destroyEntity(Entity entity) {
    if (!entity.valid()) return;
    if (auto* sc = entity.tryGet<ScriptComponent>())
        for (auto& s : sc->scripts)
            if (s->m_created) s->onDestroy();
    m_registry.destroy(entity.handle());
}

Entity Scene::findEntity(const std::string& name) {
    for (auto [e, n] : m_registry.view<NameComponent>().each())
        if (n.name == name) return {e, this};
    return {};
}

void Scene::clear() {
    for (auto [e, sc] : m_registry.view<ScriptComponent>().each())
        for (auto& s : sc.scripts)
            if (s->m_created) s->onDestroy();
    m_registry.clear();
}

void Scene::resetStorage() {
    clear();
    m_registry = entt::registry{};
    connectSignals();
}

void Scene::setWorldSpace(const WorldSpaceDesc& desc) {
    // Carry positions over in meters: the integer steps mean something else under the new unit.
    std::vector<std::pair<entt::entity, glm::dvec3>> meters;
    for (auto [e, t] : m_registry.view<TransformComponent>().each()) meters.emplace_back(e, t.position.toMeters());
    WorldSpace::setActive(WorldSpace(desc));
    m_worldSpace = WorldSpace::active().desc(); // as resolved (clamped sizes)
    for (auto& [e, m] : meters) m_registry.get<TransformComponent>(e).position = GlobalPosition::fromMeters(m);
}

void Scene::activateWorldSpace() const {
    const WorldSpace& active = WorldSpace::active();
    const WorldSpaceDesc& d = active.desc();
    bool same = d.unit == m_worldSpace.unit;
    for (int i = 0; same && i < 3; ++i)
        same = d.axes[i].size == m_worldSpace.axes[i].size && d.axes[i].boundary == m_worldSpace.axes[i].boundary;
    if (!same) WorldSpace::setActive(WorldSpace(m_worldSpace));
}

void Scene::update(Duration frame) {
    activateWorldSpace();
    m_time += frame;
    const f32 dt = frame.secondsF(); // scripts and built-in systems work in float seconds

    // Native scripts. Iterate over a snapshot: scripts may create/destroy entities.
    std::vector<entt::entity> scripted;
    for (auto e : m_registry.view<ScriptComponent>()) scripted.push_back(e);
    for (entt::entity e : scripted) {
        if (!m_registry.valid(e)) continue;
        auto* sc = m_registry.try_get<ScriptComponent>(e);
        if (!sc) continue;
        for (size_t i = 0; i < sc->scripts.size(); ++i) {
            Script& s = *sc->scripts[i];
            if (!s.m_created) {
                s.m_created = true;
                s.onCreate();
            }
            s.onUpdate(dt);
            if (!m_registry.valid(e)) break;
        }
    }

    // Built-in systems.
    for (auto [e, t, r] : m_registry.view<TransformComponent, RotatorComponent>().each()) {
        t.rotate(r.speed * dt, r.axis);
    }

    if (m_simulating) stepPhysics(frame);
}

// ----------------------------------------------------------------- physics --
namespace {
u64 toUserData(entt::entity e) { return static_cast<u64>(entt::to_integral(e)); }
// Barrier walls are bodies without an entity: the top bit marks them, the low bits say which edge.
constexpr u64 kWallUserData = 1ull << 63;
bool isWall(u64 v) { return (v & kWallUserData) != 0; }
entt::entity fromUserData(u64 v) { return static_cast<entt::entity>(static_cast<entt::id_type>(v)); }

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
        for (size_t k = 0; k < out.back().size(); ++k) // flood fill from each member
            for (size_t j = i + 1; j < points.size(); ++j)
                if (group[j] < 0 && isWithin(out.back()[k], points[j], distance)) {
                    group[j] = group[i];
                    out.back().push_back(points[j]);
                }
    }
    return out;
}
} // namespace

Scene::PhysicsRegion* Scene::findRegion(u32 id) {
    for (PhysicsRegion& r : m_regions)
        if (r.id == id) return &r;
    return nullptr;
}

const GlobalPosition& Scene::physicsOrigin(u32 region) const {
    static const GlobalPosition none;
    return region < m_regions.size() ? m_regions[region].origin : none;
}

u32 Scene::physicsBodyCount() const {
    u32 n = 0;
    for (const PhysicsRegion& r : m_regions) n += r.world->bodyCount();
    return n;
}

bool Scene::isPhysicsDormant(Entity e) const {
    if (!m_simulating || !e.valid() || !e.has<RigidBodyComponent>()) return false;
    auto it = m_bindings.find(e.handle());
    return it == m_bindings.end() || it->second.instances.empty();
}

void Scene::startSimulation(physics::Backend backend) {
    if (m_simulating) return;
    activateWorldSpace();
    m_simulating = true;
    m_backend = backend;
    m_physicsAccumulator = Duration();
    updatePhysicsRegions(); // creates the regions and every body
}

void Scene::stopSimulation() {
    if (!m_simulating) return;
    for (auto [e, rb] : m_registry.view<RigidBodyComponent>().each()) rb.body = {};
    m_bindings.clear();
    m_regions.clear(); // destroys the worlds and their bodies
    m_simulating = false;
}

std::vector<GlobalPosition> Scene::currentFoci() {
    if (m_hasPhysicsFocus) return m_physicsFoci;
    if (Entity cam = primaryCamera()) return {cam.transform().position};
    return {};
}

Scene::PhysicsRegion& Scene::createRegion(const GlobalPosition& origin) {
    PhysicsRegion r;
    r.id = m_nextRegionId++;
    r.world = physics::PhysicsWorld::create(m_backend);
    r.origin = origin;
    m_regions.push_back(std::move(r));
    rebuildBarrierWalls(m_regions.back());
    return m_regions.back();
}

void Scene::destroyRegion(size_t index) {
    const u32 id = m_regions[index].id;
    // Bodies leave the region; dynamic ones keep their velocity for wherever they go next.
    for (auto& [e, binding] : m_bindings)
        for (size_t i = binding.instances.size(); i-- > 0;)
            if (binding.instances[i].region == id) removeInstance(e, binding, i);
    m_regions.erase(m_regions.begin() + static_cast<std::ptrdiff_t>(index));
}

void Scene::rebaseRegion(PhysicsRegion& r, const GlobalPosition& target) {
    if (isWithin(target, r.origin, physicsRebaseDistance)) return;
    // Shift by an amount floats represent exactly, so the integer origin and the physics engine
    // agree to the bit after the move (no drift however often it happens).
    const glm::vec3 shift = (target - r.origin).toVec3();
    r.origin += GlobalOffset::fromMeters(shift);
    r.world->shiftOrigin(shift);
    rebuildBarrierWalls(r);
}

void Scene::rebuildBarrierWalls(PhysicsRegion& r) {
    for (physics::BodyId wall : r.walls) r.world->destroyBody(wall);
    r.walls.clear();
    const WorldSpace& world = WorldSpace::active();
    const f32 thickness = 1.0f, span = static_cast<f32>(barrierWallRange);
    for (int axis = 0; axis < 3; ++axis) {
        const WorldSpace::Axis& a = world.axis(axis);
        if (a.boundary != WorldBoundary::Barrier) continue;
        for (int side : {-1, 1}) {
            // Distance along the axis from the region's origin to the edge.
            const i64 edgeRaw = side < 0 ? a.min : a.max;
            const f64 toEdge = static_cast<f64>(world.difference(axis, edgeRaw, r.origin.raw(axis))) * world.unit();
            if (std::abs(toEdge) > barrierWallRange) continue;
            physics::BodyDesc desc;
            desc.type = physics::BodyType::Static;
            desc.shape.type = physics::ShapeType::Box;
            desc.shape.halfExtents = glm::vec3(span);
            desc.shape.halfExtents[axis] = thickness;
            desc.position = glm::vec3(0.0f);
            desc.position[axis] = static_cast<f32>(toEdge) + side * thickness; // just outside the edge
            desc.friction = 0.5f;
            desc.restitution = 0.0f; // the edge adds no bounce of its own (the body's restitution still applies)
            desc.userData = kWallUserData | static_cast<u64>(axis * 2 + (side > 0 ? 1 : 0));
            if (physics::BodyId wall = r.world->createBody(desc)) r.walls.push_back(wall);
        }
    }
}

void Scene::updatePhysicsRegions() {
    const std::vector<GlobalPosition> foci = currentFoci();
    m_regionsBounded = physicsRegionRadius > 0.0 && !foci.empty();
    if (!m_regionsBounded) {
        // Unlimited: a single region holds every body; its origin follows the foci (if any).
        if (m_regions.empty()) createRegion(foci.empty() ? GlobalPosition() : centroid(foci));
        while (m_regions.size() > 1) destroyRegion(m_regions.size() - 1);
        m_regions.front().foci = foci;
        if (!foci.empty()) rebaseRegion(m_regions.front(), centroid(foci));
    } else {
        // Foci closer than two radii share a region (their simulation spheres overlap).
        const auto clusters = cluster(foci, 2.0 * physicsRegionRadius);
        std::vector<u32> claimed;
        std::vector<u32> regionOf(clusters.size(), 0);
        for (size_t c = 0; c < clusters.size(); ++c) {
            const GlobalPosition center = centroid(clusters[c]);
            // Keep the nearest unclaimed region that is still around these foci; else a new one.
            PhysicsRegion* best = nullptr;
            UInt128 bestDistance;
            for (PhysicsRegion& r : m_regions) {
                if (std::find(claimed.begin(), claimed.end(), r.id) != claimed.end()) continue;
                if (!isWithin(r.origin, center, 4.0 * physicsRegionRadius)) continue;
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
            PhysicsRegion& r = *findRegion(regionOf[c]);
            r.foci = clusters[c];
            rebaseRegion(r, centroid(clusters[c]));
        }
    }
    // Every rigid body to the region(s) it belongs to, in registry order (deterministic).
    for (auto e : m_registry.view<RigidBodyComponent>()) assignBody(e);
}

Scene::PhysicsRegion* Scene::regionAt(const GlobalPosition& p) {
    if (m_regions.empty()) return nullptr;
    if (!m_regionsBounded) return &m_regions.front();
    PhysicsRegion* best = nullptr;
    UInt128 bestDistance;
    for (PhysicsRegion& r : m_regions)
        for (const GlobalPosition& f : r.foci) {
            const UInt128 d = distanceSquared(f, p);
            if (!best || d < bestDistance) best = &r, bestDistance = d;
        }
    if (!best) return nullptr;
    const f64 limit = physicsRegionRadius / WorldSpace::active().unit();
    return bestDistance.toDouble() <= limit * limit ? best : nullptr;
}

namespace {
// Radius of a sphere around the transform that contains the entity's collider (meters).
f64 colliderReach(const entt::registry& reg, entt::entity e, const TransformComponent& t) {
    const glm::vec3 s = glm::abs(t.scale);
    const f32 maxScale = std::max({s.x, s.y, s.z});
    if (auto* box = reg.try_get<BoxColliderComponent>(e))
        return glm::length(box->halfExtents * s) + glm::length(box->offset * s);
    if (auto* sphere = reg.try_get<SphereColliderComponent>(e))
        return sphere->radius * maxScale + glm::length(sphere->offset * s);
    if (auto* cap = reg.try_get<CapsuleColliderComponent>(e))
        return (cap->radius + cap->halfHeight) * maxScale + glm::length(cap->offset * s);
    return glm::length(0.5f * s);
}
} // namespace

void Scene::assignBody(entt::entity e) {
    auto& rb = m_registry.get<RigidBodyComponent>(e);
    const auto& t = m_registry.get<TransformComponent>(e);
    PhysicsBinding& binding = m_bindings[e];

    // Which regions should hold this body?
    std::vector<u32> wanted;
    if (!m_regionsBounded) {
        if (!m_regions.empty()) wanted.push_back(m_regions.front().id);
    } else {
        const f64 reach = physicsRegionRadius + colliderReach(m_registry, e, t);
        if (rb.type == physics::BodyType::Static) { // every region it reaches
            for (const PhysicsRegion& r : m_regions)
                for (const GlobalPosition& f : r.foci)
                    if (isWithin(f, t.position, reach)) {
                        wanted.push_back(r.id);
                        break;
                    }
        } else { // the region of the nearest focus, if within reach; else dormant
            const PhysicsRegion* best = nullptr;
            UInt128 bestDistance;
            for (const PhysicsRegion& r : m_regions)
                for (const GlobalPosition& f : r.foci) {
                    const UInt128 d = distanceSquared(f, t.position);
                    if (!best || d < bestDistance) best = &r, bestDistance = d;
                }
            const f64 limit = reach / WorldSpace::active().unit();
            if (best && bestDistance.toDouble() <= limit * limit) wanted.push_back(best->id);
        }
    }

    for (size_t i = binding.instances.size(); i-- > 0;)
        if (std::find(wanted.begin(), wanted.end(), binding.instances[i].region) == wanted.end())
            removeInstance(e, binding, i);
    for (u32 id : wanted) {
        const bool present = std::any_of(binding.instances.begin(), binding.instances.end(),
                                         [id](const BodyInstance& b) { return b.region == id; });
        if (!present)
            if (PhysicsRegion* r = findRegion(id)) createBodyIn(e, *r, binding);
    }
    rb.body = binding.instances.empty() ? physics::BodyId{} : binding.instances.front().body;
}

void Scene::removeInstance(entt::entity e, PhysicsBinding& binding, size_t index) {
    const BodyInstance inst = binding.instances[index];
    if (PhysicsRegion* r = findRegion(inst.region)) {
        const auto* rb = m_registry.try_get<RigidBodyComponent>(e);
        if (rb && rb->type == physics::BodyType::Dynamic) { // keep its motion for the next region
            binding.savedLinear = r->world->linearVelocity(inst.body);
            binding.savedAngular = r->world->angularVelocity(inst.body);
        }
        r->world->destroyBody(inst.body);
    }
    binding.instances.erase(binding.instances.begin() + static_cast<std::ptrdiff_t>(index));
}

void Scene::createBodyIn(entt::entity e, PhysicsRegion& region, PhysicsBinding& binding) {
    auto& rb = m_registry.get<RigidBodyComponent>(e);
    auto& t = m_registry.get<TransformComponent>(e);
    const glm::vec3 s = glm::abs(t.scale);

    physics::BodyDesc desc;
    if (auto* box = m_registry.try_get<BoxColliderComponent>(e)) {
        desc.shape.type = physics::ShapeType::Box;
        desc.shape.halfExtents = box->halfExtents * s;
        desc.shape.offset = box->offset * s;
    } else if (auto* sphere = m_registry.try_get<SphereColliderComponent>(e)) {
        desc.shape.type = physics::ShapeType::Sphere;
        desc.shape.radius = sphere->radius * std::max({s.x, s.y, s.z});
        desc.shape.offset = sphere->offset * s;
    } else if (auto* capsule = m_registry.try_get<CapsuleColliderComponent>(e)) {
        desc.shape.type = physics::ShapeType::Capsule;
        desc.shape.radius = capsule->radius * std::max(s.x, s.z);
        desc.shape.halfHeight = capsule->halfHeight * s.y;
        desc.shape.offset = capsule->offset * s;
    } else {
        desc.shape.type = physics::ShapeType::Box;
        desc.shape.halfExtents = 0.5f * s;
    }

    desc.type = rb.type;
    desc.position = region.toPhysics(t.position);
    desc.rotation = t.orientation();
    desc.mass = rb.mass;
    desc.friction = rb.friction;
    desc.restitution = rb.restitution;
    desc.linearDamping = rb.linearDamping;
    desc.angularDamping = rb.angularDamping;
    desc.gravityFactor = rb.gravityFactor;
    desc.isSensor = rb.isSensor;
    desc.userData = toUserData(e);
    if (rb.type == physics::BodyType::Dynamic) desc.linearVelocity = binding.savedLinear;
    const physics::BodyId body = region.world->createBody(desc);
    if (!body) return;
    if (rb.type == physics::BodyType::Dynamic && binding.savedAngular != glm::vec3(0.0f))
        region.world->setAngularVelocity(body, binding.savedAngular);
    binding.savedLinear = binding.savedAngular = glm::vec3(0.0f);
    binding.instances.push_back({region.id, body});
}

void Scene::onRigidBodyDestroyed(entt::registry& registry, entt::entity e) {
    registry.get<RigidBodyComponent>(e).body = {};
    auto it = m_bindings.find(e);
    if (it == m_bindings.end()) return;
    for (const BodyInstance& inst : it->second.instances)
        if (PhysicsRegion* r = findRegion(inst.region)) r->world->destroyBody(inst.body);
    m_bindings.erase(it);
}

void Scene::stepPhysics(Duration dt) {
    updatePhysicsRegions(); // regions follow the foci; bodies join, leave and change regions

    constexpr int kMaxSteps = 4; // avoid the spiral of death after a hitch
    m_physicsAccumulator += dt;
    const f32 step = fixedTimeStep.secondsF();
    int steps = 0;
    while (fixedTimeStep > Duration() && m_physicsAccumulator >= fixedTimeStep && steps < kMaxSteps) {
        for (auto [e, rb, t] : m_registry.view<RigidBodyComponent, TransformComponent>().each()) {
            if (rb.type != physics::BodyType::Kinematic) continue;
            if (BodyRef b = bodyOf({e, this})) b.region->world->moveKinematic(b.body, b.region->toPhysics(t.position), t.orientation(), step);
        }
        for (PhysicsRegion& r : m_regions) r.world->step(step);
        m_physicsAccumulator -= fixedTimeStep;
        ++steps;
    }
    if (steps == kMaxSteps) m_physicsAccumulator = Duration();
    if (steps == 0) return;

    // Simulation -> ECS
    for (auto [e, rb, t] : m_registry.view<RigidBodyComponent, TransformComponent>().each()) {
        if (rb.type != physics::BodyType::Dynamic) continue;
        BodyRef b = bodyOf({e, this});
        if (!b || !b.region->world->isActive(b.body)) continue;
        glm::quat q;
        glm::vec3 p;
        b.region->world->getTransform(b.body, p, q);
        t.position = b.region->fromPhysics(p);
        t.rotation = q; // quaternion in, quaternion out: no Euler round trip
    }

    // Collision callbacks for scripts on both entities, each seen from its own side.
    for (PhysicsRegion& r : m_regions)
        for (const physics::ContactEvent& c : r.world->drainContactEvents()) {
            if (isWall(c.userDataA) || isWall(c.userDataB)) { // a body touched a barrier edge of the world
                const bool wallIsA = isWall(c.userDataA);
                const u64 wall = wallIsA ? c.userDataA : c.userDataB;
                Entity self{fromUserData(wallIsA ? c.userDataB : c.userDataA), this};
                if (!self.valid() || isWall(wallIsA ? c.userDataB : c.userDataA)) continue;
                WorldBoundaryHit hit;
                hit.axis = static_cast<int>((wall & 0xFF) / 2);
                hit.side = (wall & 1) ? 1 : -1;
                hit.point = r.fromPhysics(wallIsA ? c.pointB : c.pointA);
                hit.normal = glm::vec3(0.0f);
                hit.normal[hit.axis] = static_cast<f32>(-hit.side);
                if (auto* sc = self.tryGet<ScriptComponent>())
                    for (auto& s : sc->scripts)
                        if (s->m_created) s->onWorldBoundary(hit);
                continue;
            }
            Entity a{fromUserData(c.userDataA), this};
            Entity b{fromUserData(c.userDataB), this};
            auto notify = [&](Entity self, const Collision& info) {
                if (!self.valid() || !info.other.valid()) return;
                if (auto* sc = self.tryGet<ScriptComponent>())
                    for (auto& s : sc->scripts)
                        if (s->m_created) s->onCollision(info);
            };
            // c.normal points from A to B: it pushes B out of A, so A is pushed along -normal.
            const GlobalPosition pa = r.fromPhysics(c.pointA), pb = r.fromPhysics(c.pointB);
            notify(a, Collision{b, -c.normal, pa, pb, c.surfaceNormalA, c.surfaceNormalB});
            notify(b, Collision{a, c.normal, pb, pa, c.surfaceNormalB, c.surfaceNormalA});
        }
}

Scene::BodyRef Scene::bodyOf(Entity e) {
    if (!m_simulating || !e.valid() || !e.has<RigidBodyComponent>()) return {};
    auto it = m_bindings.find(e.handle());
    if (it == m_bindings.end()) { // added since the last update
        assignBody(e.handle());
        it = m_bindings.find(e.handle());
    }
    if (it == m_bindings.end() || it->second.instances.empty()) return {};
    const BodyInstance& inst = it->second.instances.front();
    return {findRegion(inst.region), inst.body};
}

void Scene::addForce(Entity e, const glm::vec3& force) {
    if (BodyRef b = bodyOf(e)) b.region->world->addForce(b.body, force);
}
void Scene::addImpulse(Entity e, const glm::vec3& impulse) {
    if (BodyRef b = bodyOf(e)) b.region->world->addImpulse(b.body, impulse);
    else if (isPhysicsDormant(e)) m_bindings[e.handle()].savedLinear += impulse / std::max(e.get<RigidBodyComponent>().mass, 0.001f);
}
void Scene::setLinearVelocity(Entity e, const glm::vec3& velocity) {
    if (BodyRef b = bodyOf(e)) b.region->world->setLinearVelocity(b.body, velocity);
    else if (isPhysicsDormant(e)) m_bindings[e.handle()].savedLinear = velocity; // applied when it wakes
}
glm::vec3 Scene::linearVelocity(Entity e) {
    if (BodyRef b = bodyOf(e)) return b.region->world->linearVelocity(b.body);
    if (isPhysicsDormant(e)) return m_bindings[e.handle()].savedLinear;
    return glm::vec3(0.0f);
}
void Scene::syncTransformToPhysics(Entity e) {
    if (!m_simulating || !e.valid() || !e.has<RigidBodyComponent>()) return;
    bodyOf(e); // make sure it is bound
    auto it = m_bindings.find(e.handle());
    if (it == m_bindings.end()) return;
    auto& t = e.transform();
    for (const BodyInstance& inst : it->second.instances)
        if (PhysicsRegion* r = findRegion(inst.region)) {
            r->world->setTransform(inst.body, r->toPhysics(t.position), t.orientation());
            r->world->setLinearVelocity(inst.body, glm::vec3(0.0f));
            r->world->setAngularVelocity(inst.body, glm::vec3(0.0f));
        }
    it->second.savedLinear = it->second.savedAngular = glm::vec3(0.0f);
    assignBody(e.handle()); // it may now belong to another region
}

u64 Scene::stateHash() {
    std::vector<std::pair<u64, entt::entity>> ordered;
    for (auto [e, id] : m_registry.view<IdComponent>().each()) ordered.emplace_back(id.uuid, e);
    std::sort(ordered.begin(), ordered.end());
    u64 h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
    };
    for (auto& [uuid, e] : ordered) {
        mix(&uuid, sizeof(uuid));
        if (const auto* t = m_registry.try_get<TransformComponent>(e)) {
            const glm::i64vec3 raw = t->position.raw();
            mix(&raw, sizeof(raw));
            mix(&t->rotation, sizeof(t->rotation));
            mix(&t->scale, sizeof(t->scale));
        }
        if (m_registry.all_of<RigidBodyComponent>(e)) {
            const glm::vec3 v = linearVelocity({e, this});
            mix(&v, sizeof(v));
        }
    }
    return h;
}

// ----------------------------------------------------------------- queries --
physics::QueryFilter Scene::filterIgnoring(Entity e, const PhysicsRegion& region) {
    physics::QueryFilter f;
    if (!e.valid()) return f;
    auto it = m_bindings.find(e.handle());
    if (it != m_bindings.end())
        for (const BodyInstance& inst : it->second.instances)
            if (inst.region == region.id) f.ignoreBody = inst.body;
    return f;
}

SceneHit Scene::toSceneHit(const physics::QueryHit& hit, const GlobalPosition& queryOrigin, const PhysicsRegion& region) {
    SceneHit out;
    out.entity = isWall(hit.userData) ? Entity{} : Entity{fromUserData(hit.userData), this};
    out.point = region.fromPhysics(hit.point);
    out.normal = hit.normal;
    out.hitSurfaceNormal = hit.hitSurfaceNormal;
    out.sweptPoint = hit.sweptPoint == glm::vec3(0.0f) ? queryOrigin : region.fromPhysics(hit.sweptPoint);
    out.sweptSurfaceNormal = hit.sweptSurfaceNormal;
    out.distance = hit.distance;
    return out;
}

std::optional<SceneHit> Scene::raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance,
                                       Entity ignore) {
    PhysicsRegion* r = regionAt(origin);
    if (!r) return std::nullopt;
    physics::RaycastHit hit;
    if (!r->world->raycast(r->toPhysics(origin), direction, maxDistance, hit, filterIgnoring(ignore, *r)))
        return std::nullopt;
    return toSceneHit(hit, origin, *r);
}

std::optional<SceneHit> Scene::sweep(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                     const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                     Entity ignore) {
    PhysicsRegion* r = regionAt(position);
    if (!r) return std::nullopt;
    physics::SweepHit hit;
    if (!r->world->sweep(shape, r->toPhysics(position), rotation, direction, maxDistance, hit, filterIgnoring(ignore, *r)))
        return std::nullopt;
    return toSceneHit(hit, position, *r);
}

std::vector<SceneHit> Scene::sweepAll(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                      const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                      Entity ignore) {
    std::vector<SceneHit> out;
    PhysicsRegion* r = regionAt(position);
    if (!r) return out;
    for (const auto& h :
         r->world->sweepAll(shape, r->toPhysics(position), rotation, direction, maxDistance, filterIgnoring(ignore, *r)))
        out.push_back(toSceneHit(h, position, *r));
    return out;
}

std::optional<SceneHit> Scene::sweepSphere(const GlobalPosition& origin, f32 radius, const glm::vec3& direction,
                                           f32 maxDistance, Entity ignore) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Sphere;
    s.radius = radius;
    return sweep(s, origin, glm::quat(1, 0, 0, 0), direction, maxDistance, ignore);
}

std::optional<SceneHit> Scene::sweepBox(const GlobalPosition& center, const glm::vec3& halfExtents,
                                        const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                        Entity ignore) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Box;
    s.halfExtents = halfExtents;
    return sweep(s, center, rotation, direction, maxDistance, ignore);
}

std::optional<SceneHit> Scene::sweepCapsule(const GlobalPosition& center, f32 radius, f32 halfHeight,
                                            const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                            Entity ignore) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Capsule;
    s.radius = radius;
    s.halfHeight = halfHeight;
    return sweep(s, center, rotation, direction, maxDistance, ignore);
}

std::optional<SceneHit> Scene::sweepEntity(Entity entity, const glm::vec3& direction, f32 maxDistance) {
    BodyRef b = bodyOf(entity);
    if (!b) return std::nullopt;
    physics::SweepHit hit;
    if (!b.region->world->sweepBody(b.body, direction, maxDistance, hit)) return std::nullopt;
    return toSceneHit(hit, entity.transform().position, *b.region);
}

Entity Scene::primaryCamera() {
    for (auto [e, c] : m_registry.view<CameraComponent>().each())
        if (c.primary) return {e, this};
    return {};
}

bool Scene::buildRenderScene(RenderScene& out, f32 aspect, const CameraData* cameraOverride) {
    out.clear();
    out.time = static_cast<f32>(m_time.secondsInCycle(kShaderTimeCycle));

    bool hasCamera = false;
    if (cameraOverride) {
        out.camera = *cameraOverride;
        hasCamera = true;
    } else if (Entity cam = primaryCamera()) {
        auto& t = cam.transform();
        auto& c = cam.get<CameraComponent>();
        // Camera-relative: the camera is the frame's origin, everything else is measured from it.
        out.camera = CameraData::relative(t.position, t.orientation(), c.fovY, aspect, c.nearClip, c.farClip);
        hasCamera = true;
    }

    auto suns = m_registry.view<TransformComponent, DirectionalLightComponent>();
    if (auto it = suns.begin(); it != suns.end()) { // one sun
        auto [t, l] = suns.get<TransformComponent, DirectionalLightComponent>(*it);
        out.sun.direction = t.forward();
        out.sun.color = l.color;
        out.sun.intensity = l.intensity;
    }

    for (auto [e, t, l] : m_registry.view<TransformComponent, PointLightComponent>().each())
        out.pointLights.push_back({(t.position - out.camera.origin).toVec3(), l.radius, l.color, l.intensity});

    for (auto [e, t, m] : m_registry.view<TransformComponent, MeshComponent>().each()) {
        if (!m.mesh) continue;
        out.draws.push_back({m.mesh, t.matrixRelativeTo(out.camera.origin), m.material, m.castShadows});
    }
    return hasCamera;
}

} // namespace nova
