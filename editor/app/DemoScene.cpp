#include "DemoScene.h"

using namespace nova;

namespace {
Entity primitive(Scene& scene, const std::string& name, MeshHandle mesh) {
    Entity e = scene.createEntity(name);
    e.add<MeshComponent>(mesh);
    return e;
}
} // namespace

void buildDemoScene(editor::EditorContext& ctx) {
    Scene& scene = ctx.scene();
    Renderer& r = ctx.renderer();

    Entity cam = scene.createEntity("Main Camera");
    cam.add<CameraComponent>();
    cam.transform().position = {0.0f, 3.0f, 10.0f};
    cam.transform().lookAt({0.0f, 1.0f, 0.0f});

    Entity sun = scene.createEntity("Sun");
    sun.add<DirectionalLightComponent>(glm::vec3(1.0f, 0.95f, 0.85f), 3.5f);
    sun.transform().rotation = {glm::radians(-42.0f), glm::radians(30.0f), 0.0f};

    Entity ground = primitive(scene, "Ground", r.plane());
    ground.transform().scale = {60.0f, 1.0f, 60.0f};
    auto& gm = ground.get<MeshComponent>().material;
    gm.baseColor = {0.75f, 0.75f, 0.78f, 1.0f};
    gm.roughness = 0.8f;
    gm.checker = true;
    ground.add<RigidBodyComponent>(physics::BodyType::Static);
    ground.add<BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f));

    // Physics playground: a stack of boxes and balls falling onto it (press Play).
    for (int y = 0; y < 6; ++y) {
        Entity b = primitive(scene, "Box " + std::to_string(y), r.cube());
        b.transform().position = {7.5f + (y % 2) * 0.15f, 0.4f + y * 0.81f, 1.0f};
        b.transform().scale = glm::vec3(0.8f);
        b.get<MeshComponent>().material.baseColor = {0.55f, 0.38f, 0.2f, 1.0f};
        b.add<RigidBodyComponent>();
        b.add<BoxColliderComponent>();
    }
    for (int i = 0; i < 4; ++i) {
        Entity ball = primitive(scene, "Ball " + std::to_string(i), r.sphere());
        ball.transform().position = {7.3f + 0.2f * i, 7.0f + 1.5f * i, 1.0f + 0.15f * i};
        ball.transform().scale = glm::vec3(0.6f);
        auto& bm = ball.get<MeshComponent>().material;
        bm.baseColor = {0.2f, 0.8f, 0.35f, 1.0f};
        bm.roughness = 0.3f;
        ball.add<RigidBodyComponent>().restitution = 0.6f;
        ball.add<SphereColliderComponent>();
    }

    // Rows of spheres sweeping roughness: red dielectric and gold metal.
    for (int i = 0; i < 6; ++i) {
        Entity s = primitive(scene, "Sphere " + std::to_string(i), r.sphere());
        s.transform().position = {-5.0f + 2.0f * i, 0.6f, 0.0f};
        s.transform().scale = glm::vec3(1.2f);
        auto& m = s.get<MeshComponent>().material;
        m.baseColor = {0.85f, 0.15f, 0.12f, 1.0f};
        m.roughness = 0.05f + 0.18f * i;
        s.add<RigidBodyComponent>(physics::BodyType::Static);
        s.add<SphereColliderComponent>();

        Entity g = primitive(scene, "Gold " + std::to_string(i), r.sphere());
        g.transform().position = {-5.0f + 2.0f * i, 0.6f, 2.5f};
        g.transform().scale = glm::vec3(1.2f);
        auto& gmat = g.get<MeshComponent>().material;
        gmat.baseColor = {1.0f, 0.78f, 0.34f, 1.0f};
        gmat.metallic = 1.0f;
        gmat.roughness = 0.05f + 0.18f * i;
    }

    // Pillars that cast long shadows.
    for (int i = 0; i < 4; ++i) {
        Entity p = primitive(scene, "Pillar " + std::to_string(i), r.cube());
        p.transform().position = {-6.0f + 4.0f * i, 2.0f, -4.0f};
        p.transform().scale = {0.6f, 4.0f, 0.6f};
        p.get<MeshComponent>().material.baseColor = {0.6f, 0.62f, 0.66f, 1.0f};
        p.add<RigidBodyComponent>(physics::BodyType::Static);
        p.add<BoxColliderComponent>();
    }

    Entity crate = primitive(scene, "Spinning Crate", r.cube());
    crate.transform().position = {0.0f, 2.2f, -1.5f};
    crate.transform().scale = glm::vec3(1.2f);
    crate.get<MeshComponent>().material.baseColor = {0.2f, 0.45f, 0.9f, 1.0f};
    crate.add<RotatorComponent>(glm::vec3(0.4f, 1.0f, 0.2f), 0.7f);
    crate.add<RigidBodyComponent>(physics::BodyType::Kinematic);

    const std::pair<const char*, glm::vec3> lamps[] = {{"Lamp", {1.0f, 0.55f, 0.2f}}, {"Lamp 2", {0.3f, 0.6f, 1.0f}}};
    for (int i = 0; i < 2; ++i) {
        Entity lamp = primitive(scene, lamps[i].first, r.sphere());
        lamp.transform().position = {i == 0 ? 4.0f : -4.0f, i == 0 ? 1.5f : 1.2f, -2.0f};
        lamp.transform().scale = glm::vec3(0.25f);
        auto& lm = lamp.get<MeshComponent>().material;
        lm.emissive = lamps[i].second;
        lm.emissiveStrength = 12.0f;
        lamp.get<MeshComponent>().castShadows = false;
        lamp.add<PointLightComponent>(lamps[i].second, 25.0f, 9.0f);
    }

    ctx.camera().position = {0.0f, 4.5f, 13.0f};
    ctx.camera().pitch = -0.28f;
    ctx.select(crate);
}
