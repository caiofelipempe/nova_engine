#include "WorldScenarios.h"

#include <cmath>

using namespace nova;

// ------------------------------------------------------------------ scripts --
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

void Mover::onWorldBoundary(const WorldBoundaryHit& hit) {
    // Point away from the edge we touched (the physics bounce already did the same to the velocity).
    if (direction[hit.axis] * hit.normal[hit.axis] < 0.0f) direction[hit.axis] = -direction[hit.axis];
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
    // 60 × 40 × 60 m, barriers everywhere (Y from −20 to +20: the floor sits at y = 0).
    scene.setWorldSpace(WorldSpaceDesc{}
                            .setUnit(1, LengthUnit::Millimeter)
                            .setAxis(0, kSmallWorldSize, LengthUnit::Meter, WorldBoundary::Barrier)
                            .setAxis(1, 40, LengthUnit::Meter, WorldBoundary::Barrier)
                            .setAxis(2, kSmallWorldSize, LengthUnit::Meter, WorldBoundary::Barrier));
    common(scene);

    Entity floor = mesh(scene, "Floor", m.plane, {0.0, 0.0, 0.0}, {kSmallWorldSize, 1.0f, kSmallWorldSize},
                        {0.78f, 0.78f, 0.8f, 1});
    floor.get<MeshComponent>().material.checker = true;
    floor.add<RigidBodyComponent>(physics::BodyType::Static);
    floor.add<BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f));

    // Balls wandering around: they hit the invisible edges and flash.
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
    // X and Z loop every 60 m; Y is a 40 m barrier.
    scene.setWorldSpace(WorldSpaceDesc{}
                            .setUnit(1, LengthUnit::Millimeter)
                            .setAxis(0, kSmallWorldSize, LengthUnit::Meter, WorldBoundary::Wrap)
                            .setAxis(1, 40, LengthUnit::Meter, WorldBoundary::Barrier)
                            .setAxis(2, kSmallWorldSize, LengthUnit::Meter, WorldBoundary::Wrap));
    common(scene);

    // The floor is 6 × 6 tiles of 10 m: each is drawn and simulated the short way from the
    // camera, so the floor always surrounds you, across the seams too.
    const int tiles = 6;
    const double tile = kSmallWorldSize / tiles;
    for (int x = 0; x < tiles; ++x)
        for (int z = 0; z < tiles; ++z) {
            const GlobalPosition at(-kSmallWorldSize / 2 + tile * (x + 0.5), 0.0, -kSmallWorldSize / 2 + tile * (z + 0.5));
            const float shade = ((x + z) % 2) ? 0.72f : 0.84f;
            Entity t = mesh(scene, "Tile " + std::to_string(x) + "," + std::to_string(z), m.plane, at,
                            {static_cast<float>(tile), 1.0f, static_cast<float>(tile)}, {shade, shade, shade + 0.03f, 1});
            t.get<MeshComponent>().material.checker = true;
            t.add<RigidBodyComponent>(physics::BodyType::Static);
            t.add<BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f));
        }
    // Landmarks: a red pillar just before the X seam, a blue one just after it, a yellow one on
    // the Z seam — fly across and you see them side by side.
    const std::pair<GlobalPosition, glm::vec4> pillars[] = {{{27.0, 3.0, 0.0}, {0.9f, 0.2f, 0.2f, 1}},
                                                            {{-27.0, 3.0, 0.0}, {0.2f, 0.4f, 0.95f, 1}},
                                                            {{0.0, 3.0, 28.0}, {0.95f, 0.85f, 0.2f, 1}},
                                                            {{12.0, 3.0, -12.0}, {0.3f, 0.85f, 0.4f, 1}}};
    for (const auto& [at, color] : pillars) {
        Entity p = mesh(scene, "Pillar", m.cube, at, {1.0f, 6.0f, 1.0f}, color);
        p.add<RigidBodyComponent>(physics::BodyType::Static);
        p.add<BoxColliderComponent>();
    }
    // Balls rolling forever: along X, along Z and diagonally, looping through the seams.
    const glm::vec3 directions[] = {{1, 0, 0}, {0, 0, 1}, {1, 0, 1}, {-1, 0, 0.5f}};
    for (int i = 0; i < 8; ++i) {
        Entity b = ball(scene, m, "Runner " + std::to_string(i), {-20.0 + i * 5.0, 0.5, -15.0 + (i % 3) * 12.0},
                        kColors[i % 6]);
        b.get<RigidBodyComponent>().restitution = 0.2f;
        auto& mover = b.addScript<Mover>();
        mover.direction = directions[i % 4];
        mover.speed = 5.0f + (i % 3) * 2.0f;
    }
    return camera(scene, {20.0, 8.0, 10.0}, {28.0, 2.0, 0.0}); // looking at the X seam
}

} // namespace

Entity buildSmallWorld(Scene& scene, const ScenarioMeshes& meshes, SmallWorld kind) {
    scene.clear();
    scene.setName(kind == SmallWorld::Closed ? "Small World (closed)" : "Small World (open)");
    return kind == SmallWorld::Closed ? buildClosed(scene, meshes) : buildOpen(scene, meshes);
}
