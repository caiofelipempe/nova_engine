#include "Scenarios.h"

#include <cmath>

using namespace nova;

// ------------------------------------------------------------- contacts --
std::vector<ContactMark>& contactMarks() {
    static std::vector<ContactMark> marks;
    return marks;
}
bool gRecordContacts = true;

void FlashOnHit::onCollision(const Collision& c) {
    flash = 1.0f;
    if (gRecordContacts && contactMarks().size() < 2000) contactMarks().push_back({c.point, c.normal, 0.0f});
}

void FlashOnHit::onUpdate(float dt) {
    flash = std::max(0.0f, flash - dt * 3.0f);
    if (auto* m = entity().tryGet<MeshComponent>()) {
        m->material.emissive = color;
        m->material.emissiveStrength = flash * 4.0f;
    }
}

namespace {

constexpr float kPi = 3.14159265f;
const glm::vec4 kPalette[] = {{0.9f, 0.3f, 0.25f, 1}, {0.25f, 0.6f, 0.95f, 1}, {0.95f, 0.8f, 0.2f, 1},
                              {0.4f, 0.85f, 0.4f, 1}, {0.75f, 0.4f, 0.9f, 1}, {0.95f, 0.55f, 0.2f, 1}};
const glm::vec4 kWood{0.55f, 0.38f, 0.2f, 1.0f};

// ---------------------------------------------------------------- scripts --
// Moves a kinematic body back and forth: base + amplitude · sin(2π t / period).
struct Oscillator : Script {
    GlobalPosition base;
    glm::vec3 amplitude{4.0f, 0.0f, 0.0f};
    float period = 8.0f;
    float t = 0.0f;
    void onCreate() override { base = transform().position; }
    void onUpdate(float dt) override {
        t += dt;
        transform().position = base + amplitude * std::sin(2.0f * kPi * t / period);
    }
};

// Flips a slider's position motor between two targets.
struct SliderPinger : Script {
    float every = 2.0f, t = 0.0f, a = 0.0f, b = 3.0f;
    void onUpdate(float dt) override {
        if ((t += dt) < every) return;
        t = 0.0f;
        auto& j = get<JointComponent>();
        j.motorTarget = toFloat(j.motorTarget) == a ? b : a;
    }
};

// Turns red and logs when its joint breaks.
struct BreakFlash : Script {
    void onJointBreak(Entity) override {
        NOVA_INFO("Joint of '{}' broke", entity().name());
        if (auto* m = entity().tryGet<MeshComponent>()) {
            m->material.emissive = {1.0f, 0.15f, 0.1f};
            m->material.emissiveStrength = 3.0f;
        }
    }
};

// A sensor that lights up what is inside it (green) and counts it.
struct TriggerZone : Script {
    int inside = 0;
    void onTriggerEnter(Entity other) override {
        ++inside;
        if (other.valid())
            if (auto* m = other.tryGet<MeshComponent>()) {
                m->material.emissive = {0.2f, 1.0f, 0.3f};
                m->material.emissiveStrength = 2.5f;
            }
    }
    void onTriggerExit(Entity other) override {
        inside = std::max(0, inside - 1);
        if (other.valid())
            if (auto* m = other.tryGet<MeshComponent>()) m->material.emissiveStrength = 0.0f;
    }
};

// Rolls its body back and forth along X (keeps the vertical motion).
struct Shuttle : Script {
    float speed = 5.0f, rate = 0.7f, phase = 0.0f, t = 0.0f;
    void onUpdate(float dt) override {
        t += dt;
        glm::vec3 v = scene().linearVelocity(entity()).toGlm();
        v.x = speed * (std::sin(t * rate + phase) >= 0.0f ? 1.0f : -1.0f);
        scene().setLinearVelocity(entity(), v);
    }
};

// Keeps its body moving along `direction`, turning to a new pseudo-random direction every so often
// (deterministic) without sudden turns (at most `steering` m/s²); keeps going through the seams.
struct Mover : Script {
    glm::vec3 direction{1.0f, 0.0f, 0.0f};
    float speed = 7.0f, kickEvery = 3.0f, sinceKick = 0.0f;
    float steering = 6.0f;
    unsigned seed = 1;
    void onUpdate(float dt) override {
        if ((sinceKick += dt) >= kickEvery) {
            sinceKick = 0.0f;
            seed = seed * 1664525u + 1013904223u;
            const float a = static_cast<float>(seed % 3600u) * 0.1f * kPi / 180.0f;
            direction = {std::cos(a), 0.0f, std::sin(a)};
        }
        const glm::vec3 v = scene().linearVelocity(entity()).toGlm();
        glm::vec3 change = direction * speed - glm::vec3(v.x, 0.0f, v.z);
        const float most = steering * dt, length = glm::length(change);
        if (length > most) change *= most / length;
        scene().setLinearVelocity(entity(), v + change);
    }
};

// Flashes (cyan) when the physics carries it across a seam of the world.
struct EdgeFlash : Script {
    float glow = 0.0f;
    int wraps = 0;
    void onWorldWrap(const WorldWrapHit&) override {
        glow = 1.0f;
        ++wraps;
    }
    void onUpdate(float dt) override {
        glow = std::max(0.0f, glow - dt * 2.0f);
        if (auto* m = entity().tryGet<MeshComponent>()) {
            m->material.emissive = glm::vec3(0.2f, 0.85f, 1.0f);
            m->material.emissiveStrength = glow * 6.0f;
        }
    }
};

// --------------------------------------------------------------- builders --
Entity visual(Scene& s, const std::string& name, MeshHandle mesh, const GlobalPosition& at, const glm::vec3& scale,
              const glm::vec4& color) {
    Entity e = s.createEntity(name);
    e.transform().position = at;
    e.transform().scale = scale;
    Material m;
    m.baseColor = color;
    m.roughness = 0.6f;
    e.add<MeshComponent>(mesh, m);
    return e;
}

// A body with a mesh; without collider components its shape is a box matching the scale.
Entity body(Scene& s, const std::string& name, MeshHandle mesh, const GlobalPosition& at, const glm::vec3& scale,
            const glm::vec4& color, physics::BodyType type = physics::BodyType::Dynamic) {
    Entity e = visual(s, name, mesh, at, scale, color);
    e.add<RigidBodyComponent>(type);
    return e;
}

Entity ball(Scene& s, const BenchMeshes& m, const std::string& name, const GlobalPosition& at, float diameter,
            const glm::vec4& color) {
    Entity e = body(s, name, m.sphere, at, glm::vec3(diameter), color);
    e.get<MeshComponent>().material.roughness = 0.25f;
    e.add<SphereColliderComponent>();
    return e;
}

Entity crate(Scene& s, const BenchMeshes& m, const std::string& name, const GlobalPosition& at, const glm::vec3& size,
             const glm::vec4& color = kWood) {
    Entity e = body(s, name, m.cube, at, size, color);
    e.addScript<FlashOnHit>();
    return e;
}

Entity floor(Scene& s, const BenchMeshes& m, float size, const glm::vec4& color = {0.8f, 0.8f, 0.8f, 1.0f}) {
    Entity e = visual(s, "Floor", m.plane, {0.0, 0.0, 0.0}, {size, 1.0f, size}, color);
    e.get<MeshComponent>().material.checker = true;
    e.add<RigidBodyComponent>(physics::BodyType::Static);
    e.add<BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f)); // top face at y = 0
    return e;
}

void sun(Scene& s) {
    Entity e = s.createEntity("Sun");
    e.add<DirectionalLightComponent>();
    e.transform().setEulerDegrees({-50.0f, 35.0f, 0.0f});
}

glm::quat around(float degrees, const glm::vec3& axis) { return glm::angleAxis(glm::radians(degrees), axis); }

// Stacks: a tower, a pyramid, dominoes and a kinematic sweeper among loose crates.
View buildStacks(Scene& s, const BenchMeshes& m) {
    sun(s);
    floor(s, m, 80.0f);
    for (int i = 0; i < 10; ++i)
        crate(s, m, "Tower " + std::to_string(i), {-9.0, 0.5 + i * 1.0, -3.0}, glm::vec3(1.0f));
    for (int row = 0; row < 7; ++row)
        for (int i = 0; i < 7 - row; ++i)
            crate(s, m, "Pyramid", {(i - (6 - row) * 0.5) * 1.02, 0.5 + row * 1.0, -5.0}, glm::vec3(1.0f),
                  kPalette[row % 6]);
    for (int i = 0; i < 20; ++i) {
        Entity d = crate(s, m, "Domino " + std::to_string(i), {4.0 + i * 1.1, 1.0, 4.0}, {0.2f, 2.0f, 1.0f},
                         {0.92f, 0.9f, 0.85f, 1.0f});
        if (i == 0) d.transform().rotation = around(-14.0f, {0, 0, 1}); // leans onto the next one
    }
    Entity sweeper = body(s, "Sweeper", m.cube, {-9.0, 0.35, 7.0}, {7.0f, 0.5f, 0.5f}, {0.3f, 0.3f, 0.35f, 1.0f},
                          physics::BodyType::Kinematic);
    sweeper.add<RotatorComponent>(glm::vec3(0, 1, 0), 0.7f);
    for (int i = 0; i < 8; ++i) {
        const float a = i * kPi / 4;
        crate(s, m, "Loose crate", {-9.0 + 2.2 * std::cos(a), 1.6, 7.0 + 2.2 * std::sin(a)}, glm::vec3(0.7f),
              kPalette[i % 6]);
    }
    return {{2.0, 9.0, 20.0}, {2.0, 1.5, 0.0}};
}

void statusStacks(Scene& s) {
    if (Entity top = s.findEntity("Tower 9"))
        ImGui::Text("Tower top: %.3f m (built at 9.5 m)", top.transform().position.toMeters().y);
}

// Materials: friction on a ramp, restitution of dropped balls.
constexpr float kFrictions[] = {0.02f, 0.1f, 0.2f, 0.3f, 0.6f};
constexpr float kRestitutions[] = {0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};

View buildMaterials(Scene& s, const BenchMeshes& m) {
    sun(s);
    floor(s, m, 80.0f).get<RigidBodyComponent>().restitution = 0.0f; // bounce = the ball's own restitution
    const glm::quat tilt = around(25.0f, {0, 0, 1}); // high at +X (tan 25° = 0.47)
    const GlobalPosition rampCenter{0.0, 2.6, -5.0};
    Entity ramp = body(s, "Ramp", m.cube, rampCenter, {14.0f, 0.4f, 9.0f}, {0.6f, 0.62f, 0.66f, 1.0f},
                       physics::BodyType::Static);
    ramp.transform().rotation = tilt;
    ramp.get<RigidBodyComponent>().friction = 1.0f; // contact friction = √(a·b): the box's value decides
    for (int i = 0; i < 5; ++i) {
        Entity b = crate(s, m, "Friction " + std::to_string(i), rampCenter + tilt * glm::vec3(5.0f, 0.71f, -3.6f + i * 1.8f),
                         glm::vec3(1.0f), glm::mix(glm::vec4(0.3f, 0.6f, 1.0f, 1), glm::vec4(1.0f, 0.35f, 0.2f, 1), i / 4.0f));
        b.transform().rotation = tilt;
        b.get<RigidBodyComponent>().friction = kFrictions[i];
    }
    for (int i = 0; i < 6; ++i) {
        Entity b = ball(s, m, "Bounce " + std::to_string(i), {-5.0 + i * 2.0, 6.0, 5.0}, 1.0f,
                        glm::mix(glm::vec4(0.3f, 0.3f, 0.35f, 1), glm::vec4(0.2f, 0.9f, 0.4f, 1), i / 5.0f));
        auto& rb = b.get<RigidBodyComponent>();
        rb.restitution = kRestitutions[i];
        rb.linearDamping = 0.0f;
    }
    return {{0.0, 7.0, 20.0}, {0.0, 2.0, 0.0}};
}

void statusMaterials(Scene& s) {
    ImGui::TextUnformatted("Ramp (25 deg, slides below 0.47), friction front to back:");
    for (int i = 0; i < 5; ++i)
        if (Entity b = s.findEntity("Friction " + std::to_string(i)))
            ImGui::Text("  %.2f -> speed %.2f m/s", kFrictions[i], glm::length(s.linearVelocity(b).toGlm()));
    ImGui::TextUnformatted("Balls, restitution left to right:");
    for (int i = 0; i < 6; ++i)
        if (Entity b = s.findEntity("Bounce " + std::to_string(i)))
            ImGui::Text("  %.1f -> height %.2f m", kRestitutions[i], b.transform().position.toMeters().y - 0.5);
}

// Shapes: every collider type falling on a triangle-mesh terrain.
View buildShapes(Scene& s, const BenchMeshes& m) {
    sun(s);
    floor(s, m, 80.0f);
    Entity ground = visual(s, "Terrain", m.terrain, {0.0, 0.0, 0.0}, glm::vec3(1.0f), {0.55f, 0.7f, 0.5f, 1.0f});
    ground.add<RigidBodyComponent>(physics::BodyType::Static);
    ground.add<MeshColliderComponent>(); // the exact triangles of its mesh
    for (int i = 0; i < 18; ++i) {
        const GlobalPosition at{-7.5 + (i % 6) * 3.0, 4.0 + (i / 6) * 2.5, -3.0 + (i / 6) * 3.0};
        const glm::vec4 color = kPalette[i % 6];
        Entity e;
        switch (i % 6) {
            case 0: e = crate(s, m, "Box", at, glm::vec3(1.0f), color); break;
            case 1:
                e = ball(s, m, "Sphere", at, 1.0f, color);
                e.addScript<FlashOnHit>();
                break;
            case 2:
                e = body(s, "Capsule", m.capsule, at, glm::vec3(1.0f), color);
                e.add<CapsuleColliderComponent>();
                e.addScript<FlashOnHit>();
                break;
            case 3:
                e = body(s, "Cylinder", m.cylinder, at, glm::vec3(1.0f), color);
                e.add<CylinderColliderComponent>();
                e.addScript<FlashOnHit>();
                break;
            case 4:
                e = body(s, "Convex hull", m.gem, at, glm::vec3(1.2f), color);
                e.add<ConvexHullColliderComponent>(); // hull of its own mesh
                e.addScript<FlashOnHit>();
                break;
            default:
                e = crate(s, m, "Plank", at, {2.0f, 0.2f, 0.6f}, color);
                e.get<RigidBodyComponent>().mass = 2.0f;
                break;
        }
        e.transform().rotation = around(25.0f + i * 37.0f, glm::normalize(glm::vec3(1.0f, 0.3f * (i % 3), 0.6f)));
    }
    return {{0.0, 9.0, 18.0}, {0.0, 1.0, 0.0}};
}

// Joints: hinge door with limits, motorized wheel, slider with a position motor, a chain of point
// joints, a spring rope and a breakable point joint.
View buildJoints(Scene& s, const BenchMeshes& m) {
    sun(s);
    floor(s, m, 80.0f);

    Entity door = crate(s, m, "Door", {-8.0, 1.4, 0.0}, {2.0f, 2.6f, 0.12f}, {0.75f, 0.55f, 0.35f, 1.0f});
    auto& hinge = door.add<JointComponent>();
    hinge.type = physics::JointType::Hinge;
    hinge.anchor = {-0.5f, 0.0f, 0.0f}; // left edge
    hinge.axis = {0, 1, 0};
    hinge.limitsEnabled = true;
    hinge.limitMin = -80.0f;
    hinge.limitMax = 80.0f;
    visual(s, "Door post", m.cube, {-9.05, 1.4, 0.0}, {0.1f, 2.8f, 0.2f}, {0.3f, 0.3f, 0.3f, 1.0f});

    Entity wheel = body(s, "Wheel", m.cylinder, {-3.5, 2.6, 0.0}, {2.0f, 0.3f, 2.0f}, {0.35f, 0.35f, 0.4f, 1.0f});
    wheel.transform().rotation = around(90.0f, {1, 0, 0}); // axis along Z
    wheel.add<CylinderColliderComponent>();
    auto& motor = wheel.add<JointComponent>();
    motor.type = physics::JointType::Hinge;
    motor.axis = {0, 1, 0}; // the cylinder's axis
    motor.motorMode = physics::JointMotorMode::Velocity;
    motor.motorTarget = 90.0f; // deg/s
    crate(s, m, "Rider", {-3.5, 4.2, 0.0}, glm::vec3(0.6f)); // something for it to fling

    Entity slider = crate(s, m, "Slider", {1.0, 0.8, -3.0}, glm::vec3(1.0f), {0.3f, 0.6f, 0.95f, 1.0f});
    slider.get<RigidBodyComponent>().gravityFactor = 0.0f;
    auto& rail = slider.add<JointComponent>();
    rail.type = physics::JointType::Slider;
    rail.axis = {1, 0, 0};
    rail.limitsEnabled = true;
    rail.limitMin = 0.0f;
    rail.limitMax = 3.0f;
    rail.motorMode = physics::JointMotorMode::Position;
    rail.motorTarget = 3.0f;
    slider.addScript<SliderPinger>();
    visual(s, "Rail", m.cube, {2.5, 0.25, -3.0}, {5.0f, 0.1f, 0.2f}, {0.3f, 0.3f, 0.3f, 1.0f});

    // A chain hanging from the world at x = 4, starting horizontal so it swings down.
    Entity previous;
    for (int i = 0; i < 8; ++i) {
        Entity link = crate(s, m, "Link " + std::to_string(i), {4.0 + 0.3 + i * 0.6, 6.0, 0.0}, {0.25f, 0.6f, 0.25f},
                            kPalette[i % 6]);
        link.transform().rotation = around(90.0f, {0, 0, 1}); // local +Y points back to the previous link
        auto& j = link.add<JointComponent>();
        j.type = physics::JointType::Point;
        j.anchor = {0.0f, 0.5f, 0.0f}; // local top, scaled: the shared end
        if (previous) j.connected = Scene::refOf(previous);
        previous = link;
    }
    visual(s, "Chain hook", m.cube, {4.0, 6.0, 0.0}, glm::vec3(0.2f), {0.2f, 0.2f, 0.2f, 1.0f});

    Entity bob = ball(s, m, "Rope bob", {12.0, 5.0, 3.0}, 0.8f, {0.95f, 0.8f, 0.2f, 1.0f});
    auto& rope = bob.add<JointComponent>();
    rope.type = physics::JointType::Distance;
    rope.connectedAnchor = {-3.0f, 2.0f, 0.0f}; // the world point (9, 7, 3)
    rope.springFrequency = 1.5f;
    rope.springDamping = 0.15f;
    visual(s, "Rope hook", m.cube, {9.0, 7.0, 3.0}, glm::vec3(0.2f), {0.2f, 0.2f, 0.2f, 1.0f});

    Entity hanging = crate(s, m, "Breakable", {0.0, 4.0, 3.0}, glm::vec3(1.0f), {0.8f, 0.8f, 0.85f, 1.0f});
    hanging.get<RigidBodyComponent>().mass = 10.0f; // weighs 98 N
    auto& weak = hanging.add<JointComponent>();
    weak.type = physics::JointType::Point;
    weak.anchor = {0.0f, 0.5f, 0.0f};
    weak.breakForce = 400.0f; // holds its weight, not a shot
    hanging.addScript<BreakFlash>();
    visual(s, "Breakable hook", m.cube, {0.0, 4.55, 3.0}, glm::vec3(0.15f), {0.2f, 0.2f, 0.2f, 1.0f});
    return {{1.0, 5.0, 16.0}, {1.0, 2.5, 0.0}};
}

void statusJoints(Scene& s) {
    auto* world = s.physics();
    if (!world) return;
    auto jointOf = [&](const char* name) -> JointComponent* {
        Entity e = s.findEntity(name);
        return e ? e.tryGet<JointComponent>() : nullptr;
    };
    if (auto* j = jointOf("Door"); j && j->joint) ImGui::Text("Door angle: %.1f deg (limit 80)", glm::degrees(world->jointPosition(j->joint)));
    if (auto* j = jointOf("Slider"); j && j->joint) ImGui::Text("Slider: %.2f m -> target %.0f m", world->jointPosition(j->joint), toFloat(j->motorTarget));
    if (auto* j = jointOf("Rope bob"); j && j->joint) ImGui::Text("Rope length: %.2f m", world->jointPosition(j->joint));
    if (auto* j = jointOf("Breakable")) ImGui::Text("Breakable joint: %s", j->broken ? "BROKEN" : "holding (shoot it)");
}

// CCD: bullets at 150 m/s against a 5 cm wall, with and without continuous collision.
View buildCcd(Scene& s, const BenchMeshes& m) {
    sun(s);
    floor(s, m, 120.0f);
    body(s, "Thin wall", m.cube, {0.0, 2.0, 0.0}, {0.05f, 4.0f, 8.0f}, {0.85f, 0.85f, 0.9f, 1.0f}, physics::BodyType::Static);
    visual(s, "Lane (discrete)", m.cube, {-10.0, 0.01, -1.5}, {20.0f, 0.02f, 0.4f}, {0.9f, 0.3f, 0.25f, 1.0f});
    visual(s, "Lane (CCD)", m.cube, {-10.0, 0.01, 1.5}, {20.0f, 0.02f, 0.4f}, {0.3f, 0.85f, 0.4f, 1.0f});
    return {{-6.0, 4.0, 10.0}, {0.0, 1.5, 0.0}};
}

void fireCcd(Scene& s, const BenchMeshes& m, std::vector<Entity>& spawned) {
    for (int lane = 0; lane < 2; ++lane) {
        const bool ccd = lane == 1;
        Entity b = ball(s, m, ccd ? "Bullet (CCD)" : "Bullet", {-20.0, 2.0, ccd ? 1.5 : -1.5}, 0.2f,
                        ccd ? glm::vec4(0.3f, 0.9f, 0.4f, 1) : glm::vec4(0.95f, 0.3f, 0.25f, 1));
        auto& rb = b.get<RigidBodyComponent>();
        rb.mass = 0.5f;
        rb.gravityFactor = 0.0f;
        rb.linearDamping = 0.0f;
        rb.restitution = 0.0f;
        rb.continuousCollision = ccd;
        b.addScript<FlashOnHit>();
        s.setLinearVelocity(b, glm::vec3(150.0f, 0.0f, 0.0f)); // creates the body now
        spawned.push_back(b);
    }
}

void statusCcd(Scene& s) {
    int passed[2] = {0, 0}, stopped[2] = {0, 0};
    for (auto [e, name, t] : s.registry().view<NameComponent, TransformComponent>().each()) {
        const int lane = name.name == "Bullet (CCD)" ? 1 : name.name == "Bullet" ? 0 : -1;
        if (lane < 0) continue;
        (t.position.toMeters().x > 0.1 ? passed : stopped)[lane] += 1;
    }
    ImGui::Text("Discrete: %d stopped, %d tunneled through", stopped[0], passed[0]);
    ImGui::Text("CCD:      %d stopped, %d tunneled through", stopped[1], passed[1]);
}

// Character: stairs, a ledge, walkable and steep slopes, moving platforms and crates to push.
View buildCharacter(Scene& s, const BenchMeshes& m) {
    sun(s);
    floor(s, m, 80.0f);
    const glm::vec4 stone{0.62f, 0.62f, 0.66f, 1.0f};
    for (int i = 0; i < 6; ++i) // 0.2 m steps (walkable: stepHeight 0.3)
        body(s, "Stair", m.cube, {-7.0, 0.1 * (i + 1), -1.0 - i}, {3.0f, 0.2f * (i + 1), 1.0f}, stone, physics::BodyType::Static);
    body(s, "Ledge 0.5 m", m.cube, {-12.0, 0.25, -2.0}, {3.0f, 0.5f, 3.0f}, {0.75f, 0.5f, 0.45f, 1.0f}, physics::BodyType::Static);
    auto ramp = [&](const char* name, float degrees, float length, double x, const glm::vec4& color) {
        const float a = glm::radians(degrees);
        Entity r = body(s, name, m.cube, {x, 0.5 * length * std::sin(a), -2.0 - 0.5 * length * std::cos(a)},
                        {3.0f, 0.2f, length}, color, physics::BodyType::Static);
        r.transform().rotation = around(degrees, {1, 0, 0}); // low end towards +Z
    };
    ramp("Slope 30 deg", 30.0f, 8.0f, 3.0, {0.5f, 0.75f, 0.5f, 1.0f});
    ramp("Slope 60 deg", 60.0f, 6.0f, 8.0, {0.85f, 0.45f, 0.4f, 1.0f});

    Entity platform = body(s, "Moving platform", m.cube, {8.0, 0.15, 6.0}, {3.0f, 0.3f, 3.0f}, {0.95f, 0.8f, 0.3f, 1.0f},
                           physics::BodyType::Kinematic);
    platform.addScript<Oscillator>();
    Entity disk = body(s, "Turntable", m.cylinder, {-7.0, 0.15, 8.0}, {5.0f, 0.3f, 5.0f}, {0.4f, 0.6f, 0.9f, 1.0f},
                       physics::BodyType::Kinematic);
    disk.add<CylinderColliderComponent>();
    disk.add<RotatorComponent>(glm::vec3(0, 1, 0), 0.6f);
    for (int i = 0; i < 4; ++i)
        crate(s, m, "Pushable", {1.0 + i * 1.5, 0.5, 2.0}, glm::vec3(1.0f)).get<RigidBodyComponent>().mass = 8.0f;

    Entity player = visual(s, "Player", m.player, {0.0, 0.05, 8.0}, glm::vec3(1.0f), {0.2f, 0.5f, 1.0f, 1.0f});
    player.add<CharacterControllerComponent>();
    return {{0.0, 4.0, 15.0}, {0.0, 1.0, 8.0}};
}

void statusCharacter(Scene& s) {
    Entity p = s.findEntity("Player");
    if (!p) return;
    const auto& cc = p.get<CharacterControllerComponent>();
    const char* states[] = {"on ground", "on steep ground", "not supported", "in air"};
    const int g = static_cast<int>(cc.groundState);
    ImGui::Text("Character: %s, speed %.2f m/s", g >= 0 && g < 4 ? states[g] : "?", glm::length(cc.velocity.toGlm()));
    if (Entity ground = s.resolve(cc.ground)) ImGui::Text("Standing on: %s", ground.name().c_str());
}

// Sensors and layers: a trigger zone, and a shelf that one layer falls through.
View buildSensorsLayers(Scene& s, const BenchMeshes& m) {
    PhysicsSettings settings;
    settings.layerNames[1] = "Ghosts";
    settings.layerNames[2] = "Shelf";
    settings.layers.set(1, 2, false); // ghosts pass through the shelf (and still land on the floor)
    s.setPhysicsSettings(settings);
    sun(s);
    floor(s, m, 80.0f);

    Entity zone = s.createEntity("Trigger zone");
    zone.transform().position = {-6.0, 1.5, 0.0};
    zone.transform().scale = {6.0f, 3.0f, 6.0f};
    auto& rb = zone.add<RigidBodyComponent>(physics::BodyType::Static);
    rb.isSensor = true;
    zone.addScript<TriggerZone>();
    visual(s, "Zone marker", m.plane, {-6.0, 0.01, 0.0}, {6.0f, 1.0f, 6.0f}, {0.2f, 0.75f, 0.35f, 1.0f});
    for (int i = 0; i < 4; ++i) {
        Entity b = ball(s, m, "Shuttle", {-12.0 + i * 1.5, 0.5, -2.0 + i * 1.3}, 1.0f, {0.85f, 0.85f, 0.9f, 1.0f});
        auto& sh = b.addScript<Shuttle>();
        sh.phase = static_cast<float>(i) * 0.8f;
    }

    Entity shelf = body(s, "Shelf (layer Shelf)", m.cube, {6.0, 3.0, 0.0}, {6.0f, 0.3f, 6.0f}, {0.3f, 0.5f, 0.95f, 1.0f},
                        physics::BodyType::Static);
    shelf.get<RigidBodyComponent>().layer = 2;
    return {{0.0, 8.0, 16.0}, {0.0, 1.5, 0.0}};
}

void dropOnShelf(Scene& s, const BenchMeshes& m, std::vector<Entity>& spawned) {
    for (int i = 0; i < 4; ++i) {
        const bool ghost = (i % 2) == 0;
        Entity c = crate(s, m, ghost ? "Ghost (layer Ghosts)" : "Crate (layer Default)",
                         {4.5 + (i % 2) * 2.5, 6.0 + i * 1.2, -1.5 + (i / 2) * 3.0}, glm::vec3(0.8f),
                         ghost ? glm::vec4(0.95f, 0.3f, 0.3f, 1.0f) : glm::vec4(0.92f, 0.92f, 0.92f, 1.0f));
        if (ghost) c.get<RigidBodyComponent>().layer = 1;
        spawned.push_back(c);
    }
}

void statusSensorsLayers(Scene& s) {
    if (Entity z = s.findEntity("Trigger zone"))
        if (auto* sc = z.tryGet<ScriptComponent>(); sc && !sc->scripts.empty())
            ImGui::Text("Inside the trigger zone: %d", static_cast<TriggerZone&>(*sc->scripts[0]).inside);
    ImGui::TextUnformatted("Red = layer Ghosts (ignores Shelf), white = Default");
}

// Gamepads: one ball per connected gamepad (spawned by the bench), plus things to roll into.
View buildPlayers(Scene& s, const BenchMeshes& m) {
    sun(s);
    floor(s, m, 60.0f);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 3; ++x) crate(s, m, "Crate", {3.0 + x * 1.02, 0.5 + y * 1.0, -4.0}, glm::vec3(1.0f));
    Entity ramp = body(s, "Ramp", m.cube, {-5.0, 0.6, -3.0}, {4.0f, 0.2f, 6.0f}, {0.6f, 0.62f, 0.66f, 1.0f},
                       physics::BodyType::Static);
    ramp.transform().rotation = around(12.0f, {1, 0, 0});
    return {{0.0, 4.0, 10.0}, {0.0, 0.5, 0.0}};
}

// A floor of `count` × `count` tiles of `tile` meters centered on the origin (top face at y = 0):
// ONE static body under them (where tiles meet there is no edge to bump into; in a small world
// the physics gives it copies one lap over, in the same body, so it surrounds the focus).
void tiledFloor(Scene& s, const BenchMeshes& m, int count, float tile, bool visible, const glm::vec4& color) {
    Entity floor = s.createEntity("Floor");
    floor.transform().scale = glm::vec3(count * tile, 1.0f, count * tile);
    floor.add<RigidBodyComponent>(physics::BodyType::Static);
    floor.add<BoxColliderComponent>(glm::vec3(0.5f, 0.05f, 0.5f), glm::vec3(0.0f, -0.05f, 0.0f));
    if (!visible) return;
    for (int x = 0; x < count; ++x)
        for (int z = 0; z < count; ++z) {
            const double cx = (x - (count - 1) * 0.5) * tile, cz = (z - (count - 1) * 0.5) * tile;
            const float shade = ((x + z) % 2) ? 0.92f : 1.0f;
            Entity t = s.createEntity("Floor tile");
            t.transform().position = {cx, 0.0, cz};
            t.transform().scale = glm::vec3(tile, 1.0f, tile);
            auto& mc = t.add<MeshComponent>(m.plane);
            mc.material.baseColor = glm::vec4(glm::vec3(color) * shade, 1.0f);
            mc.material.checker = true;
        }
}

// Balls that wander and flash at the seams, and crates to knock across them.
void wanderers(Scene& s, const BenchMeshes& m, int balls, double spread) {
    for (int i = 0; i < balls; ++i) {
        const double a = i * (6.2831853 / balls), r = spread * (0.3 + (i % 3) * 0.25);
        Entity b = ball(s, m, "Ball " + std::to_string(i), {r * std::cos(a), 1.0 + (i % 4), r * std::sin(a)}, 1.0f,
                        kPalette[i % 6]);
        auto& rb = b.get<RigidBodyComponent>();
        rb.restitution = 0.6f;
        rb.friction = 0.2f;
        rb.linearDamping = 0.0f;
        b.addScript<EdgeFlash>();
        auto& mover = b.addScript<Mover>();
        mover.speed = 7.0f + (i % 4);
        mover.kickEvery = 2.5f + (i % 5) * 0.7f;
        mover.seed = 17u + static_cast<unsigned>(i) * 31u;
        mover.direction = {static_cast<float>(std::cos(a)), 0.0f, static_cast<float>(std::sin(a))};
    }
}

// World edges: a 60 × 60 m world whose X and Z axes wrap around (Y keeps the default size).
View buildEdges(Scene& s, const BenchMeshes& m) {
    constexpr double kSize = 60.0;
    s.setWorldSize(WorldSize::meters(kSize, 0.0, kSize));
    sun(s);
    tiledFloor(s, m, 6, 10.0f, true, {0.74f, 0.79f, 0.82f, 1});
    for (int i = 0; i < 4; ++i)
        visual(s, "Corner", m.cube, {(i & 1) ? kSize / 2 - 0.2 : -kSize / 2 + 0.2, 1.5, (i & 2) ? kSize / 2 - 0.2 : -kSize / 2 + 0.2},
               {0.3f, 3.0f, 0.3f}, {0.2f, 0.75f, 0.95f, 1});
    wanderers(s, m, 12, 20.0);
    for (int i = 0; i < 8; ++i) {
        Entity c = body(s, "Crate", m.cube, {-12.0 + i * 3.4, 0.6, -4.0 + (i % 2) * 8.0}, glm::vec3(1.2f), kWood);
        c.get<RigidBodyComponent>().mass = 3.0f;
        c.addScript<EdgeFlash>();
    }
    return {{0.0, 22.0, 40.0}, {0.0, 0.0, 0.0}};
}

// A tiny circular world, 48 m on every axis (Y too: what falls below comes in from above), to see
// the circular world. The floor you see is ONE mesh of 48 × 48 m, placed across the X seam: drawn
// per object it shows on one side only, per triangle or split (NOVA_WRAP_RENDER_MODES builds) it
// covers the world; with further laps the world repeats up to the horizon. The bodies stand on an
// invisible floor body (the physics gives it copies one lap over, in the same body).
constexpr double kTinySize = 48.0;

View buildTiny(Scene& s, const BenchMeshes& m) {
    s.setWorldSize(WorldSize::cube(kTinySize));
    sun(s);
    Entity floor = visual(s, "Floor (one mesh)", m.grid, {12.0, 0.0, 0.0},
                          {static_cast<float>(kTinySize), 1.0f, static_cast<float>(kTinySize)}, {0.78f, 0.8f, 0.84f, 1});
    floor.get<MeshComponent>().material.checker = true;
    tiledFloor(s, m, 6, 8.0f, false, {});
    // Pillars on both sides of the seams: red just before +X, blue just after it (at −X), yellow
    // and green on the Z seam. With the world repeating they line up lap after lap.
    const double e = kTinySize / 2 - 1.0;
    visual(s, "Pillar +X", m.cube, {e, 2.0, -6.0}, {0.8f, 4.0f, 0.8f}, {0.9f, 0.2f, 0.2f, 1});
    visual(s, "Pillar -X", m.cube, {-e, 2.0, -6.0}, {0.8f, 4.0f, 0.8f}, {0.2f, 0.4f, 0.95f, 1});
    visual(s, "Pillar +Z", m.cube, {6.0, 2.0, e}, {0.8f, 4.0f, 0.8f}, {0.95f, 0.85f, 0.2f, 1});
    visual(s, "Pillar -Z", m.cube, {6.0, 2.0, -e}, {0.8f, 4.0f, 0.8f}, {0.3f, 0.85f, 0.4f, 1});
    // A long bar across the X seam (one cube, 10 m): per object it jumps from side to side as the
    // camera moves; per triangle its big faces go whole to one side or the other (the cut follows
    // its 12 triangles); split cuts it exactly at the seam.
    visual(s, "Bar across the seam", m.cube, {kTinySize / 2, 0.4, 8.0}, {10.0f, 0.8f, 0.8f}, {0.95f, 0.5f, 0.15f, 1});
    // A stack of crates right on the Z seam: it stands on tiles of both sides, and its crates
    // touch across the seam.
    for (int i = 0; i < 4; ++i) {
        Entity c = body(s, "Seam crate", m.cube, {-8.0 + (i % 2) * 0.4, 0.5 + i * 1.0, kTinySize / 2}, glm::vec3(1.0f), kWood);
        c.addScript<EdgeFlash>();
    }
    wanderers(s, m, 8, 14.0);
    return {{0.0, 16.0, 26.0}, {12.0, 0.0, 0.0}};
}

void statusTiny(Scene& s) {
    const glm::dvec3 o = s.worldOrigin().toMeters();
    ImGui::Text("World %.0f m per axis, circular | origin %.1f, %.1f, %.1f", kTinySize, o.x, o.y, o.z);
    int wraps = 0;
    for (auto [e, sc] : s.registry().view<ScriptComponent>().each())
        for (auto& script : sc.scripts)
            if (auto* f = dynamic_cast<EdgeFlash*>(script.get())) wraps += f->wraps;
    ImGui::Text("Seams crossed: %d", wraps);
#if NOVA_WRAP_RENDER_MODES
    RenderSettings& rs = Application::get().renderer().settings();
    int mode = static_cast<int>(rs.wrapShift);
    const char* modes[] = {"Per triangle", "Per object", "Split at the seam"};
    if (ImGui::Combo("Across a seam", &mode, modes, 3)) rs.wrapShift = static_cast<WrapShift>(mode);
    int laps = static_cast<int>(rs.wrapRepeat);
    if (ImGui::SliderInt("Laps around", &laps, 0, 6)) rs.wrapRepeat = static_cast<u32>(laps);
    const RenderStats& st = Application::get().renderer().stats();
    ImGui::Text("%u instances, %u of them extra copies", st.instances, st.wrapCopies);
#else
    ImGui::TextDisabled("Build with -DNOVA_WRAP_RENDER_MODES=ON (preset windows-vs2022-wrap)");
    ImGui::TextDisabled("to draw across the seams and repeat the world.");
#endif
}

} // namespace

const std::vector<Scenario>& scenarios() {
    static const std::vector<Scenario> list = [] {
        std::vector<Scenario> v;
        v.push_back({"Stacks", "Tower, pyramid, dominoes and a kinematic sweeper. Shoot them; watch stability.",
                     buildStacks, nullptr, nullptr, 0.0f, statusStacks});
        v.push_back({"Materials", "Friction on the ramp (front: none), restitution of the balls (left: none).",
                     buildMaterials, nullptr, nullptr, 0.0f, statusMaterials});
        v.push_back({"Shapes", "Box, sphere, capsule, cylinder, convex hull and planks on a triangle-mesh terrain.",
                     buildShapes});
        v.push_back({"Joints", "Hinge door with limits, motor wheel, slider motor, chain, spring rope, breakable joint.",
                     buildJoints, nullptr, nullptr, 0.0f, statusJoints});
        v.push_back({"CCD", "150 m/s bullets on a 5 cm wall: red lane discrete, green lane continuous collision.",
                     buildCcd, fireCcd, "Fire bullets", 1.5f, statusCcd});
        v.push_back({"Character", "WASD walk, Shift run, Space jump, RMB orbit. Stairs, ledge, slopes, platforms, crates.",
                     buildCharacter, nullptr, nullptr, 0.0f, statusCharacter, true});
        v.push_back({"Sensors & layers", "Balls turn green inside the trigger. Red boxes ignore the blue shelf's layer.",
                     buildSensorsLayers, dropOnShelf, "Drop boxes", 0.0f, statusSensorsLayers});
        v.push_back({"Gamepads", "Each connected gamepad gets a ball: left stick rolls, A jumps.", buildPlayers,
                     nullptr, nullptr, 0.0f, nullptr, false, true});
        v.push_back({"Edges: wrap", "60 x 60 m world that wraps around: balls flash cyan and come in the other side.",
                     buildEdges});
        v.push_back({"Tiny circular world", "48 m on every axis: one floor mesh across the seam, pillars on both sides, a crate stack on the Z seam.",
                     buildTiny, nullptr, nullptr, 0.0f, statusTiny});
        return v;
    }();
    return list;
}
