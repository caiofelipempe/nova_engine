// Physics features through the Scene (frontend): contact lifecycle, triggers, layers, overlaps...
// Headless (no window / GPU). Run: ctest --preset ... or NovaPhysicsFeatureTests.exe
#include <nova/Nova.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::physics;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAILED: %s (%s:%d)\n", expr, file, line);
    }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)
void checkNear(double a, double b, double eps, const char* expr, const char* file, int line) {
    check(std::abs(a - b) <= eps, expr, file, line);
    if (std::abs(a - b) > eps) std::printf("    got %.6f, expected %.6f (+-%g)\n", a, b, eps);
}
#define CHECK_NEAR(a, b, eps) checkNear(static_cast<double>(a), static_cast<double>(b), static_cast<double>(eps), #a " ~= " #b, __FILE__, __LINE__)
#define CHECK_VEC(a, b, eps) check(glm::length((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)

const Duration kFrame = Duration::perSecond(60);

void run(Scene& s, int frames) {
    for (int i = 0; i < frames; ++i) s.update(kFrame);
}

// Everything a script can hear about contacts, in order.
struct Recorder : Script {
    struct Entry {
        std::string what;
        Entity other;
    };
    std::vector<Entry> log;
    int count(const std::string& what) const {
        int n = 0;
        for (const Entry& e : log) n += e.what == what;
        return n;
    }
    void onCollision(const Collision& c) override { log.push_back({"begin", c.other}); }
    void onCollisionEnd(Entity other) override { log.push_back({"end", other}); }
    void onTriggerEnter(Entity other) override { log.push_back({"enter", other}); }
    void onTriggerExit(Entity other) override { log.push_back({"exit", other}); }
};

Entity addGround(Scene& s, const GlobalPosition& top = {}) {
    Entity g = s.createEntity("Ground");
    g.transform().position = top + glm::vec3(0, -0.5f, 0);
    g.transform().scale = {100, 1, 100};
    g.add<RigidBodyComponent>(BodyType::Static).restitution = 0.0f; // no bounces: clean touches
    return g;
}

Entity addBall(Scene& s, const GlobalPosition& p, f32 radius = 0.5f) {
    Entity b = s.createEntity("Ball");
    b.transform().position = p;
    auto& rb = b.add<RigidBodyComponent>();
    rb.restitution = 0.0f;
    b.add<SphereColliderComponent>().radius = radius;
    return b;
}

// ------------------------------------------------------------- contacts --
void contactBeginsAndEnds() {
    Scene scene("contacts");
    Entity ground = addGround(scene);
    Entity ball = addBall(scene, {0.0, 2.0, 0.0});
    auto& rb = ball.addScript<Recorder>();
    auto& rg = ground.addScript<Recorder>();
    scene.startSimulation();
    run(scene, 120); // lands, comes to rest, falls asleep: still one touch
    CHECK(!scene.physics()->isActive(ball.get<RigidBodyComponent>().body));
    CHECK(rb.count("begin") == 1 && rb.count("end") == 0);
    CHECK(rg.count("begin") == 1 && rg.count("end") == 0);
    // Thrown up: the touch ends, once, on both sides.
    scene.setLinearVelocity(ball, {0, 8, 0});
    run(scene, 20);
    CHECK(rb.count("end") == 1 && rg.count("end") == 1);
    if (rb.count("end") == 1) CHECK(rb.log.back().what == "end" && rb.log.back().other == ground);
    // ...and lands again: a new begin.
    run(scene, 180);
    CHECK(rb.count("begin") == 2);
}

void meshTouchIsOne() {
    // A ball rolling over a ground mesh crosses many triangles: still one continuous touch.
    MeshData plane = MeshData::plane(40.0f, 8); // 128 triangles
    Scene scene("mesh");
    scene.setMeshSource([&](MeshHandle h) -> const MeshData* { return h.id == 1 ? &plane : nullptr; });
    Entity ground = scene.createEntity("Ground");
    ground.add<MeshComponent>(MeshHandle{1});
    ground.add<RigidBodyComponent>(BodyType::Static);
    ground.add<MeshColliderComponent>();
    Entity ball = addBall(scene, {-8.0, 0.6, 0.0});
    auto& rec = ball.addScript<Recorder>();
    scene.startSimulation();
    run(scene, 30);
    scene.setLinearVelocity(ball, {6, 0, 0});
    run(scene, 150);
    // Rolled across triangle edges (x = −5, 0) without hopping on them (internal edge removal).
    CHECK(ball.transform().position.toMeters().x > 0.0);
    CHECK(rec.count("begin") == 1);
    CHECK(rec.count("end") == 0);
}

void destroyingEndsTouches() {
    Scene scene("destroy");
    Entity ground = addGround(scene);
    Entity ball = addBall(scene, {0.0, 0.5, 0.0});
    auto& rg = ground.addScript<Recorder>();
    scene.startSimulation();
    run(scene, 30);
    CHECK(rg.count("begin") == 1);
    scene.destroyEntity(ball);
    run(scene, 2);
    CHECK(rg.count("end") == 1);
    if (rg.count("end") == 1) CHECK(!rg.log.back().other.valid()); // the other one is gone
}

void dormancyEndsTouches() {
    Scene scene("dormant");
    Entity ground = addGround(scene);
    Entity ball = addBall(scene, {0.0, 0.5, 0.0});
    auto& rb = ball.addScript<Recorder>();
    scene.setPhysicsFocus({0.0, 0.0, 0.0});
    scene.physicsRegionRadius = 100.0;
    scene.startSimulation();
    run(scene, 30);
    CHECK(rb.count("begin") == 1);
    scene.setPhysicsFocus({50'000.0, 0.0, 0.0}); // far away: the ball stops simulating
    run(scene, 2);
    CHECK(scene.isPhysicsDormant(ball));
    CHECK(rb.count("end") == 1);
}

void triggers() {
    Scene scene("triggers");
    addGround(scene);
    Entity zone = scene.createEntity("Zone");
    zone.transform().position = {0.0, 3.0, 0.0};
    zone.transform().scale = {2, 2, 2};
    auto& zrb = zone.add<RigidBodyComponent>(BodyType::Static);
    zrb.isSensor = true;
    Entity ball = addBall(scene, {0.0, 6.0, 0.0}, 0.25f);
    auto& rz = zone.addScript<Recorder>();
    auto& rb = ball.addScript<Recorder>();
    scene.startSimulation();
    run(scene, 120); // falls through the zone onto the ground
    CHECK(rz.count("enter") == 1 && rz.count("exit") == 1);
    CHECK(rb.count("enter") == 1 && rb.count("exit") == 1);
    if (!rz.log.empty()) CHECK(rz.log.front().other == ball);
    CHECK(rz.count("begin") == 0); // a sensor never collides
    CHECK(rb.count("begin") == 1);  // ...only the ground does
    CHECK_NEAR(ball.transform().position.toMeters().y, 0.25, 0.03);
}

// --------------------------------------------------------------- layers --
void collisionLayers() {
    Scene scene("layers");
    PhysicsSettings settings;
    settings.layerNames[1] = "Player";
    settings.layerNames[2] = "Debris";
    settings.layers.set(1, 2, false);
    scene.setPhysicsSettings(settings);
    CHECK(settings.findLayer("Debris") == 2 && settings.layerName(0) == "Default");
    CHECK(settings.maskOf({"Player", "Debris"}) == (layerBit(1) | layerBit(2)));

    addGround(scene);
    Entity debris = scene.createEntity("Debris");
    debris.transform().position = {0.0, 1.0, 0.0};
    debris.add<RigidBodyComponent>(BodyType::Static).layer = 2;
    Entity player = addBall(scene, {0.0, 4.0, 0.0});
    player.get<RigidBodyComponent>().layer = 1;
    Entity crate = addBall(scene, {3.0, 4.0, 0.0}); // default layer: lands on debris-like boxes as usual
    Entity block = scene.createEntity("Block");
    block.transform().position = {3.0, 1.0, 0.0};
    block.add<RigidBodyComponent>(BodyType::Static).layer = 2;
    scene.startSimulation();
    run(scene, 120);
    CHECK_NEAR(player.transform().position.toMeters().y, 0.5, 0.03); // through the debris, onto the ground
    CHECK_NEAR(crate.transform().position.toMeters().y, 2.0, 0.03);  // on top of the block

    // Queries filter by layer: a ray down through the debris finds the ground only.
    auto hit = scene.raycast(GlobalPosition(0.0, 5.0, 0.0), {0, -1, 0}, 10.0f, player,
                             physics::kAllLayers & ~layerBit(2));
    CHECK(hit && hit->entity.name() == "Ground");
    auto all = scene.raycast(GlobalPosition(0.0, 5.0, 0.0), {0, -1, 0}, 10.0f, player);
    CHECK(all && all->entity == debris);

    // At runtime: debris and player collide again; a new player stops on the debris.
    settings.layers.set(1, 2, true);
    scene.setPhysicsSettings(settings);
    Entity second = addBall(scene, {0.0, 4.0, 0.0});
    second.get<RigidBodyComponent>().layer = 1;
    run(scene, 120);
    CHECK_NEAR(second.transform().position.toMeters().y, 2.0, 0.03);
}

// -------------------------------------------------------------- overlap --
void overlaps() {
    Scene scene("overlap");
    addGround(scene);
    Entity a = addBall(scene, {0.0, 0.5, 0.0});
    Entity b = addBall(scene, {1.5, 0.5, 0.0});
    Entity far = addBall(scene, {10.0, 0.5, 0.0});
    Entity zone = scene.createEntity("Zone");
    zone.transform().position = {0.5, 0.5, 0.0};
    zone.add<RigidBodyComponent>(BodyType::Static).isSensor = true;
    b.get<RigidBodyComponent>().layer = 3;
    scene.startSimulation();
    run(scene, 5);

    auto contains = [](const std::vector<Entity>& v, Entity e) { return std::find(v.begin(), v.end(), e) != v.end(); };
    std::vector<Entity> found = scene.overlapSphere(GlobalPosition(0.7, 0.5, 0.0), 1.0f);
    CHECK(contains(found, a) && contains(found, b) && !contains(found, far));
    CHECK(!contains(found, zone));                          // sensors only on request
    CHECK(contains(found, scene.findEntity("Ground")));     // the ground touches it too
    found = scene.overlapSphere(GlobalPosition(0.7, 0.5, 0.0), 1.0f, a, kAllLayers, true);
    CHECK(!contains(found, a) && contains(found, zone));    // ignore + sensors
    found = scene.overlapBox(GlobalPosition(0.7, 0.5, 0.0), glm::vec3(1.0f, 0.4f, 0.4f), glm::quat(1, 0, 0, 0), {},
                             layerBit(3));
    CHECK(found.size() == 1 && found.front() == b);         // only layer 3
    CHECK(scene.overlapSphere(GlobalPosition(30.0, 5.0, 0.0), 1.0f).empty());
}

// --------------------------------------------------------------- saving --
void settingsAreSaved() {
    Scene scene("saved");
    PhysicsSettings s;
    s.gravity = {0.0f, -3.7f, 0.0f};
    s.layerNames[4] = "Projectiles";
    s.layers.set(4, 4, false);
    scene.setPhysicsSettings(s);
    Entity e = scene.createEntity("Bullet");
    e.add<RigidBodyComponent>().layer = 4;
    const std::string json = serializeScene(scene);
    Scene loaded("loaded");
    CHECK(deserializeScene(loaded, json));
    CHECK(loaded.physicsSettings() == s);
    CHECK(loaded.findEntity("Bullet").get<RigidBodyComponent>().layer == 4);
    // Gravity reaches the simulation.
    Entity ball = addBall(loaded, {0.0, 10.0, 0.0});
    ball.get<RigidBodyComponent>().linearDamping = 0.0f;
    loaded.startSimulation();
    run(loaded, 60);
    CHECK_NEAR(loaded.linearVelocity(ball).y, -3.7f, 0.01f);
    // Old files (no "physics") get the defaults.
    Scene plain("plain");
    CHECK(deserializeScene(plain, R"({"format": "nova.scene", "version": 1, "entities": []})"));
    CHECK(plain.physicsSettings() == PhysicsSettings{});
}

} // namespace

int main() {
    Log::init();
    Log::core().set_level(spdlog::level::warn);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"contacts: begin and end, once each", contactBeginsAndEnds},
        {"contacts: rolling across mesh triangles is one touch", meshTouchIsOne},
        {"contacts: destroying a body ends its touches", destroyingEndsTouches},
        {"contacts: going dormant ends touches", dormancyEndsTouches},
        {"triggers: enter and exit, no collision", triggers},
        {"collision layers: matrix, queries, runtime changes", collisionLayers},
        {"overlap queries", overlaps},
        {"physics settings are saved with the scene", settingsAreSaved},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    Log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
