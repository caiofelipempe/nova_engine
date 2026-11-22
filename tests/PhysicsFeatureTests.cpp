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

void destroyingSupportWakes() {
    // A crate asleep on a platform falls when the platform is destroyed.
    Scene scene("support");
    addGround(scene);
    Entity platform = scene.createEntity("Platform");
    platform.transform().position = {0.0, 3.0, 0.0};
    platform.transform().scale = {3, 0.2f, 3};
    platform.add<RigidBodyComponent>(BodyType::Static);
    Entity crate = addBall(scene, {0.0, 3.6, 0.0});
    scene.startSimulation();
    run(scene, 90);
    CHECK(!scene.physics()->isActive(crate.get<RigidBodyComponent>().body)); // asleep up there
    scene.destroyEntity(platform);
    run(scene, 90);
    CHECK_NEAR(crate.transform().position.toMeters().y, 0.5, 0.03);
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

// ---------------------------------------------------------------- bodies --
void runtimeProperties() {
    Scene scene("runtime");
    Entity ground = addGround(scene);
    Entity ball = addBall(scene, {0.0, 20.0, 0.0});
    ball.get<RigidBodyComponent>().linearDamping = 0.0f;
    scene.startSimulation();
    run(scene, 30);
    // Gravity off mid-flight: the fall goes on at the speed it had.
    ball.get<RigidBodyComponent>().gravityFactor = 0.0f;
    run(scene, 1);
    const f32 vy = scene.linearVelocity(ball).y;
    run(scene, 30);
    CHECK(vy < -4.0f);
    CHECK_NEAR(scene.linearVelocity(ball).y, vy, 1e-3f);

    // Friction on mid-slide: the crate stops.
    Entity crate = scene.createEntity("Crate");
    crate.transform().position = {5.0, 0.5, 0.0};
    auto& crb = crate.add<RigidBodyComponent>();
    crb.friction = 0.0f;
    crb.linearDamping = 0.0f;
    ground.get<RigidBodyComponent>().friction = 0.0f;
    run(scene, 2);
    scene.setLinearVelocity(crate, {4, 0, 0});
    run(scene, 30);
    CHECK_NEAR(scene.linearVelocity(crate).x, 4.0f, 0.05f); // ice
    crb.friction = 1.0f;
    ground.get<RigidBodyComponent>().friction = 1.0f;
    run(scene, 60);
    CHECK(std::abs(scene.linearVelocity(crate).x) < 0.05f);

    // A solid floor turned into a sensor: what rests on it falls through.
    Entity shelf = scene.createEntity("Shelf");
    shelf.transform().position = {-5.0, 2.0, 0.0};
    shelf.transform().scale = {2, 0.2f, 2};
    shelf.add<RigidBodyComponent>(BodyType::Static);
    Entity pot = addBall(scene, {-5.0, 3.0, 0.0}, 0.3f);
    run(scene, 60);
    CHECK_NEAR(pot.transform().position.toMeters().y, 2.4, 0.03);
    shelf.get<RigidBodyComponent>().isSensor = true;
    run(scene, 90);
    CHECK_NEAR(pot.transform().position.toMeters().y, 0.3, 0.03);
}

void rebuildOnShapeChange() {
    Scene scene("rebuild");
    addGround(scene);
    Entity ball = addBall(scene, {0.0, 0.5, 0.0});
    scene.startSimulation();
    run(scene, 30);
    ball.get<SphereColliderComponent>().radius = 1.0f; // bigger: pushed up out of the ground
    run(scene, 60);
    CHECK_NEAR(ball.transform().position.toMeters().y, 1.0, 0.03);
    // Rebuilt while moving: keeps its velocity.
    ball.get<RigidBodyComponent>().gravityFactor = 0.0f;
    ball.get<RigidBodyComponent>().linearDamping = 0.0f;
    ball.transform().position = GlobalPosition(0.0, 10.0, 0.0);
    scene.syncTransformToPhysics(ball);
    scene.setLinearVelocity(ball, {3, 0, 0});
    run(scene, 2);
    ball.get<RigidBodyComponent>().mass = 5.0f; // a rebuild
    run(scene, 2);
    CHECK_NEAR(scene.linearVelocity(ball).x, 3.0f, 1e-3f);
    CHECK_NEAR(ball.transform().position.toMeters().y, 10.0, 1e-3);
    // Scaling the entity rebuilds it too.
    ball.transform().scale = glm::vec3(0.25f);
    run(scene, 1);
    auto hit = scene.raycast(GlobalPosition(ball.transform().position.toMeters().x, 20.0, 0.0), {0, -1, 0}, 20.0f);
    CHECK(hit && hit->entity == ball);
    if (hit) CHECK_NEAR(hit->point.toMeters().y, 10.0 + 0.25, 0.02); // radius 1 × 0.25
}

void staticFollowsTransform() {
    Scene scene("static");
    Entity wall = scene.createEntity("Wall");
    wall.transform().position = {5.0, 0.0, 0.0};
    wall.add<RigidBodyComponent>(BodyType::Static);
    scene.startSimulation();
    run(scene, 1);
    auto hit = scene.raycast(GlobalPosition(0.0, 0.0, 0.0), {1, 0, 0}, 20.0f);
    CHECK(hit && std::abs(hit->distance - 4.5f) < 1e-3f);
    wall.transform().position = {8.0, 0.0, 0.0}; // moved by code
    run(scene, 1);
    hit = scene.raycast(GlobalPosition(0.0, 0.0, 0.0), {1, 0, 0}, 20.0f);
    CHECK(hit && std::abs(hit->distance - 7.5f) < 1e-3f);
}

void continuousCollision() {
    // A 300 m/s bullet against a 5 cm wall 17.5 m away: one step moves it 5 m (0, 5, ... 20).
    for (bool ccd : {false, true}) {
        Scene scene("bullets");
        Entity wall = scene.createEntity("Wall");
        wall.transform().position = {17.5, 0.0, 0.0};
        wall.transform().scale = {0.05f, 4.0f, 4.0f};
        wall.add<RigidBodyComponent>(BodyType::Static);
        Entity bullet = addBall(scene, {0.0, 0.0, 0.0}, 0.05f);
        auto& rb = bullet.get<RigidBodyComponent>();
        rb.gravityFactor = 0.0f;
        rb.continuousCollision = ccd;
        scene.startSimulation();
        run(scene, 1);
        scene.setLinearVelocity(bullet, {300, 0, 0});
        run(scene, 10);
        const f64 x = bullet.transform().position.toMeters().x;
        if (ccd) CHECK(x < 17.5);  // stopped at the wall
        else CHECK(x > 17.5);      // tunnelled through (why CCD exists)
    }
}

void axisLocks() {
    Scene scene("locks");
    addGround(scene);
    Entity crate = scene.createEntity("Crate");
    crate.transform().position = {0.0, 0.5, 0.0};
    auto& rb = crate.add<RigidBodyComponent>();
    rb.locks.rotationX = rb.locks.rotationY = rb.locks.rotationZ = true;
    rb.locks.positionZ = true;
    scene.startSimulation();
    run(scene, 10);
    // An off-center push would spin it; locked, it only slides (and never along Z).
    scene.physics()->addImpulse(rb.body, {2, 0, 2});
    scene.physics()->addTorque(rb.body, {0, 50, 50});
    run(scene, 30);
    CHECK(std::abs(glm::dot(crate.transform().rotation * glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)) - 1.0f) < 1e-4f);
    CHECK(glm::length(scene.physics()->angularVelocity(rb.body)) < 1e-4f);
    CHECK(crate.transform().position.toMeters().x > 0.1);
    CHECK(std::abs(crate.transform().position.toMeters().z) < 1e-4);
}

void compoundColliders() {
    Scene scene("compound");
    addGround(scene);
    // A box and a sphere on one entity: a mallet. The sphere sits 1.5 above the box's center.
    Entity mallet = scene.createEntity("Mallet");
    mallet.transform().position = {0.0, 3.0, 0.0};
    mallet.add<RigidBodyComponent>(BodyType::Static);
    mallet.add<BoxColliderComponent>().halfExtents = {0.2f, 1.0f, 0.2f};
    auto& head = mallet.add<SphereColliderComponent>();
    head.radius = 0.5f;
    head.offset = {0.0f, 1.5f, 0.0f};
    // A box collider rotated 90° about Z: 2 m long along X becomes 2 m tall.
    Entity post = scene.createEntity("Post");
    post.transform().position = {5.0, 5.0, 0.0};
    post.add<RigidBodyComponent>();
    auto& pc = post.add<BoxColliderComponent>();
    pc.halfExtents = {1.0f, 0.25f, 0.25f};
    pc.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    scene.startSimulation();
    run(scene, 150);
    auto top = scene.raycast(GlobalPosition(0.0, 10.0, 0.0), {0, -1, 0}, 20.0f);
    CHECK(top && top->entity == mallet);
    if (top) CHECK_NEAR(top->point.toMeters().y, 3.0 + 1.5 + 0.5, 1e-3); // the head
    auto side = scene.raycast(GlobalPosition(-5.0, 3.0, 0.0), {1, 0, 0}, 20.0f);
    CHECK(side && side->entity == mallet);
    if (side) CHECK_NEAR(side->point.toMeters().x, -0.2, 1e-3); // the handle
    CHECK_NEAR(post.transform().position.toMeters().y, 1.0, 0.03); // standing on its 2 m length
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
        {"bodies: destroying a support wakes what rests on it", destroyingSupportWakes},
        {"triggers: enter and exit, no collision", triggers},
        {"collision layers: matrix, queries, runtime changes", collisionLayers},
        {"overlap queries", overlaps},
        {"physics settings are saved with the scene", settingsAreSaved},
        {"bodies: property edits apply while simulating", runtimeProperties},
        {"bodies: collider, mass and scale edits rebuild them", rebuildOnShapeChange},
        {"bodies: static bodies follow their transform", staticFollowsTransform},
        {"bodies: continuous collision stops fast bullets", continuousCollision},
        {"bodies: axis locks", axisLocks},
        {"bodies: compound and rotated colliders", compoundColliders},
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
