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
    physics::WorldDesc desc;
    desc.gravity = m_physicsSettings.gravity;
    desc.layers = m_physicsSettings.layers;
    startSimulation(physics::PhysicsWorld::create(backend, desc));
}

void Scene::setPhysicsSettings(const PhysicsSettings& settings) {
    m_physicsSettings = settings;
    if (!m_physics) return;
    m_physics->setGravity(settings.gravity);
    m_physics->setCollisionLayers(settings.layers);
}

void Scene::startSimulation(Scope<physics::PhysicsWorld> world) {
    if (m_physics || !world) return;
    activateWorldSpace();
    m_physics = std::move(world);
    m_physics->setGravity(m_physicsSettings.gravity); // a world made elsewhere follows the scene too
    m_physics->setCollisionLayers(m_physicsSettings.layers);
    m_physicsAccumulator = Duration();
    pushPhysicsFocus(); // before the bodies: they are placed where the action is
    for (auto e : m_registry.view<RigidBodyComponent>()) createBody(e);
}

void Scene::stopSimulation() {
    if (!m_physics) return;
    for (auto [e, rb] : m_registry.view<RigidBodyComponent>().each()) rb.body = {};
    m_physics.reset();
    m_geometry.clear();
    m_warnedMeshOnDynamic.clear();
    m_warnedNoVertices.clear();
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

physics::BodyDesc Scene::bodyDescFor(entt::entity e) {
    auto& rb = m_registry.get<RigidBodyComponent>(e);
    auto& t = m_registry.get<TransformComponent>(e);
    const glm::vec3 s = glm::abs(t.scale);

    // Every collider on the entity is a part; several make one compound shape. Sizes and offsets
    // follow the scale along the collider's own axes (exact unless rotated and scaled unevenly).
    std::vector<physics::ShapeDesc> parts;
    if (auto* box = m_registry.try_get<BoxColliderComponent>(e)) {
        physics::ShapeDesc p;
        p.type = physics::ShapeType::Box;
        p.halfExtents = box->halfExtents * s;
        p.offset = box->offset * s;
        p.rotation = box->rotation;
        parts.push_back(p);
    }
    if (auto* sphere = m_registry.try_get<SphereColliderComponent>(e)) {
        physics::ShapeDesc p;
        p.type = physics::ShapeType::Sphere;
        p.radius = sphere->radius * std::max({s.x, s.y, s.z});
        p.offset = sphere->offset * s;
        parts.push_back(p);
    }
    if (auto* capsule = m_registry.try_get<CapsuleColliderComponent>(e)) {
        physics::ShapeDesc p;
        p.type = physics::ShapeType::Capsule;
        p.radius = capsule->radius * std::max(s.x, s.z);
        p.halfHeight = capsule->halfHeight * s.y;
        p.offset = capsule->offset * s;
        p.rotation = capsule->rotation;
        parts.push_back(p);
    }
    if (auto* cylinder = m_registry.try_get<CylinderColliderComponent>(e)) {
        physics::ShapeDesc p;
        p.type = physics::ShapeType::Cylinder;
        p.radius = cylinder->radius * std::max(s.x, s.z);
        p.halfHeight = cylinder->halfHeight * s.y;
        p.offset = cylinder->offset * s;
        p.rotation = cylinder->rotation;
        parts.push_back(p);
    }
    if (m_registry.any_of<ConvexHullColliderComponent, MeshColliderComponent>(e)) {
        // Polyhedra from the mesh's vertices, local to the body (scaled; the rotation is the body's).
        const auto* hull = m_registry.try_get<ConvexHullColliderComponent>(e);
        const auto* mc = m_registry.try_get<MeshColliderComponent>(e);
        MeshHandle mesh = hull ? hull->mesh : mc->mesh;
        if (!mesh)
            if (const auto* m = m_registry.try_get<MeshComponent>(e)) mesh = m->mesh;
        bool triangles = mc != nullptr && !hull;
        if (triangles && rb.type == physics::BodyType::Dynamic) {
            if (m_warnedMeshOnDynamic.insert(e).second)
                NOVA_CORE_WARN("Scene: '{}' has a MeshCollider on a dynamic body; using its convex hull",
                               Entity{e, this}.name());
            triangles = false;
        }
        physics::ShapeDesc p;
        p.type = triangles ? physics::ShapeType::Mesh : physics::ShapeType::ConvexHull;
        p.geometry = meshGeometry(mesh, s, triangles);
        if (hull) {
            p.offset = hull->offset * s;
            p.rotation = hull->rotation;
        }
        if (p.geometry) {
            parts.push_back(p);
        } else if (m_warnedNoVertices.insert(e).second) {
            NOVA_CORE_WARN("Scene: no vertices for the collider of '{}' (mesh {}); ignoring it",
                           Entity{e, this}.name(), mesh.id);
        }
    }

    physics::BodyDesc desc;
    if (parts.empty()) {
        desc.shape.type = physics::ShapeType::Box;
        desc.shape.halfExtents = 0.5f * s;
    } else if (parts.size() == 1) {
        desc.shape = parts.front();
    } else {
        desc.shape.type = physics::ShapeType::Compound;
        desc.shape.children = std::make_shared<const std::vector<physics::ShapeDesc>>(std::move(parts));
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
    desc.layer = std::min(rb.layer, physics::kCollisionLayerCount - 2); // the last one is the world edge
    desc.continuousCollision = rb.continuousCollision;
    desc.lockedAxes = rb.locks.bits();
    desc.userData = toUserData(e);
    return desc;
}

namespace {
struct Hasher {
    u64 h = 1469598103934665603ull;
    void bytes(const void* data, size_t size) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    template <typename T>
    void operator()(const T& v) {
        bytes(&v, sizeof(v));
    }
    void shape(const physics::ShapeDesc& s) {
        (*this)(s.type);
        (*this)(s.halfExtents);
        (*this)(s.radius);
        (*this)(s.halfHeight);
        (*this)(s.offset);
        (*this)(s.rotation);
        (*this)(s.geometry.get()); // geometries are shared per mesh and scale
        if (s.children)
            for (const physics::ShapeDesc& c : *s.children) shape(c);
    }
};

// What needs a new body when it changes.
u64 shapeKey(const physics::BodyDesc& d) {
    Hasher h;
    h.shape(d.shape);
    h(d.type);
    h(d.mass);
    h(d.lockedAxes);
    return h.h | 1; // never 0 ("nothing applied")
}

// What changes in place.
u64 propertiesKey(const physics::BodyDesc& d) {
    Hasher h;
    h(d.friction);
    h(d.restitution);
    h(d.linearDamping);
    h(d.angularDamping);
    h(d.gravityFactor);
    h(d.isSensor);
    h(d.layer);
    h(d.continuousCollision);
    return h.h | 1;
}

u64 poseKey(const TransformComponent& t) {
    Hasher h;
    h(t.position.raw());
    h(t.rotation);
    return h.h | 1;
}
} // namespace

void Scene::createBody(entt::entity e) {
    auto& rb = m_registry.get<RigidBodyComponent>(e);
    const physics::BodyDesc desc = bodyDescFor(e);
    rb.body = m_physics->createBody(desc);
    rb.appliedShape = shapeKey(desc);
    rb.appliedProperties = propertiesKey(desc);
    rb.appliedPose = poseKey(m_registry.get<TransformComponent>(e));
}

// Edits made while simulating: rebuild, update in place, or move static bodies.
void Scene::syncBodies() {
    for (auto [e, rb, t] : m_registry.view<RigidBodyComponent, TransformComponent>().each()) {
        if (!rb.body) continue;
        const physics::BodyDesc desc = bodyDescFor(e);
        if (const u64 key = shapeKey(desc); key != rb.appliedShape) {
            const glm::vec3 v = m_physics->linearVelocity(rb.body), w = m_physics->angularVelocity(rb.body);
            physics::BodyDesc fresh = desc;
            if (rb.type == physics::BodyType::Dynamic) {
                m_physics->getTransform(rb.body, fresh.position, fresh.rotation); // where it is now
                fresh.linearVelocity = v;
            }
            m_physics->destroyBody(rb.body);
            rb.body = m_physics->createBody(fresh);
            if (rb.body && rb.type == physics::BodyType::Dynamic) m_physics->setAngularVelocity(rb.body, w);
            rb.appliedShape = key;
            rb.appliedProperties = propertiesKey(desc);
            rb.appliedPose = poseKey(t);
            continue;
        }
        if (const u64 key = propertiesKey(desc); key != rb.appliedProperties) {
            m_physics->setBodyProperties(rb.body, desc);
            rb.appliedProperties = key;
        }
        if (rb.type == physics::BodyType::Static)
            if (const u64 key = poseKey(t); key != rb.appliedPose) {
                m_physics->setTransform(rb.body, t.position, t.rotation);
                rb.appliedPose = key;
            }
    }
}

void Scene::onRigidBodyDestroyed(entt::registry& registry, entt::entity e) {
    auto& rb = registry.get<RigidBodyComponent>(e);
    if (m_physics && rb.body) m_physics->destroyBody(rb.body);
    rb.body = {};
}

void Scene::stepPhysics(Duration dt) {
    pushPhysicsFocus(); // the simulation follows the foci (regions, dormancy: up to the backend)
    syncBodies(); // edits since last frame
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

    dispatchContacts();
}

// Contact events to scripts, on both entities, each seen from its own side.
void Scene::dispatchContacts() {
    auto scriptsOf = [&](Entity e) -> ScriptComponent* { return e.valid() ? e.tryGet<ScriptComponent>() : nullptr; };
    auto each = [&](Entity self, auto&& fn) {
        if (auto* sc = scriptsOf(self))
            for (auto& s : sc->scripts)
                if (s->m_created) fn(*s);
    };
    for (const physics::ContactEvent& c : m_physics->drainContactEvents()) {
        const bool began = c.type == physics::ContactEventType::Began;
        if (physics::isWorldBoundary(c.userDataA) || physics::isWorldBoundary(c.userDataB)) { // a barrier edge
            if (!began) continue;
            const bool wallIsA = physics::isWorldBoundary(c.userDataA);
            const u64 wall = wallIsA ? c.userDataA : c.userDataB;
            const u64 other = wallIsA ? c.userDataB : c.userDataA;
            if (physics::isWorldBoundary(other)) continue;
            WorldBoundaryHit hit;
            hit.axis = static_cast<int>((wall & 0xFF) / 2);
            hit.side = (wall & 1) ? 1 : -1;
            hit.point = wallIsA ? c.pointB : c.pointA;
            hit.normal = glm::vec3(0.0f);
            hit.normal[hit.axis] = static_cast<f32>(-hit.side);
            each(Entity{fromUserData(other), this}, [&](Script& s) { s.onWorldBoundary(hit); });
            continue;
        }
        Entity a{fromUserData(c.userDataA), this};
        Entity b{fromUserData(c.userDataB), this};
        // Ended events may name a destroyed entity: hand it over invalid.
        const Entity otherOfA = b.valid() ? b : Entity{}, otherOfB = a.valid() ? a : Entity{};
        if (c.sensor) {
            if (began) {
                each(a, [&](Script& s) { s.onTriggerEnter(otherOfA); });
                each(b, [&](Script& s) { s.onTriggerEnter(otherOfB); });
            } else {
                each(a, [&](Script& s) { s.onTriggerExit(otherOfA); });
                each(b, [&](Script& s) { s.onTriggerExit(otherOfB); });
            }
            continue;
        }
        if (!began) {
            each(a, [&](Script& s) { s.onCollisionEnd(otherOfA); });
            each(b, [&](Script& s) { s.onCollisionEnd(otherOfB); });
            continue;
        }
        if (!a.valid() || !b.valid()) continue;
        // c.normal points from A to B: it pushes B out of A, so A is pushed along -normal.
        const Collision forA{b, -c.normal, c.pointA, c.pointB, c.surfaceNormalA, c.surfaceNormalB};
        const Collision forB{a, c.normal, c.pointB, c.pointA, c.surfaceNormalB, c.surfaceNormalA};
        each(a, [&](Script& s) { s.onCollision(forA); });
        each(b, [&](Script& s) { s.onCollision(forB); });
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
physics::QueryFilter Scene::filterIgnoring(Entity e, u32 layerMask) const {
    physics::QueryFilter f;
    f.layerMask = layerMask;
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
                                       Entity ignore, u32 layerMask) {
    if (!m_physics) return std::nullopt;
    physics::RaycastHit hit;
    if (!m_physics->raycast(origin, direction, maxDistance, hit, filterIgnoring(ignore, layerMask))) return std::nullopt;
    return toSceneHit(hit);
}

std::optional<SceneHit> Scene::sweep(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                     const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                     Entity ignore, u32 layerMask) {
    if (!m_physics) return std::nullopt;
    physics::SweepHit hit;
    if (!m_physics->sweep(shape, position, rotation, direction, maxDistance, hit, filterIgnoring(ignore, layerMask)))
        return std::nullopt;
    return toSceneHit(hit);
}

std::vector<SceneHit> Scene::sweepAll(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                      const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                      Entity ignore, u32 layerMask) {
    std::vector<SceneHit> out;
    if (!m_physics) return out;
    for (const auto& h :
         m_physics->sweepAll(shape, position, rotation, direction, maxDistance, filterIgnoring(ignore, layerMask)))
        out.push_back(toSceneHit(h));
    return out;
}

std::optional<SceneHit> Scene::sweepSphere(const GlobalPosition& origin, f32 radius, const glm::vec3& direction,
                                           f32 maxDistance, Entity ignore, u32 layerMask) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Sphere;
    s.radius = radius;
    return sweep(s, origin, glm::quat(1, 0, 0, 0), direction, maxDistance, ignore, layerMask);
}

std::optional<SceneHit> Scene::sweepBox(const GlobalPosition& center, const glm::vec3& halfExtents,
                                        const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                        Entity ignore, u32 layerMask) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Box;
    s.halfExtents = halfExtents;
    return sweep(s, center, rotation, direction, maxDistance, ignore, layerMask);
}

std::optional<SceneHit> Scene::sweepCapsule(const GlobalPosition& center, f32 radius, f32 halfHeight,
                                            const glm::quat& rotation, const glm::vec3& direction, f32 maxDistance,
                                            Entity ignore, u32 layerMask) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Capsule;
    s.radius = radius;
    s.halfHeight = halfHeight;
    return sweep(s, center, rotation, direction, maxDistance, ignore, layerMask);
}

std::optional<SceneHit> Scene::sweepEntity(Entity entity, const glm::vec3& direction, f32 maxDistance, u32 layerMask) {
    physics::BodyId body = bodyOf(entity);
    if (!body) return std::nullopt;
    physics::SweepHit hit;
    physics::QueryFilter f;
    f.layerMask = layerMask;
    if (!m_physics->sweepBody(body, direction, maxDistance, hit, f)) return std::nullopt;
    return toSceneHit(hit);
}

std::vector<Entity> Scene::overlap(const physics::ShapeDesc& shape, const GlobalPosition& position,
                                   const glm::quat& rotation, Entity ignore, u32 layerMask, bool includeSensors) {
    std::vector<Entity> out;
    if (!m_physics) return out;
    physics::QueryFilter f = filterIgnoring(ignore, layerMask);
    f.includeSensors = includeSensors;
    for (const physics::OverlapHit& h : m_physics->overlap(shape, position, rotation, f)) {
        if (physics::isWorldBoundary(h.userData)) continue;
        Entity e{fromUserData(h.userData), this};
        if (e.valid()) out.push_back(e);
    }
    return out;
}

std::vector<Entity> Scene::overlapSphere(const GlobalPosition& center, f32 radius, Entity ignore, u32 layerMask,
                                         bool includeSensors) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Sphere;
    s.radius = radius;
    return overlap(s, center, glm::quat(1, 0, 0, 0), ignore, layerMask, includeSensors);
}

std::vector<Entity> Scene::overlapBox(const GlobalPosition& center, const glm::vec3& halfExtents,
                                      const glm::quat& rotation, Entity ignore, u32 layerMask, bool includeSensors) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Box;
    s.halfExtents = halfExtents;
    return overlap(s, center, rotation, ignore, layerMask, includeSensors);
}

} // namespace nova
