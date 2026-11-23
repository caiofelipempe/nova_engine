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
Scene::Scene(std::string name) : m_name(std::move(name)) { connectSignals(); }

void Scene::connectSignals() {
    // Removing the component (or destroying the entity) removes the body from the world.
    m_registry.on_destroy<RigidBodyComponent>().connect<&Scene::onRigidBodyDestroyed>(*this);
    m_registry.on_destroy<JointComponent>().connect<&Scene::onJointDestroyed>(*this);
    m_registry.on_destroy<CharacterControllerComponent>().connect<&Scene::onCharacterDestroyed>(*this);
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
    // Its descendants go with it, the deepest first.
    std::vector<Entity> doomed = descendantsOf(entity);
    std::reverse(doomed.begin(), doomed.end());
    doomed.push_back(entity);
    for (Entity e : doomed) {
        if (!e.valid()) continue;
        if (auto* sc = e.tryGet<ScriptComponent>())
            for (auto& s : sc->scripts)
                if (s->m_created) s->onDestroy();
        if (e.valid()) m_registry.destroy(e.handle());
    }
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

void Scene::update(Duration frame) {
    activateWorld(); // positions go around this scene's world
    m_time += frame;

    updateTransforms(); // links made since the last update (loaded, set by tools) before scripts move parents

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
            s.onUpdate(frame);
            if (!m_registry.valid(e)) break;
        }
    }

    // Built-in systems.
    for (auto [e, t, r] : m_registry.view<TransformComponent, RotatorComponent>().each()) {
        const f64 angle = toDouble(r.speed) * frame.seconds(); // exact frame time, no float in between
        t.rotation = (TransformComponent::turn(angle, r.axis.toDouble()) * t.rotation).normalized();
    }

    updateAnimations(frame); // clips set their nodes' offsets from their parents
    updateTransforms(); // children follow their parents (moved by scripts, systems and animations)
    if (m_physics) {
        stepPhysics(frame);
        updateTransforms(); // and their parents' bodies (a child's own dynamic body moves it on its own)
    }
    dispatchWrapHits();
}

Entity Scene::primaryCamera() {
    for (auto [e, c] : m_registry.view<CameraComponent>().each())
        if (c.primary) return {e, this};
    return {};
}

bool Scene::buildRenderScene(RenderScene& out, f32 aspect, const CameraData* cameraOverride) {
    activateWorld();
    updateTransforms(); // children where their parents are now (moved by tools since the update)
    out.clear();
    out.time = static_cast<f32>(m_time.secondsInCycle(kShaderTimeCycle));
    out.wrapSize = glm::vec3(m_worldSize.sizes());

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

    out.worldCenter = (GlobalPosition() - out.camera.origin).toVec3();

    auto suns = m_registry.view<TransformComponent, DirectionalLightComponent>();
    if (auto it = suns.begin(); it != suns.end()) { // one sun
        auto [t, l] = suns.get<TransformComponent, DirectionalLightComponent>(*it);
        out.sun.direction = t.forward();
        out.sun.color = l.color;
        out.sun.intensity = l.intensity;
    }

    // What the physics moves is drawn between its last two steps (interpolatePhysics).
    TransformComponent drawn;
    for (auto [e, t, l] : m_registry.view<TransformComponent, PointLightComponent>().each()) {
        const TransformComponent& at = drawnPose(e, t, drawn) ? drawn : t;
        out.pointLights.push_back({(at.position - out.camera.origin).toVec3(), l.radius, l.color, l.intensity});
    }

    for (auto [e, t, m] : m_registry.view<TransformComponent, MeshComponent>().each()) {
        if (!m.mesh) continue;
        const TransformComponent& at = drawnPose(e, t, drawn) ? drawn : t;
        out.draws.push_back({m.mesh, at.matrixRelativeTo(out.camera.origin), m.material, m.castShadows});
    }
    return hasCamera;
}

} // namespace nova
