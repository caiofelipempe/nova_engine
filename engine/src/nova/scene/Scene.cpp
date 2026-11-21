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
    m_physics.reset();
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
        glm::quat q = glm::angleAxis(r.speed * dt, glm::normalize(r.axis)) * t.orientation();
        t.rotation = glm::eulerAngles(q);
    }

    if (m_physics) stepPhysics(frame);
}

// ----------------------------------------------------------------- physics --
namespace {
u64 toUserData(entt::entity e) { return static_cast<u64>(entt::to_integral(e)); }
// Barrier walls are bodies without an entity: the top bit marks them, the low bits say which edge.
constexpr u64 kWallUserData = 1ull << 63;
bool isWall(u64 v) { return (v & kWallUserData) != 0; }
entt::entity fromUserData(u64 v) { return static_cast<entt::entity>(static_cast<entt::id_type>(v)); }
} // namespace

void Scene::startSimulation(physics::Backend backend) {
    if (m_physics) return;
    activateWorldSpace();
    m_physics = physics::PhysicsWorld::create(backend);
    m_physicsAccumulator = Duration();
    // Start the floating origin at the focus (or the camera) so nearby bodies are precise from frame one.
    if (m_hasPhysicsFocus) m_physicsOrigin = m_physicsFocus;
    else if (Entity cam = primaryCamera()) m_physicsOrigin = cam.transform().position;
    for (auto e : m_registry.view<RigidBodyComponent>()) createBody(e);
    rebuildBarrierWalls();
}

void Scene::stopSimulation() {
    if (!m_physics) return;
    for (auto [e, rb] : m_registry.view<RigidBodyComponent>().each()) rb.body = {};
    m_barrierWalls.clear();
    m_physics.reset();
}

void Scene::updatePhysicsOrigin() {
    GlobalPosition focus;
    if (m_hasPhysicsFocus) focus = m_physicsFocus;
    else if (Entity cam = primaryCamera()) focus = cam.transform().position;
    else return;
    if (isWithin(focus, m_physicsOrigin, physicsRebaseDistance)) return;
    // Shift by an amount floats represent exactly, so the integer origin and the physics engine
    // agree to the bit after the move (no drift however often it happens).
    const glm::vec3 shift = (focus - m_physicsOrigin).toVec3();
    m_physicsOrigin += GlobalOffset::fromMeters(shift);
    m_physics->shiftOrigin(shift);
    rebuildBarrierWalls();
}

void Scene::rebuildBarrierWalls() {
    for (physics::BodyId wall : m_barrierWalls) m_physics->destroyBody(wall);
    m_barrierWalls.clear();
    const WorldSpace& world = WorldSpace::active();
    const f32 thickness = 1.0f, span = static_cast<f32>(barrierWallRange);
    for (int axis = 0; axis < 3; ++axis) {
        const WorldSpace::Axis& a = world.axis(axis);
        if (a.boundary != WorldBoundary::Barrier) continue;
        for (int side : {-1, 1}) {
            // Distance along the axis from the physics origin to the edge.
            const i64 edgeRaw = side < 0 ? a.min : a.max;
            const f64 toEdge = static_cast<f64>(world.difference(axis, edgeRaw, m_physicsOrigin.raw(axis))) * world.unit();
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
            if (physics::BodyId wall = m_physics->createBody(desc)) m_barrierWalls.push_back(wall);
        }
    }
}

void Scene::createBody(entt::entity e) {
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
    desc.position = toPhysics(t.position);
    desc.rotation = t.orientation();
    desc.mass = rb.mass;
    desc.friction = rb.friction;
    desc.restitution = rb.restitution;
    desc.linearDamping = rb.linearDamping;
    desc.angularDamping = rb.angularDamping;
    desc.gravityFactor = rb.gravityFactor;
    desc.isSensor = rb.isSensor;
    desc.userData = toUserData(e);
    rb.body = m_physics->createBody(desc);
}

void Scene::onRigidBodyDestroyed(entt::registry& registry, entt::entity e) {
    if (!m_physics) return;
    auto& rb = registry.get<RigidBodyComponent>(e);
    if (rb.body) m_physics->destroyBody(rb.body);
    rb.body = {};
}

void Scene::stepPhysics(Duration dt) {
    updatePhysicsOrigin();
    // Entities that gained a rigid body since the last frame.
    for (auto [e, rb] : m_registry.view<RigidBodyComponent>().each())
        if (!rb.body) createBody(e);

    constexpr int kMaxSteps = 4; // avoid the spiral of death after a hitch
    m_physicsAccumulator += dt;
    const f32 step = fixedTimeStep.secondsF();
    int steps = 0;
    while (fixedTimeStep > Duration() && m_physicsAccumulator >= fixedTimeStep && steps < kMaxSteps) {
        for (auto [e, rb, t] : m_registry.view<RigidBodyComponent, TransformComponent>().each())
            if (rb.type == physics::BodyType::Kinematic && rb.body)
                m_physics->moveKinematic(rb.body, toPhysics(t.position), t.orientation(), step);
        m_physics->step(step);
        m_physicsAccumulator -= fixedTimeStep;
        ++steps;
    }
    if (steps == kMaxSteps) m_physicsAccumulator = Duration();
    if (steps == 0) return;

    // Simulation -> ECS
    for (auto [e, rb, t] : m_registry.view<RigidBodyComponent, TransformComponent>().each()) {
        if (rb.type != physics::BodyType::Dynamic || !rb.body || !m_physics->isActive(rb.body)) continue;
        glm::quat q;
        glm::vec3 p;
        m_physics->getTransform(rb.body, p, q);
        t.position = fromPhysics(p);
        t.rotation = glm::eulerAngles(q);
    }

    // Collision callbacks for scripts on both entities, each seen from its own side.
    for (const physics::ContactEvent& c : m_physics->drainContactEvents()) {
        if (isWall(c.userDataA) || isWall(c.userDataB)) { // a body touched a barrier edge of the world
            const bool wallIsA = isWall(c.userDataA);
            const u64 wall = wallIsA ? c.userDataA : c.userDataB;
            Entity self{fromUserData(wallIsA ? c.userDataB : c.userDataA), this};
            if (!self.valid() || isWall(wallIsA ? c.userDataB : c.userDataA)) continue;
            WorldBoundaryHit hit;
            hit.axis = static_cast<int>((wall & 0xFF) / 2);
            hit.side = (wall & 1) ? 1 : -1;
            hit.point = fromPhysics(wallIsA ? c.pointB : c.pointA);
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
        const GlobalPosition pa = fromPhysics(c.pointA), pb = fromPhysics(c.pointB);
        notify(a, Collision{b, -c.normal, pa, pb, c.surfaceNormalA, c.surfaceNormalB});
        notify(b, Collision{a, c.normal, pb, pa, c.surfaceNormalB, c.surfaceNormalA});
    }
}

physics::BodyId Scene::bodyOf(Entity e) {
    if (!m_physics || !e.valid()) return {};
    auto* rb = e.tryGet<RigidBodyComponent>();
    if (!rb) return {};
    if (!rb->body) createBody(e.handle());
    return rb->body;
}

void Scene::addForce(Entity e, const glm::vec3& force) {
    if (auto b = bodyOf(e)) m_physics->addForce(b, force);
}
void Scene::addImpulse(Entity e, const glm::vec3& impulse) {
    if (auto b = bodyOf(e)) m_physics->addImpulse(b, impulse);
}
void Scene::setLinearVelocity(Entity e, const glm::vec3& velocity) {
    if (auto b = bodyOf(e)) m_physics->setLinearVelocity(b, velocity);
}
glm::vec3 Scene::linearVelocity(Entity e) {
    auto b = bodyOf(e);
    return b ? m_physics->linearVelocity(b) : glm::vec3(0.0f);
}
void Scene::syncTransformToPhysics(Entity e) {
    if (auto b = bodyOf(e)) {
        auto& t = e.transform();
        m_physics->setTransform(b, toPhysics(t.position), t.orientation());
        m_physics->setLinearVelocity(b, glm::vec3(0.0f));
        m_physics->setAngularVelocity(b, glm::vec3(0.0f));
    }
}

// ----------------------------------------------------------------- queries --
physics::QueryFilter Scene::filterIgnoring(Entity e) {
    physics::QueryFilter f;
    if (e.valid())
        if (auto* rb = e.tryGet<RigidBodyComponent>()) f.ignoreBody = rb->body;
    return f;
}

SceneHit Scene::toSceneHit(const physics::QueryHit& hit, const GlobalPosition& queryOrigin) {
    SceneHit out;
    out.entity = isWall(hit.userData) ? Entity{} : Entity{fromUserData(hit.userData), this};
    out.point = fromPhysics(hit.point);
    out.normal = hit.normal;
    out.hitSurfaceNormal = hit.hitSurfaceNormal;
    out.sweptPoint = hit.sweptPoint == glm::vec3(0.0f) ? queryOrigin : fromPhysics(hit.sweptPoint);
    out.sweptSurfaceNormal = hit.sweptSurfaceNormal;
    out.distance = hit.distance;
    return out;
}

std::optional<SceneHit> Scene::raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance,
                                       Entity ignore) {
    if (!m_physics) return std::nullopt;
    physics::RaycastHit hit;
    if (!m_physics->raycast(toPhysics(origin), direction, maxDistance, hit, filterIgnoring(ignore))) return std::nullopt;
    return toSceneHit(hit, origin);
}

std::optional<SceneHit> Scene::sweep(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                     const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                     Entity ignore) {
    if (!m_physics) return std::nullopt;
    physics::SweepHit hit;
    if (!m_physics->sweep(shape, toPhysics(position), rotation, direction, maxDistance, hit, filterIgnoring(ignore)))
        return std::nullopt;
    return toSceneHit(hit, position);
}

std::vector<SceneHit> Scene::sweepAll(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                      const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                      Entity ignore) {
    std::vector<SceneHit> out;
    if (!m_physics) return out;
    for (const auto& h :
         m_physics->sweepAll(shape, toPhysics(position), rotation, direction, maxDistance, filterIgnoring(ignore)))
        out.push_back(toSceneHit(h, position));
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
    physics::BodyId body = bodyOf(entity);
    if (!body) return std::nullopt;
    physics::SweepHit hit;
    if (!m_physics->sweepBody(body, direction, maxDistance, hit)) return std::nullopt;
    return toSceneHit(hit, entity.transform().position);
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
