// Scene <-> physics (frontend): turns physics components into bodies of a physics::PhysicsWorld,
// steps it, writes results back into the ECS and exposes queries in entities. Talks to physics
// only through the global contract; backends (floating origin, Jolt) live in physics/backend.
#include "nova/scene/Scene.h"

#include "nova/core/Log.h"

#include <algorithm>
#include <cstring>

namespace nova {

// ----------------------------------------------------------------- physics --
// The scene talks to physics only in global positions (physics::PhysicsWorld). Whatever a backend
// needs to reach that precision (floating origins, regions...) lives behind that interface.
namespace {
u64 toUserData(entt::entity e) { return static_cast<u64>(entt::to_integral(e)); }
entt::entity fromUserData(u64 v) { return static_cast<entt::entity>(static_cast<entt::id_type>(v)); }
} // namespace

void Scene::startSimulation(physics::Backend backend) {
    if (m_physics) return;
    startSimulation(physics::PhysicsWorld::create(backend));
}

void Scene::startSimulation(Scope<physics::PhysicsWorld> world) {
    if (m_physics || !world) return;
    activateWorldSpace();
    m_physics = std::move(world);
    m_physicsAccumulator = Duration();
    pushPhysicsFocus(); // before the bodies: they are placed where the action is
    for (auto e : m_registry.view<RigidBodyComponent>()) createBody(e);
}

void Scene::stopSimulation() {
    if (!m_physics) return;
    for (auto [e, rb] : m_registry.view<RigidBodyComponent>().each()) rb.body = {};
    m_physics.reset();
    m_geometry.clear();
}

namespace {
Scene::MeshSource& defaultMeshSource() {
    static Scene::MeshSource source;
    return source;
}
u32 floatBits(f32 v) {
    u32 b;
    std::memcpy(&b, &v, sizeof(b));
    return b;
}
} // namespace

void Scene::setDefaultMeshSource(MeshSource source) { defaultMeshSource() = std::move(source); }

const MeshData* Scene::meshData(MeshHandle mesh) const {
    if (!mesh) return nullptr;
    if (m_meshSource) return m_meshSource(mesh);
    const MeshSource& fallback = defaultMeshSource();
    return fallback ? fallback(mesh) : nullptr;
}

std::shared_ptr<const physics::ShapeGeometry> Scene::meshGeometry(MeshHandle mesh, const glm::vec3& scale, bool triangles) {
    const auto key = std::make_tuple(mesh.id, floatBits(scale.x), floatBits(scale.y), floatBits(scale.z), triangles);
    if (auto it = m_geometry.find(key); it != m_geometry.end()) return it->second;
    const MeshData* data = meshData(mesh);
    if (!data || data->vertices.empty()) return nullptr;
    auto g = std::make_shared<physics::ShapeGeometry>();
    g->points.reserve(data->vertices.size());
    for (const Vertex& v : data->vertices) g->points.push_back(v.position * scale); // local to the body, scaled
    if (triangles) g->indices = data->indices;
    m_geometry.emplace(key, g);
    return g;
}

std::vector<GlobalPosition> Scene::currentFoci() {
    if (m_hasPhysicsFocus) return m_physicsFoci;
    if (Entity cam = primaryCamera()) return {cam.transform().position};
    return {};
}

void Scene::pushPhysicsFocus() {
    physics::SimulationFocus focus;
    focus.foci = currentFoci();
    focus.radius = physicsRegionRadius;
    focus.rebaseDistance = physicsRebaseDistance;
    focus.barrierWallRange = barrierWallRange;
    m_physics->setFocus(focus);
}

bool Scene::isPhysicsDormant(Entity e) const {
    if (!m_physics || !e.valid()) return false;
    const auto* rb = e.tryGet<RigidBodyComponent>();
    return rb && rb->body && m_physics->isDormant(rb->body);
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
    } else if (auto* cylinder = m_registry.try_get<CylinderColliderComponent>(e)) {
        desc.shape.type = physics::ShapeType::Cylinder;
        desc.shape.radius = cylinder->radius * std::max(s.x, s.z);
        desc.shape.halfHeight = cylinder->halfHeight * s.y;
        desc.shape.offset = cylinder->offset * s;
    } else if (m_registry.any_of<ConvexHullColliderComponent, MeshColliderComponent>(e)) {
        // Polyhedra from the mesh's vertices, local to the body (scaled; the rotation is the body's).
        const auto* hull = m_registry.try_get<ConvexHullColliderComponent>(e);
        const auto* mc = m_registry.try_get<MeshColliderComponent>(e);
        MeshHandle mesh = hull ? hull->mesh : mc->mesh;
        if (!mesh)
            if (const auto* m = m_registry.try_get<MeshComponent>(e)) mesh = m->mesh;
        bool triangles = mc != nullptr;
        if (triangles && rb.type == physics::BodyType::Dynamic) {
            NOVA_CORE_WARN("Scene: '{}' has a MeshCollider on a dynamic body; using its convex hull",
                           Entity{e, this}.name());
            triangles = false;
        }
        desc.shape.type = triangles ? physics::ShapeType::Mesh : physics::ShapeType::ConvexHull;
        desc.shape.geometry = meshGeometry(mesh, s, triangles);
        if (hull) desc.shape.offset = hull->offset * s;
        if (!desc.shape.geometry) {
            NOVA_CORE_WARN("Scene: no vertices for the collider of '{}' (mesh {}); using a box",
                           Entity{e, this}.name(), mesh.id);
            desc.shape = {};
            desc.shape.halfExtents = 0.5f * s;
        }
    } else {
        desc.shape.type = physics::ShapeType::Box;
        desc.shape.halfExtents = 0.5f * s;
    }
    desc.type = rb.type;
    desc.position = t.position; // global: the backend decides how to get there
    desc.rotation = t.rotation;
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
    auto& rb = registry.get<RigidBodyComponent>(e);
    if (m_physics && rb.body) m_physics->destroyBody(rb.body);
    rb.body = {};
}

void Scene::stepPhysics(Duration dt) {
    pushPhysicsFocus(); // the simulation follows the foci (regions, dormancy: up to the backend)
    for (auto [e, rb] : m_registry.view<RigidBodyComponent>().each()) // gained a rigid body since last frame
        if (!rb.body) createBody(e);

    constexpr int kMaxSteps = 4; // avoid the spiral of death after a hitch
    m_physicsAccumulator += dt;
    int steps = 0;
    while (fixedTimeStep > Duration() && m_physicsAccumulator >= fixedTimeStep && steps < kMaxSteps) {
        for (auto [e, rb, t] : m_registry.view<RigidBodyComponent, TransformComponent>().each())
            if (rb.type == physics::BodyType::Kinematic && rb.body)
                m_physics->moveKinematic(rb.body, t.position, t.rotation, fixedTimeStep);
        m_physics->step(fixedTimeStep);
        m_physicsAccumulator -= fixedTimeStep;
        ++steps;
    }
    if (steps == kMaxSteps) m_physicsAccumulator = Duration();
    if (steps == 0) return;

    // Simulation -> ECS
    for (auto [e, rb, t] : m_registry.view<RigidBodyComponent, TransformComponent>().each()) {
        if (rb.type != physics::BodyType::Dynamic || !rb.body || !m_physics->isActive(rb.body)) continue;
        glm::quat q;
        m_physics->getTransform(rb.body, t.position, q);
        t.setRotation(q);
    }

    // Collision callbacks for scripts on both entities, each seen from its own side.
    for (const physics::ContactEvent& c : m_physics->drainContactEvents()) {
        if (physics::isWorldBoundary(c.userDataA) || physics::isWorldBoundary(c.userDataB)) { // a barrier edge
            const bool wallIsA = physics::isWorldBoundary(c.userDataA);
            const u64 wall = wallIsA ? c.userDataA : c.userDataB;
            const u64 other = wallIsA ? c.userDataB : c.userDataA;
            Entity self{fromUserData(other), this};
            if (physics::isWorldBoundary(other) || !self.valid()) continue;
            WorldBoundaryHit hit;
            hit.axis = static_cast<int>((wall & 0xFF) / 2);
            hit.side = (wall & 1) ? 1 : -1;
            hit.point = wallIsA ? c.pointB : c.pointA;
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
        notify(a, Collision{b, -c.normal, c.pointA, c.pointB, c.surfaceNormalA, c.surfaceNormalB});
        notify(b, Collision{a, c.normal, c.pointB, c.pointA, c.surfaceNormalB, c.surfaceNormalA});
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
        m_physics->setTransform(b, t.position, t.rotation);
        m_physics->setLinearVelocity(b, glm::vec3(0.0f));
        m_physics->setAngularVelocity(b, glm::vec3(0.0f));
    }
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
physics::QueryFilter Scene::filterIgnoring(Entity e) const {
    physics::QueryFilter f;
    if (e.valid())
        if (auto* rb = e.tryGet<RigidBodyComponent>()) f.ignoreBody = rb->body;
    return f;
}

SceneHit Scene::toSceneHit(const physics::QueryHit& hit) {
    SceneHit out;
    out.entity = physics::isWorldBoundary(hit.userData) ? Entity{} : Entity{fromUserData(hit.userData), this};
    out.point = hit.point;
    out.normal = hit.normal;
    out.hitSurfaceNormal = hit.hitSurfaceNormal;
    out.sweptPoint = hit.sweptPoint;
    out.sweptSurfaceNormal = hit.sweptSurfaceNormal;
    out.distance = hit.distance;
    return out;
}

std::optional<SceneHit> Scene::raycast(const GlobalPosition& origin, const glm::vec3& direction, f32 maxDistance,
                                       Entity ignore) {
    if (!m_physics) return std::nullopt;
    physics::RaycastHit hit;
    if (!m_physics->raycast(origin, direction, maxDistance, hit, filterIgnoring(ignore))) return std::nullopt;
    return toSceneHit(hit);
}

std::optional<SceneHit> Scene::sweep(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                     const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                     Entity ignore) {
    if (!m_physics) return std::nullopt;
    physics::SweepHit hit;
    if (!m_physics->sweep(shape, position, rotation, direction, maxDistance, hit, filterIgnoring(ignore)))
        return std::nullopt;
    return toSceneHit(hit);
}

std::vector<SceneHit> Scene::sweepAll(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                      const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                      Entity ignore) {
    std::vector<SceneHit> out;
    if (!m_physics) return out;
    for (const auto& h : m_physics->sweepAll(shape, position, rotation, direction, maxDistance, filterIgnoring(ignore)))
        out.push_back(toSceneHit(h));
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
    return toSceneHit(hit);
}

} // namespace nova
