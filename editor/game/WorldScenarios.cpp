#include "WorldScenarios.h"

#include <cmath>

using namespace nova;

// ------------------------------------------------------------------ scripts --
bool isWall(Entity e) { return e.valid() && e.name().rfind("Wall", 0) == 0; }

void EdgeFlash::onCollision(const Collision& c) {
    if (!isWall(c.other)) return;
    glow = 1.0f;
    ++touches;
}

void EdgeFlash::onUpdate(float dt) {
    glow = std::max(0.0f, glow - dt * 2.0f);
    if (auto* mesh = entity().tryGet<MeshComponent>()) {
        mesh->material.emissive = {1.0f, 0.25f, 0.15f};
        mesh->material.emissiveStrength = glow * 6.0f;
    }
}

void Mover::onUpdate(float dt) {
    // Deterministic pseudo-random kicks (same run, same result).
    if (kickEvery > 0.0f && (sinceKick += dt) >= kickEvery) {
        sinceKick = 0.0f;
        seed = seed * 1664525u + 1013904223u;
        const float angle = static_cast<float>(seed % 3600u) * 0.1f * 3.14159265f / 180.0f;
        direction = {std::cos(angle), 0.0f, std::sin(angle)};
    }
    const glm::vec3 d = glm::normalize(direction);
    const glm::vec3 v = scene().linearVelocity(entity());
    const float along = glm::dot(v, d);
    if (along < speed) scene().setLinearVelocity(entity(), v + d * (speed - along)); // keep its vertical motion
}

void Mover::onCollision(const Collision& c) {
    // Point away from the wall we touched (the physics bounce already did the same to the velocity).
    if (!isWall(c.other)) return;
    const glm::vec3 n = c.normal;
    const float into = glm::dot(direction, n);
    if (into < 0.0f) direction -= 2.0f * into * n;
    direction.y = 0.0f;
}

void reflectScenarioTypes() {
    reflect::type<EdgeFlash>("EdgeFlash").field("touches", &EdgeFlash::touches, {.readOnly = true}).script();
    reflect::type<Mover>("Mover")
        .field("direction", &Mover::direction, {.speed = 0.01f})
        .field("speed", &Mover::speed, {.min = 0.0f, .max = 30.0f})
        .field("kickEvery", &Mover::kickEvery, {.min = 0.0f, .max = 10.0f, .tooltip = "Seconds; 0 = never"})
        .script();
}

// ---------------------------------------------------------------- builders --
namespace {

Entity mesh(Scene& scene, const std::string& name, MeshHandle m, const GlobalPosition& at, const glm::vec3& scale,
            const glm::vec4& color) {
    Entity e = scene.createEntity(name);
    e.transform().position = at;
    e.transform().scale = scale;
    auto& mc = e.add<MeshComponent>(m);
    mc.material.baseColor = color;
    return e;
}

Entity ball(Scene& scene, const ScenarioMeshes& m, const std::string& name, const GlobalPosition& at,
            const glm::vec4& color) {
    Entity b = mesh(scene, name, m.sphere, at, glm::vec3(1.0f), color);
    b.get<MeshComponent>().material.roughness = 0.3f;
    auto& rb = b.add<RigidBodyComponent>();
    rb.restitution = 0.6f;
    rb.friction = 0.2f;
    rb.linearDamping = 0.0f;
    b.add<SphereColliderComponent>();
    return b;
}

void common(Scene& scene) {
    Entity sun = scene.createEntity("Sun");
    sun.add<DirectionalLightComponent>(glm::vec3(1.0f, 0.95f, 0.85f), 3.5f);
    sun.transform().setEulerDegrees({-50.0f, 35.0f, 0.0f});
}

Entity camera(Scene& scene, const GlobalPosition& at, const GlobalPosition& lookAt) {
    Entity cam = scene.createEntity("Main Camera");
    cam.add<CameraComponent>().farClip = 400.0f;
    cam.transform().position = at;
    cam.transform().lookAt(lookAt);
    return cam;
}

const glm::vec4 kColors[] = {{0.9f, 0.3f, 0.25f, 1}, {0.25f, 0.6f, 0.95f, 1}, {0.95f, 0.8f, 0.2f, 1},
                             {0.4f, 0.85f, 0.4f, 1}, {0.75f, 0.4f, 0.9f, 1}, {0.95f, 0.55f, 0.2f, 1}};

Entity buildClosed(Scene& scene, const ScenarioMeshes& m) {
    // A 60 × 60 m arena: a floor at y = 0 and four walls.
    common(scene);
    Entity floor = mesh(scene, "Floor", m.plane, {0.0, 0.0, 0.0}, {kSmallWorldSize, 1.0f, kSmallWorldSize},
                        {0.78f, 0.78f, 0.8f, 1});
    floor.get<MeshComponent>().material.checker = true;
    floor.add<RigidBodyComponent>(physics::BodyType::Static);
    floor.add<BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f));
    const double half = kSmallWorldSize / 2;
    const std::pair<GlobalPosition, glm::vec3> walls[] = {{{half + 0.5, 2.0, 0.0}, {1.0f, 4.0f, kSmallWorldSize + 2}},
                                                          {{-half - 0.5, 2.0, 0.0}, {1.0f, 4.0f, kSmallWorldSize + 2}},
                                                          {{0.0, 2.0, half + 0.5}, {kSmallWorldSize + 2, 4.0f, 1.0f}},
                                                          {{0.0, 2.0, -half - 0.5}, {kSmallWorldSize + 2, 4.0f, 1.0f}}};
    for (const auto& [at, size] : walls) {
        Entity w = mesh(scene, "Wall", m.cube, at, size, {0.45f, 0.5f, 0.6f, 1});
        auto& rb = w.add<RigidBodyComponent>(physics::BodyType::Static);
        rb.restitution = 0.0f;
        w.add<BoxColliderComponent>();
    }

    // Balls wandering around: they hit the walls and flash.
    for (int i = 0; i < 12; ++i) {
        const double a = i * 0.5236, r = 6.0 + (i % 3) * 4.0;
        Entity b = ball(scene, m, "Ball " + std::to_string(i), {r * std::cos(a), 1.0 + (i % 4), r * std::sin(a)},
                        kColors[i % 6]);
        b.addScript<EdgeFlash>();
        auto& mover = b.addScript<Mover>();
        mover.speed = 7.0f + (i % 4);
        mover.kickEvery = 2.5f + (i % 5) * 0.7f;
        mover.seed = 17u + static_cast<unsigned>(i) * 31u;
        mover.direction = {std::cos(a), 0.0f, std::sin(a)};
    }
    // Crates to knock around.
    for (int i = 0; i < 8; ++i) {
        Entity c = mesh(scene, "Crate " + std::to_string(i), m.cube, {-12.0 + i * 3.4, 0.6, -4.0 + (i % 2) * 8.0},
                        glm::vec3(1.2f), {0.55f, 0.38f, 0.2f, 1});
        c.add<RigidBodyComponent>().mass = 3.0f;
        c.add<BoxColliderComponent>();
        c.addScript<EdgeFlash>();
    }
    return camera(scene, {0.0, 16.0, 26.0}, {0.0, 0.0, 0.0});
}

Entity buildOpen(Scene& scene, const ScenarioMeshes& m) {
    // A floor strip across the X seam of the torus (+8 192 m ≡ −8 192 m): 400 m long, 60 m wide.
    common(scene);
    const double seam = WorldSpace::kHalfLap, tile = 10.0;
    const int along = static_cast<int>(kSeamFloorLength / tile), across = static_cast<int>(kSmallWorldSize / tile);
    for (int x = 0; x < along; ++x)
        for (int z = 0; z < across; ++z) {
            // Tile centers from 200 m before the seam to 200 m after it (positions loop by themselves).
            const GlobalPosition at(seam - kSeamFloorLength / 2 + tile * (x + 0.5), 0.0,
                                    -kSmallWorldSize / 2 + tile * (z + 0.5));
            const float shade = ((x + z) % 2) ? 0.72f : 0.84f;
            Entity t = mesh(scene, "Tile " + std::to_string(x) + "," + std::to_string(z), m.plane, at,
                            {static_cast<float>(tile), 1.0f, static_cast<float>(tile)}, {shade, shade, shade + 0.03f, 1});
            t.get<MeshComponent>().material.checker = true;
            t.add<RigidBodyComponent>(physics::BodyType::Static);
            t.add<BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f));
        }
    // Landmarks: a red pillar just before the seam, a blue one just after it (all off the runners'
    // lanes, which are 6 m apart from z = −21 to +21).
    const std::pair<GlobalPosition, glm::vec4> pillars[] = {{{seam - 3.0, 3.0, 0.0}, {0.9f, 0.2f, 0.2f, 1}},
                                                            {{-seam + 3.0, 3.0, 0.0}, {0.2f, 0.4f, 0.95f, 1}},
                                                            {{seam - 3.0, 3.0, 25.0}, {0.95f, 0.85f, 0.2f, 1}},
                                                            {{-seam + 3.0, 3.0, -25.0}, {0.3f, 0.85f, 0.4f, 1}}};
    for (const auto& [at, color] : pillars) {
        Entity p = mesh(scene, "Pillar", m.cube, at, {1.0f, 6.0f, 1.0f}, color);
        p.add<RigidBodyComponent>(physics::BodyType::Static);
        p.add<BoxColliderComponent>();
    }
    // Runners heading through the seam along +X, starting 150 m before it.
    for (int i = 0; i < 8; ++i) {
        Entity b = ball(scene, m, "Runner " + std::to_string(i), {seam - 150.0 + (i % 4) * 4.0, 0.5, -21.0 + i * 6.0},
                        kColors[i % 6]);
        b.get<RigidBodyComponent>().restitution = 0.2f;
        auto& mover = b.addScript<Mover>();
        mover.direction = {1.0f, 0.0f, 0.0f};
        mover.speed = 7.0f + (i % 3) * 1.5f;
    }
    return camera(scene, {seam - 20.0, 8.0, 10.0}, {seam + 5.0, 2.0, 0.0}); // looking at the seam
}

} // namespace

Entity buildSmallWorld(Scene& scene, const ScenarioMeshes& meshes, SmallWorld kind) {
    scene.clear();
    scene.setName(kind == SmallWorld::Closed ? "Arena (walls)" : "Torus seam");
    return kind == SmallWorld::Closed ? buildClosed(scene, meshes) : buildOpen(scene, meshes);
}
