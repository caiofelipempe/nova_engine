#include "nova/scene/Scene.h"

#include "nova/core/Log.h"

#include <algorithm>
#include <cstring>
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
    m_registry.on_destroy<JointComponent>().connect<&Scene::onJointDestroyed>(*this);
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

EntityRef Scene::refOf(Entity entity) {
    if (!entity.valid()) return {};
    const auto* id = entity.tryGet<IdComponent>();
    return id ? EntityRef{id->uuid} : EntityRef{};
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

    if (m_physics) stepPhysics(frame);
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
