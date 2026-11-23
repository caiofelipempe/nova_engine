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
    const f32 vy = scene.linearVelocity(ball).toGlm().y;
    run(scene, 30);
    CHECK(vy < -4.0f);
    CHECK_NEAR(scene.linearVelocity(ball).toGlm().y, vy, 1e-3f);

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
    CHECK_NEAR(scene.linearVelocity(crate).toGlm().x, 4.0f, 0.05f); // ice
    crb.friction = 1.0f;
    ground.get<RigidBodyComponent>().friction = 1.0f;
    run(scene, 60);
    CHECK(std::abs(scene.linearVelocity(crate).toGlm().x) < 0.05f);

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
    CHECK_NEAR(scene.linearVelocity(ball).toGlm().x, 3.0f, 1e-3f);
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
    CHECK(std::abs(glm::dot(crate.transform().orientation() * glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)) - 1.0f) < 1e-4f);
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

// ---------------------------------------------------------------- joints --
Entity addBox(Scene& s, const std::string& name, const GlobalPosition& p, const glm::vec3& scale,
              BodyType type = BodyType::Dynamic) {
    Entity e = s.createEntity(name);
    e.transform().position = p;
    e.transform().scale = scale;
    auto& rb = e.add<RigidBodyComponent>(type);
    rb.linearDamping = rb.angularDamping = 0.0f;
    return e;
}

f32 yawOf(Entity e) { // rotation about +Y
    const glm::vec3 x = e.transform().orientation() * glm::vec3(1, 0, 0);
    return std::atan2(-x.z, x.x);
}

void hingeDoor() {
    Scene scene("door");
    scene.setPhysicsSettings([] { PhysicsSettings s; s.gravity = glm::vec3(0.0f); return s; }());
    // A 2 m wide door hinged to the world at its left edge, around +Y, ±45°.
    Entity door = addBox(scene, "Door", {1.0, 1.0, 0.0}, {2.0f, 2.0f, 0.1f});
    auto& j = door.add<JointComponent>();
    j.type = JointType::Hinge;
    j.anchor = {-0.5f, 0.0f, 0.0f}; // local, scaled: x = −1 m, the left edge
    j.axis = {0, 1, 0};
    j.limitsEnabled = true;
    j.limitMin = -45.0f;
    j.limitMax = 45.0f;
    scene.startSimulation();
    run(scene, 1);
    CHECK(j.joint.valid());
    scene.physics()->addImpulse(door.get<RigidBodyComponent>().body, {0, 0, -3}); // push the free edge
    run(scene, 60);
    // It turned around the hinge, which did not move, and stopped at the limit.
    const GlobalPosition hinge = door.transform().position + door.transform().orientation() * glm::vec3(-1.0f, 0.0f, 0.0f);
    CHECK(isWithin(hinge, GlobalPosition(0.0, 1.0, 0.0), 0.01));
    // Swung to the limit (it may rebound a little from it), never past it.
    const f32 angle = std::abs(scene.physics()->jointPosition(j.joint));
    CHECK(angle > glm::radians(35.0f) && angle < glm::radians(45.5f));
    CHECK_NEAR(std::abs(yawOf(door)), angle, 1e-3f);
    CHECK_VEC(door.transform().orientation() * glm::vec3(0, 1, 0), glm::vec3(0, 1, 0), 1e-3f); // only around Y
}

void hingeMotor() {
    Scene scene("wheel");
    scene.setPhysicsSettings([] { PhysicsSettings s; s.gravity = glm::vec3(0.0f); return s; }());
    Entity wheel = addBox(scene, "Wheel", {0.0, 2.0, 0.0}, {1.0f, 1.0f, 0.2f});
    auto& j = wheel.add<JointComponent>();
    j.type = JointType::Hinge;
    j.axis = {0, 0, 1};
    j.motorMode = JointMotorMode::Velocity;
    j.motorTarget = 90.0f; // deg/s
    scene.startSimulation();
    run(scene, 60);
    CHECK_NEAR(scene.physics()->angularVelocity(wheel.get<RigidBodyComponent>().body).z, glm::radians(90.0f), 0.02f);
    // Motor changes apply in place: now the other way.
    j.motorTarget = -180.0f;
    run(scene, 30);
    CHECK_NEAR(scene.physics()->angularVelocity(wheel.get<RigidBodyComponent>().body).z, glm::radians(-180.0f), 0.03f);
    CHECK(isWithin(wheel.transform().position, GlobalPosition(0.0, 2.0, 0.0), 1e-3));
}

void sliderLimits() {
    Scene scene("drawer");
    Entity drawer = addBox(scene, "Drawer", {0.0, 1.0, 0.0}, glm::vec3(0.5f));
    drawer.get<RigidBodyComponent>().gravityFactor = 0.0f;
    auto& j = drawer.add<JointComponent>();
    j.type = JointType::Slider;
    j.axis = {1, 0, 0};
    j.limitsEnabled = true;
    j.limitMin = 0.0f;
    j.limitMax = 1.0f;
    scene.startSimulation();
    run(scene, 1);
    scene.setLinearVelocity(drawer, {3, 2, 1}); // only X is free, up to 1 m
    run(scene, 60);
    const glm::dvec3 p = drawer.transform().position.toMeters();
    CHECK_NEAR(p.x, 1.0, 0.02);
    CHECK_NEAR(p.y, 1.0, 0.01);
    CHECK_NEAR(p.z, 0.0, 0.01);
    CHECK_NEAR(scene.physics()->jointPosition(j.joint), 1.0f, 0.02f);
}

void fixedJoint() {
    Scene scene("glued");
    addGround(scene);
    Entity a = addBox(scene, "A", {0.0, 3.0, 0.0}, glm::vec3(1.0f));
    Entity b = addBox(scene, "B", {1.0, 3.5, 0.0}, glm::vec3(0.5f));
    b.add<JointComponent>(JointType::Fixed, Scene::refOf(a));
    scene.startSimulation();
    run(scene, 120); // they fall and land together, as one rigid piece
    const glm::vec3 rel = glm::conjugate(a.transform().orientation()) * (b.transform().position - a.transform().position).toVec3();
    CHECK_VEC(rel, glm::vec3(1.0f, 0.5f, 0.0f), 0.02f); // same place in A's frame
    CHECK(std::abs(glm::dot(a.transform().orientation(), b.transform().orientation())) > 0.9999f); // same turn
    CHECK(a.transform().position.toMeters().y < 1.5);
}

void pendulum() {
    Scene scene("pendulum");
    Entity bob = addBall(scene, {2.0, 5.0, 0.0});
    auto& j = bob.add<JointComponent>();
    j.type = JointType::Distance;
    j.connectedAnchor = {-2.0f, 0.0f, 0.0f}; // the world point (0, 5, 0): a 2 m rope
    scene.startSimulation();
    f64 lowest = 10.0, worst = 0.0;
    for (int i = 0; i < 120; ++i) {
        scene.update(kFrame);
        const f64 len = distance(bob.transform().position, GlobalPosition(0.0, 5.0, 0.0));
        worst = std::max(worst, std::abs(len - 2.0));
        lowest = std::min(lowest, bob.transform().position.toMeters().y);
    }
    CHECK(worst < 0.02);              // the rope keeps its length
    CHECK_NEAR(lowest, 3.0, 0.03);    // swung through the bottom
    CHECK_NEAR(scene.physics()->jointPosition(j.joint), 2.0f, 0.02f);
}

struct BreakRecorder : Script {
    int breaks = 0;
    Entity connected{entt::null, nullptr};
    void onJointBreak(Entity c) override {
        ++breaks;
        connected = c;
    }
};

void breakingJoints() {
    for (f32 strength : {50.0f, 500.0f}) { // a 10 kg crate weighs 98 N
        Scene scene("break");
        Entity crate = addBox(scene, "Crate", {0.0, 5.0, 0.0}, glm::vec3(0.5f));
        crate.get<RigidBodyComponent>().mass = 10.0f;
        auto& j = crate.add<JointComponent>();
        j.type = JointType::Point;
        j.anchor = {0.0f, 1.0f, 0.0f}; // 0.5 m above its center
        j.breakForce = strength;
        auto& rec = crate.addScript<BreakRecorder>();
        scene.startSimulation();
        run(scene, 60);
        if (strength < 98.0f) {
            CHECK(rec.breaks == 1 && j.broken && !j.joint);
            CHECK(!rec.connected.valid()); // it held the world
            CHECK(crate.transform().position.toMeters().y < 2.0); // fell
        } else {
            CHECK(rec.breaks == 0 && !j.broken && j.joint);
            CHECK_NEAR(crate.transform().position.toMeters().y, 5.0, 0.01);
        }
    }
}

void jointLifetime() {
    Scene scene("lifetime");
    Entity a = addBox(scene, "A", {0.0, 3.0, 0.0}, glm::vec3(1.0f));
    Entity b = addBox(scene, "B", {2.0, 3.0, 0.0}, glm::vec3(1.0f));
    auto& j = b.add<JointComponent>(JointType::Hinge, Scene::refOf(a));
    j.anchor = {-1.0f, 0.0f, 0.0f};
    scene.startSimulation();
    run(scene, 2);
    CHECK(j.joint.valid());
    scene.destroyEntity(a); // the other body goes: so does the joint
    run(scene, 10);
    CHECK(!j.joint.valid());
    CHECK(b.transform().position.toMeters().y < 3.0); // falls freely
    // Added during the simulation: made on the next update.
    Entity c = addBox(scene, "C", {5.0, 3.0, 0.0}, glm::vec3(1.0f));
    auto& jc = c.add<JointComponent>();
    jc.type = JointType::Point;
    run(scene, 1);
    CHECK(jc.joint.valid());
    c.remove<JointComponent>(); // removed: destroyed (no crash, body free)
    run(scene, 30);
    CHECK(c.transform().position.toMeters().y < 3.0);
}

void farJointsAndRebase() {
    // A pendulum near the far corner; the focus jumps (rebase): the world anchor stays where it was.
    const GlobalPosition base = GlobalPosition(7500.0, 0.0, 0.0);
    Scene scene("far");
    scene.setPhysicsFocus(base);
    Entity bob = addBall(scene, base + glm::vec3(2.0f, 0.0f, 0.0f));
    auto& j = bob.add<JointComponent>();
    j.type = JointType::Distance;
    j.connectedAnchor = {-2.0f, 0.0f, 0.0f};
    scene.startSimulation();
    run(scene, 30);
    CHECK_NEAR(distance(bob.transform().position, base), 2.0, 0.02);
    scene.setPhysicsFocus(base + glm::vec3(3000.0f, 0.0f, 0.0f));
    run(scene, 60);
    CHECK_NEAR(distance(bob.transform().position, base), 2.0, 0.02);
    CHECK(j.joint.valid());
}

// The circular world: positions go around the world's size by themselves; the physics carries
// bodies across a seam (velocity kept, reported to scripts) and collides them across it.
struct EdgeRecorder : Script {
    std::vector<WorldWrapHit> hits;
    void onWorldWrap(const WorldWrapHit& h) override { hits.push_back(h); }
};

void circularWorld() {
    // The rule itself, in exact world steps.
    Scene scene;
    scene.setWorldSize(WorldSize::meters(20.0, 0.0, 20.0));
    CHECK(scene.worldSize().size(0) == 20.0 && scene.worldSize().size(1) == WorldSpace::kDefaultSize);
    CHECK(GlobalPosition(10.25, 1.0, -10.5) == GlobalPosition(-9.75, 1.0, 9.5));
    CHECK(GlobalPosition(-10.0, 0.0, 0.0).toMeters().x == -10.0); // [−10, +10)
    CHECK(GlobalPosition(10.0, 0.0, 0.0).toMeters().x == -10.0);
    CHECK((GlobalPosition(9.0, 0.0, 0.0) - GlobalPosition(-9.0, 0.0, 0.0)).toMeters().x == -2.0); // the short way
    CHECK(GlobalPosition(0.0, 8000.0, 0.0).toMeters().y == 8000.0); // Y keeps the default size
    CHECK(WorldSize::meters(1e15, 0.0, 0.0).size(0) == WorldSpace::kMaxSize);

    // A ball rolling out at +10 m comes in at −10 m, same velocity, the crossing reported.
    PhysicsSettings ps;
    ps.gravity = {0.0, 0.0, 0.0};
    scene.setPhysicsSettings(ps);
    Entity ball = scene.createEntity("Ball");
    ball.transform().position = {9.0, 0.0, 0.0};
    auto& brb = ball.add<RigidBodyComponent>();
    brb.linearDamping = 0.0f;
    ball.add<SphereColliderComponent>();
    auto& rec = ball.addScript<EdgeRecorder>();
    scene.startSimulation();
    scene.update(Duration::perSecond(60)); // onCreate
    scene.setLinearVelocity(ball, {6.0f, 0.0f, 0.0f});
    for (int i = 0; i < 20; ++i) scene.update(Duration::perSecond(60)); // 2 m
    const f64 x = ball.transform().position.toMeters().x;
    CHECK(x > -10.0 && x < -8.5);
    CHECK(std::abs(scene.linearVelocity(ball).toGlm().x - 6.0f) < 1e-3f);
    CHECK(rec.hits.size() == 1 && rec.hits[0].axis == 0 && rec.hits[0].side == 1);

    // Two balls on both sides of the seam collide through it: for the physics they are neighbours.
    scene.stopSimulation();
    ball.transform().position = {8.5, 0.0, 0.0};
    Entity other = scene.createEntity("Other");
    other.transform().position = {-8.5, 0.0, 0.0};
    other.add<RigidBodyComponent>().linearDamping = 0.0f;
    other.add<SphereColliderComponent>();
    scene.startSimulation();
    scene.setLinearVelocity(ball, {6.0f, 0.0f, 0.0f});
    for (int i = 0; i < 40; ++i) scene.update(Duration::perSecond(60));
    CHECK(scene.linearVelocity(other).toGlm().x > 2.0f); // pushed along +X, from the other side
    CHECK(scene.linearVelocity(ball).toGlm().x < 4.0f);

    // The float window has its own seam, half a lap from the WorldOrigin. A static tile across it
    // (from 8 m to 11 m ≡ −9 m) still holds a ball dropped on its far part: statics in small worlds
    // have ghost copies one lap over.
    {
        Scene tile;
        tile.setWorldSize(WorldSize::meters(20.0, 0.0, 0.0));
        tile.setWorldOrigin(GlobalPosition()); // the window seam at x = ±10 m
        Entity floor = tile.createEntity("Tile");
        floor.transform().position = {9.5, -0.5, 0.0};
        floor.transform().scale = {3.0, 1.0, 3.0};
        floor.add<RigidBodyComponent>(BodyType::Static);
        floor.add<BoxColliderComponent>();
        Entity dropped = tile.createEntity("Dropped");
        dropped.transform().position = {-9.6, 2.0, 0.0}; // ≡ 10.4 m: on the tile, past the window seam
        dropped.add<RigidBodyComponent>();
        dropped.add<SphereColliderComponent>();
        tile.startSimulation();
        CHECK(tile.physicsBodyCount() == 2); // ghosts are not counted
        for (int i = 0; i < 90; ++i) tile.update(Duration::perSecond(60));
        CHECK_NEAR(dropped.transform().position.toMeters().y, 0.5, 0.05); // resting on the tile
        WorldSize::setActive(scene.worldSize());
    }

    // Without a body: the transform itself is always inside.
    Entity marker = scene.createEntity("Marker");
    marker.transform().position = {0.0, 50.0, -30.0};
    CHECK(marker.transform().position == GlobalPosition(0.0, 50.0, -10.0));

    // A smaller world brings every position inside it.
    scene.stopSimulation();
    marker.transform().position = {9.0, 0.0, 9.0};
    scene.setWorldSize(WorldSize::meters(10.0, 0.0, 10.0));
    CHECK(marker.transform().position == GlobalPosition(-1.0, 0.0, -1.0));

    // Saved with the scene.
    const std::string json = serializeScene(scene);
    Scene back;
    CHECK(deserializeScene(back, json));
    CHECK(back.worldSize() == scene.worldSize());
    Scene plain;
    CHECK(deserializeScene(plain, R"({"format": "nova.scene", "version": 1, "entities": []})"));
    CHECK(plain.worldSize().isDefault());
    // Older files with barrier edges load as circular worlds of the same size.
    Scene old;
    CHECK(deserializeScene(old, R"({"format": "nova.scene", "version": 1, "world": {"axes": [{"size": 30, "edge": "Barrier"},
        {"size": 16384, "edge": "Wrap"}, {"size": 40, "edge": "Wrap"}]}, "entities": []})"));
    CHECK(old.worldSize() == WorldSize::meters(30.0, 0.0, 40.0));
    WorldSize::setActive({});
}

void seamlessFloor() {
    // A floor exactly one lap long (20 m): its two ends meet, one lap over. Its copy there is part of
    // the same body, so a ball rolls across that join without the bump separate bodies give (on
    // their edge a ball rolling at 9 m/s is thrown up at more than 2 m/s). Once with the join away
    // from the float window's own seam (x = ±10 m, the origin at 0), once right on it.
    for (const f64 center : {5.0, 0.0}) {
        Scene scene;
        scene.setWorldSize(WorldSize::meters(20.0, 0.0, 0.0));
        scene.setWorldOrigin(GlobalPosition());
        Entity floor = scene.createEntity("Floor");
        floor.transform().position = {center, -0.5, 0.0};
        floor.transform().scale = {20.0, 1.0, 6.0};
        floor.add<RigidBodyComponent>(BodyType::Static);
        floor.add<BoxColliderComponent>();
        Entity ball = scene.createEntity("Ball");
        ball.transform().position = {0.0, 0.5, 0.0};
        auto& rb = ball.add<RigidBodyComponent>();
        rb.linearDamping = 0.0f;
        rb.restitution = 0.6f;
        ball.add<SphereColliderComponent>();
        scene.startSimulation();
        run(scene, 30); // settled
        scene.setLinearVelocity(ball, {9.0f, 0.0f, 0.0f});
        f32 rise = 0.0f, side = 0.0f;
        f64 highest = 0.0;
        for (int i = 0; i < 60 * 4; ++i) { // about 26 m: across the join and the window seam
            scene.update(kFrame);
            const glm::vec3 v = scene.linearVelocity(ball).toGlm();
            rise = std::max(rise, v.y);
            side = std::max(side, std::abs(v.z));
            highest = std::max(highest, ball.transform().position.toMeters().y);
        }
        CHECK(rise < (center == 0.0 ? 0.5f : 0.2f)); // a millimetre hop at most (5 mm on the window seam)
        CHECK(side < 0.01f);
        CHECK(highest < 0.51);
        CHECK(scene.linearVelocity(ball).toGlm().x > 5.5f); // rolling (5/7 of the push), not stopped
        CHECK(scene.physicsBodyCount() == 2);
    }
    WorldSize::setActive({});
}

void kinematicGlide() {
    // A kinematic body moved 2 m in a frame of two physics steps goes 1 m in each: it pushes what
    // rides it at the same speed in both (not twice as fast in the first and still in the second).
    Scene scene;
    PhysicsSettings ps;
    ps.gravity = {0.0, 0.0, 0.0};
    scene.setPhysicsSettings(ps);
    Entity platform = scene.createEntity("Platform");
    platform.add<RigidBodyComponent>(BodyType::Kinematic);
    platform.add<BoxColliderComponent>();
    scene.startSimulation();
    run(scene, 1);
    platform.transform().position = {2.0, 0.0, 0.0};
    scene.update(Duration::perSecond(30)); // two steps
    CHECK_NEAR(scene.linearVelocity(platform).toGlm().x, 60.0, 0.01); // 1 m in the last 1/60 s
    GlobalPosition at;
    glm::quat q;
    scene.physics()->getTransform(platform.get<RigidBodyComponent>().body, at, q);
    CHECK_NEAR(at.toMeters().x, 2.0, 1e-4);
}

void drawnBetweenSteps() {
    // What the physics moves is drawn between its last two steps, by the time left over; its
    // transform keeps the simulated pose.
    Scene scene;
    Entity ball = scene.createEntity("Ball");
    ball.transform().position = {0.0, 20.0, 0.0};
    ball.add<MeshComponent>(MeshHandle{1});
    ball.add<RigidBodyComponent>();
    ball.add<SphereColliderComponent>();
    scene.startSimulation();
    run(scene, 29);
    const f64 before = ball.transform().position.toMeters().y;
    run(scene, 1);
    const f64 after = ball.transform().position.toMeters().y;
    CHECK(after < before);
    CHECK_NEAR(scene.renderTransform(ball).position.toMeters().y, before, 1e-4); // no time left over: one step behind
    scene.update(Duration::perSecond(120)); // half a step: no step runs
    CHECK(ball.transform().position.toMeters().y == after);
    const f64 halfway = (before + after) / 2.0;
    CHECK_NEAR(scene.renderTransform(ball).position.toMeters().y, halfway, 1e-4);
    RenderScene rs;
    scene.buildRenderScene(rs, 1.0f);
    CHECK(rs.draws.size() == 1 && std::abs(rs.draws[0].transform[3].y - static_cast<f32>(halfway)) < 1e-3f);
    // Turned off, or moved by hand since the step: drawn where its transform is.
    scene.interpolatePhysics = false;
    CHECK(scene.renderTransform(ball).position.toMeters().y == after);
    scene.interpolatePhysics = true;
    ball.transform().position = {5.0, 3.0, 0.0};
    CHECK(scene.renderTransform(ball).position == GlobalPosition(5.0, 3.0, 0.0));
}

void prefabRemapsRefs() {
    Scene scene("prefab");
    Entity a = scene.createEntity("A");
    a.add<RigidBodyComponent>();
    Entity b = scene.createEntity("B");
    b.add<RigidBodyComponent>();
    b.add<JointComponent>(JointType::Hinge, Scene::refOf(a));
    Entity c = scene.createEntity("Outside");
    Entity d = scene.createEntity("D");
    d.add<JointComponent>(JointType::Point, Scene::refOf(c)); // points outside the prefab
    const std::string json = serializePrefab({a, b, d});
    std::vector<Entity> copies = instantiatePrefab(scene, json, GlobalPosition(10.0, 0.0, 0.0));
    CHECK(copies.size() == 3);
    if (copies.size() == 3) {
        CHECK(copies[1].get<JointComponent>().connected == Scene::refOf(copies[0])); // the new A
        CHECK(copies[2].get<JointComponent>().connected == Scene::refOf(c));         // unchanged
    }
    // And joints survive a save / load.
    Scene loaded("loaded");
    CHECK(deserializeScene(loaded, serializeScene(scene)));
    Entity lb = loaded.findEntity("B");
    CHECK(lb.valid() && lb.get<JointComponent>().type == JointType::Hinge);
    CHECK(lb.valid() && loaded.resolve(lb.get<JointComponent>().connected).name() == "A");
}

// ------------------------------------------------------------ characters --
Entity addCharacter(Scene& s, const GlobalPosition& feet) {
    Entity c = s.createEntity("Player");
    c.transform().position = feet;
    c.add<CharacterControllerComponent>();
    return c;
}

// Holds a horizontal input for some frames, as a player script would.
void walk(Scene& s, Entity c, const glm::vec3& input, int frames) {
    for (int i = 0; i < frames; ++i) {
        auto& cc = c.get<CharacterControllerComponent>();
        cc.velocity.x = input.x;
        cc.velocity.z = input.z;
        s.update(kFrame);
    }
}

Entity addStatic(Scene& s, const std::string& name, const GlobalPosition& center, const glm::vec3& size,
                 const glm::quat& rotation = glm::quat(1, 0, 0, 0)) {
    Entity e = s.createEntity(name);
    e.transform().position = center;
    e.transform().scale = size;
    e.transform().rotation = rotation;
    e.add<RigidBodyComponent>(BodyType::Static);
    return e;
}

void characterWalks() {
    Scene scene("walk");
    Entity ground = addGround(scene);
    Entity player = addCharacter(scene, {0.0, 1.0, 0.0});
    scene.startSimulation();
    run(scene, 60); // falls and lands
    auto& cc = player.get<CharacterControllerComponent>();
    CHECK(cc.grounded);
    CHECK(scene.resolve(cc.ground) == ground);
    CHECK_NEAR(player.transform().position.toMeters().y, 0.0, 0.03); // feet on the ground
    walk(scene, player, {2, 0, 0}, 60);
    CHECK_NEAR(player.transform().position.toMeters().x, 2.0, 0.05);
    CHECK(cc.grounded);
    CHECK_NEAR(player.transform().position.toMeters().y, 0.0, 0.03);
    // Jump: leaves the ground and comes back down.
    cc.velocity.y = 5.0f;
    walk(scene, player, {0, 0, 0}, 10);
    CHECK(!cc.grounded && player.transform().position.toMeters().y > 0.5);
    walk(scene, player, {0, 0, 0}, 90);
    CHECK(cc.grounded);
    CHECK_NEAR(player.transform().position.toMeters().y, 0.0, 0.03);
}

void characterSlidesAlongWalls() {
    Scene scene("wall");
    addGround(scene);
    addStatic(scene, "Wall", {2.0, 1.0, 0.0}, {1.0f, 2.0f, 20.0f}); // face at x = 1.5
    Entity player = addCharacter(scene, {0.0, 0.05, 0.0});
    scene.startSimulation();
    run(scene, 10);
    walk(scene, player, {3, 0, 1}, 60); // into the wall, and along it
    const glm::dvec3 p = player.transform().position.toMeters();
    CHECK_NEAR(p.x, 1.5 - 0.3, 0.05);       // stopped by the wall (capsule radius 0.3)
    CHECK_NEAR(p.z, 1.0, 0.1);              // kept going along it
    CHECK(player.get<CharacterControllerComponent>().grounded);
}

void characterStairsAndSlopes() {
    Scene scene("stairs");
    addGround(scene);
    // A 0.2 m step (walkable) and, further along Z, a 0.6 m ledge (too high).
    addStatic(scene, "Step", {3.0, 0.1, 0.0}, {4.0f, 0.2f, 4.0f});
    addStatic(scene, "Ledge", {3.0, 0.3, 10.0}, {4.0f, 0.6f, 4.0f});
    Entity a = addCharacter(scene, {0.0, 0.05, 0.0});
    Entity b = addCharacter(scene, {0.0, 0.05, 10.0});
    scene.startSimulation();
    run(scene, 10);
    for (int i = 0; i < 90; ++i) {
        a.get<CharacterControllerComponent>().velocity.x = 2.0f;
        b.get<CharacterControllerComponent>().velocity.x = 2.0f;
        scene.update(kFrame);
    }
    CHECK(a.transform().position.toMeters().x > 1.5);           // up the step...
    CHECK_NEAR(a.transform().position.toMeters().y, 0.2, 0.03); // ...standing on it
    CHECK(b.transform().position.toMeters().x < 1.0 - 0.25);    // the ledge stops it
    // A 60° slope is too steep to stand on.
    Scene steep("slope");
    addStatic(steep, "Slope", {0.0, 0.0, 0.0}, {10.0f, 0.2f, 10.0f},
              glm::angleAxis(glm::radians(60.0f), glm::vec3(0, 0, 1)));
    Entity c = addCharacter(steep, {0.0, 3.0, 0.0});
    steep.startSimulation();
    run(steep, 60);
    CHECK(!c.get<CharacterControllerComponent>().grounded);
    CHECK(c.get<CharacterControllerComponent>().groundState == GroundState::OnSteepGround ||
          c.transform().position.toMeters().y < 2.0); // slid down
}

void characterPushesAndIsSeen() {
    Scene scene("push");
    addGround(scene);
    Entity crate = scene.createEntity("Crate");
    crate.transform().position = {1.5, 0.5, 0.0};
    crate.add<RigidBodyComponent>().mass = 5.0f;
    Entity zone = scene.createEntity("Zone");
    zone.transform().position = {0.0, 1.0, 5.0};
    zone.transform().scale = {2, 2, 2};
    zone.add<RigidBodyComponent>(BodyType::Static).isSensor = true;
    auto& rz = zone.addScript<Recorder>();
    Entity player = addCharacter(scene, {0.0, 0.05, 0.0});
    scene.startSimulation();
    run(scene, 10);
    walk(scene, player, {2, 0, 0}, 90); // into the crate: it gets pushed
    CHECK(crate.transform().position.toMeters().x > 2.0);
    // Queries see the character (and can ignore it).
    auto hit = scene.raycast(player.transform().position + glm::vec3(0, 0.9f, -5), {0, 0, 1}, 10.0f);
    CHECK(hit && hit->entity == player);
    auto through = scene.raycast(player.transform().position + glm::vec3(0, 0.9f, -5), {0, 0, 1}, 10.0f, player);
    CHECK(!through || through->entity != player);
    // Sensors see it (walking back, then into the zone).
    walk(scene, player, {-2, 0, 0}, 60);
    const GlobalPosition start = player.transform().position;
    for (int i = 0; i < 240; ++i) {
        auto& cc = player.get<CharacterControllerComponent>();
        const glm::dvec3 to = (GlobalPosition(0.0, 0.0, 8.0) - player.transform().position).toMeters();
        cc.velocity.x = static_cast<f32>(glm::clamp(to.x, -2.0, 2.0));
        cc.velocity.z = static_cast<f32>(glm::clamp(to.z, -2.0, 2.0));
        scene.update(kFrame);
    }
    CHECK(distance(player.transform().position, start) > 5.0);
    CHECK(rz.count("enter") >= 1);
    if (!rz.log.empty()) CHECK(rz.log.front().other == player);
}

void characterTeleportAndFar() {
    const GlobalPosition base = GlobalPosition(7500.0, 0.0, 0.0);
    Scene scene("far");
    addGround(scene, base);
    Entity player = addCharacter(scene, base + glm::vec3(0.0f, 0.05f, 0.0f));
    scene.setPhysicsFocus(base);
    scene.startSimulation();
    run(scene, 10);
    walk(scene, player, {1, 0, 0}, 60);
    CHECK_NEAR((player.transform().position - base).toMeters().x, 1.0, 0.03);
    CHECK_NEAR((player.transform().position - base).toMeters().y, 0.0, 0.03);
    // Moved by code: a teleport (the velocity stays: stop it too).
    player.transform().position = base + glm::vec3(-10.0f, 3.0f, 0.0f);
    player.get<CharacterControllerComponent>().velocity = glm::vec3(0.0f);
    run(scene, 60);
    CHECK_NEAR((player.transform().position - base).toMeters().x, -10.0, 0.03);
    CHECK_NEAR((player.transform().position - base).toMeters().y, 0.0, 0.03); // fell back down
    // Removing the component removes the character.
    player.remove<CharacterControllerComponent>();
    run(scene, 2);
    CHECK(!scene.raycast(base + glm::vec3(-10.0f, 5.0f, 0.0f), {0, -1, 0}, 4.0f));
}

} // namespace

int main() {
    Log::init();
    Log::core().set_level(spdlog::level::warn);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"contacts: begin and end, once each", contactBeginsAndEnds},
        {"contacts: rolling across mesh triangles is one touch", meshTouchIsOne},
        {"contacts: destroying a body ends its touches", destroyingEndsTouches},
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
        {"joints: hinge door with limits", hingeDoor},
        {"joints: hinge motor", hingeMotor},
        {"joints: slider with limits", sliderLimits},
        {"joints: fixed", fixedJoint},
        {"joints: distance (pendulum)", pendulum},
        {"joints: breaking", breakingJoints},
        {"joints: lifetime with bodies and components", jointLifetime},
        {"joints: far out, across a rebase", farJointsAndRebase},
        {"joints: prefab references and saving", prefabRemapsRefs},
        {"circular world: wrap-around, collisions across the seam, files", circularWorld},
        {"circular world: a floor one lap long has no bump where its ends meet", seamlessFloor},
        {"kinematic bodies: a frame's move spread over its steps", kinematicGlide},
        {"drawing: between the last two physics steps", drawnBetweenSteps},
        {"characters: walk, land, jump", characterWalks},
        {"characters: slide along walls", characterSlidesAlongWalls},
        {"characters: stairs and steep slopes", characterStairsAndSlopes},
        {"characters: push bodies, seen by queries and sensors", characterPushesAndIsSeen},
        {"characters: teleport, far out, removal", characterTeleportAndFar},
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
