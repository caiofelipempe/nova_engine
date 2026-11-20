// Headless physics tests (no window / GPU). Run: ctest --preset ... or NovaTests.exe
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
#define CHECK_NEAR(a, b, eps) check(std::abs((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)

constexpr f32 kEps = 0.02f;
const glm::quat kIdentity{1, 0, 0, 0};
const glm::vec3 kDown{0, -1, 0};

// Static ground: a 100x1x100 box whose top face is at y = 0.
BodyId addGround(PhysicsWorld& w) {
    BodyDesc d;
    d.type = BodyType::Static;
    d.shape.halfExtents = {50.0f, 0.5f, 50.0f};
    d.position = {0.0f, -0.5f, 0.0f};
    d.userData = 1000;
    return w.createBody(d);
}

BodyId addBox(PhysicsWorld& w, glm::vec3 pos, BodyType type = BodyType::Static, u64 userData = 0, bool sensor = false) {
    BodyDesc d;
    d.type = type;
    d.shape.halfExtents = glm::vec3(0.5f);
    d.position = pos;
    d.userData = userData;
    d.isSensor = sensor;
    return w.createBody(d);
}

ShapeDesc sphere(f32 r) {
    ShapeDesc s;
    s.type = ShapeType::Sphere;
    s.radius = r;
    return s;
}

void sphereSweepHitsGround() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    addGround(*w);
    SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, kDown, 100.0f, hit));
    CHECK_NEAR(hit.distance, 4.5f, kEps); // centre stops 0.5 above the floor
    CHECK_NEAR(hit.point.y, 0.0f, kEps);
    CHECK_NEAR(hit.normal.y, 1.0f, kEps);
    CHECK(hit.userData == 1000);
}

void sweepMissesAndRespectsMaxDistance() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    addGround(*w);
    SweepHit hit;
    CHECK(!w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, {0, 1, 0}, 100.0f, hit)); // away from the floor
    CHECK(!w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, kDown, 4.0f, hit));       // too short
    CHECK(!w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, glm::vec3(0.0f), 10.0f, hit)); // zero direction
}

void boxAndCapsuleSweeps() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    addGround(*w);
    ShapeDesc box;
    box.type = ShapeType::Box;
    box.halfExtents = {1.0f, 0.25f, 1.0f};
    SweepHit hit;
    CHECK(w->sweep(box, {0, 3, 0}, kIdentity, kDown, 10.0f, hit));
    CHECK_NEAR(hit.distance, 2.75f, kEps);

    ShapeDesc capsule; // standing capsule: half height 1 + radius 0.5 => bottom 1.5 below centre
    capsule.type = ShapeType::Capsule;
    capsule.radius = 0.5f;
    capsule.halfHeight = 1.0f;
    CHECK(w->sweep(capsule, {0, 4, 0}, kIdentity, kDown, 10.0f, hit));
    CHECK_NEAR(hit.distance, 2.5f, kEps);

    // Rotated 90deg around Z the capsule lies flat: bottom is only 0.5 below the centre.
    glm::quat lying = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    CHECK(w->sweep(capsule, {0, 4, 0}, lying, kDown, 10.0f, hit));
    CHECK_NEAR(hit.distance, 3.5f, kEps);
}

void sideHitNormal() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    addBox(*w, {5, 0, 0}, BodyType::Static, 7);
    SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, hit));
    CHECK_NEAR(hit.distance, 4.0f, kEps); // box face at x=4.5, sphere radius 0.5
    CHECK_NEAR(hit.normal.x, -1.0f, kEps);
    CHECK_NEAR(hit.point.x, 4.5f, kEps);
    CHECK(hit.userData == 7);
}

void sweepAllSortedAndFiltered() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    BodyId a = addBox(*w, {3, 0, 0}, BodyType::Static, 1);
    addBox(*w, {6, 0, 0}, BodyType::Static, 2);
    addBox(*w, {9, 0, 0}, BodyType::Static, 3);
    addBox(*w, {9, 5, 0}, BodyType::Static, 4); // off the path

    auto hits = w->sweepAll(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f);
    CHECK(hits.size() == 3);
    if (hits.size() == 3) {
        CHECK(hits[0].userData == 1 && hits[1].userData == 2 && hits[2].userData == 3);
        CHECK(hits[0].distance < hits[1].distance && hits[1].distance < hits[2].distance);
    }

    QueryFilter ignoreFirst;
    ignoreFirst.ignoreBody = a;
    SweepHit hit;
    CHECK(w->sweep(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, hit, ignoreFirst));
    CHECK(hit.userData == 2);

    RaycastHit ray;
    CHECK(w->raycast({0, 0, 0}, {1, 0, 0}, 20.0f, ray, ignoreFirst));
    CHECK(ray.userData == 2);
    CHECK_NEAR(ray.distance, 5.5f, kEps);
}

void sensorsAreOptIn() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    addBox(*w, {3, 0, 0}, BodyType::Static, 1, /*sensor*/ true);
    addBox(*w, {6, 0, 0}, BodyType::Static, 2);
    SweepHit hit;
    CHECK(w->sweep(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, hit));
    CHECK(hit.userData == 2);
    QueryFilter withSensors;
    withSensors.includeSensors = true;
    CHECK(w->sweep(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, hit, withSensors));
    CHECK(hit.userData == 1);
}

void sweepBodyIgnoresItself() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    addGround(*w);
    BodyId box = addBox(*w, {0, 3, 0}, BodyType::Dynamic, 5);
    SweepHit hit;
    CHECK(w->sweepBody(box, kDown, 10.0f, hit));
    CHECK_NEAR(hit.distance, 2.5f, kEps);
    CHECK(hit.userData == 1000);
    CHECK(!w->sweepBody(box, {0, 1, 0}, 10.0f, hit)); // nothing above, and never itself
}

void overlappingAtStart() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    addGround(*w);
    SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), {0, 0.2f, 0}, kIdentity, kDown, 5.0f, hit));
    CHECK_NEAR(hit.fraction, 0.0f, 1e-4f);
}

void sweepAgainstBodyList() {
    auto w = PhysicsWorld::create(Backend::Jolt);
    BodyId ground = addGround(*w);
    BodyId nearBox = addBox(*w, {3, 0, 0}, BodyType::Static, 1);
    BodyId midBox = addBox(*w, {6, 0, 0}, BodyType::Dynamic, 2);
    BodyId farBox = addBox(*w, {9, 0, 0}, BodyType::Static, 3);
    BodyId sensor = addBox(*w, {1.5f, 0, 0}, BodyType::Static, 4, /*sensor*/ true);
    const glm::vec3 origin{0, 0.4f, 0}; // centre below the boxes' top (0.5) => face hits; clear of the floor (0)
    const glm::vec3 right{1, 0, 0};

    SweepHit hit;
    // Only the listed bodies count: `nearBox` is in the way but not in the list.
    std::vector<BodyId> onlyFar{farBox};
    CHECK(w->sweepAgainst(sphere(0.25f), origin, kIdentity, right, 20.0f, onlyFar, hit));
    CHECK(hit.body == farBox && hit.userData == 3);
    CHECK_NEAR(hit.distance, 8.25f, kEps); // face at x = 8.5, radius 0.25

    // Closest of the listed bodies wins, regardless of list order.
    std::vector<BodyId> farAndMid{farBox, midBox};
    CHECK(w->sweepAgainst(sphere(0.25f), origin, kIdentity, right, 20.0f, farAndMid, hit));
    CHECK(hit.body == midBox);

    // Listed sensors are tested (an explicit list means "test exactly these").
    std::vector<BodyId> withSensor{nearBox, sensor};
    CHECK(w->sweepAgainst(sphere(0.25f), origin, kIdentity, right, 20.0f, withSensor, hit));
    CHECK(hit.body == sensor);

    // Unlisted ground never reported even when sweeping straight into it.
    CHECK(!w->sweepAgainst(sphere(0.25f), origin, kIdentity, kDown, 20.0f, onlyFar, hit));
    std::vector<BodyId> onlyGround{ground};
    CHECK(w->sweepAgainst(sphere(0.25f), origin, kIdentity, kDown, 20.0f, onlyGround, hit));
    CHECK_NEAR(hit.distance, 0.15f, kEps);

    // All hits: sorted, de-duplicated, removed/invalid ids skipped.
    BodyId removed = addBox(*w, {12, 0, 0}, BodyType::Static, 5);
    w->destroyBody(removed);
    std::vector<BodyId> list{farBox, nearBox, midBox, nearBox, removed, BodyId{}};
    auto hits = w->sweepAllAgainst(sphere(0.25f), origin, kIdentity, right, 20.0f, list);
    CHECK(hits.size() == 3);
    if (hits.size() == 3) CHECK(hits[0].body == nearBox && hits[1].body == midBox && hits[2].body == farBox);

    // Empty list / out of range.
    CHECK(!w->sweepAgainst(sphere(0.25f), origin, kIdentity, right, 20.0f, {}, hit));
    CHECK(!w->sweepAgainst(sphere(0.25f), origin, kIdentity, right, 2.0f, onlyFar, hit));
}

void sceneSweepAgainstEntities() {
    Scene scene("test");
    Entity a = scene.createEntity("A");
    a.transform().position = {3, 0, 0};
    a.add<RigidBodyComponent>(BodyType::Static);
    Entity b = scene.createEntity("B");
    b.transform().position = {6, 0, 0};
    b.add<RigidBodyComponent>(BodyType::Static);
    Entity noBody = scene.createEntity("No body"); // skipped: no RigidBodyComponent
    scene.startSimulation();

    std::vector<Entity> targets{b, noBody};
    auto hit = scene.sweepAgainst(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, targets);
    CHECK(hit && hit->entity == b);

    std::vector<Entity> both{b, a};
    auto all = scene.sweepAllAgainst(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, both);
    CHECK(all.size() == 2 && all[0].entity == a && all[1].entity == b);
}

void sceneLevelQueries() {
    Scene scene("test");
    Entity ground = scene.createEntity("Ground");
    ground.transform().position = {0, -0.5f, 0};
    ground.transform().scale = {100, 1, 100};
    ground.add<RigidBodyComponent>(BodyType::Static);

    Entity crate = scene.createEntity("Crate");
    crate.transform().position = {0, 4, 0};
    crate.add<RigidBodyComponent>(); // dynamic, default box collider

    Entity wall = scene.createEntity("Wall");
    wall.transform().position = {0, 4, -5};
    wall.add<RigidBodyComponent>(BodyType::Static);

    CHECK(!scene.sweepSphere({0, 10, 0}, 0.5f, kDown)); // no world before startSimulation
    scene.startSimulation();

    auto hit = scene.sweepSphere({0, 10, 0}, 0.5f, kDown);
    CHECK(hit && hit->entity == crate);
    if (hit) CHECK_NEAR(hit->distance, 5.0f, kEps); // crate top at 4.5, sphere radius 0.5

    auto ignored = scene.sweepSphere({0, 10, 0}, 0.5f, kDown, 1000.0f, crate);
    CHECK(ignored && ignored->entity == ground);

    auto forward = scene.sweepEntity(crate, {0, 0, -1});
    CHECK(forward && forward->entity == wall);
    if (forward) CHECK_NEAR(forward->distance, 4.0f, kEps);

    auto all = scene.sweepAll(sphere(0.5f), {0, 10, 0}, kIdentity, kDown);
    CHECK(all.size() == 2 && all[0].entity == crate && all[1].entity == ground);

    auto box = scene.sweepBox({0, 10, -5}, glm::vec3(0.4f), kIdentity, kDown);
    CHECK(box && box->entity == wall);

    // After falling for a while the crate rests on the ground and the sweep distance changes accordingly.
    for (int i = 0; i < 240; ++i) scene.update(1.0f / 60.0f);
    CHECK_NEAR(crate.transform().position.y, 0.5f, 0.05f);
    hit = scene.sweepSphere({0, 10, 0}, 0.5f, kDown);
    CHECK(hit && hit->entity == crate);
    if (hit) CHECK_NEAR(hit->distance, 8.5f, 0.06f);
    scene.stopSimulation();
}

} // namespace

int main() {
    Log::init();
    Log::core().set_level(spdlog::level::warn);

    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"sphere sweep hits ground", sphereSweepHitsGround},
        {"sweep misses / max distance", sweepMissesAndRespectsMaxDistance},
        {"box and capsule sweeps", boxAndCapsuleSweeps},
        {"side hit normal", sideHitNormal},
        {"sweepAll sorted + ignore filter", sweepAllSortedAndFiltered},
        {"sensors are opt-in", sensorsAreOptIn},
        {"sweepBody ignores itself", sweepBodyIgnoresItself},
        {"overlapping at start", overlappingAtStart},
        {"sweepAgainst body list", sweepAgainstBodyList},
        {"scene sweepAgainst entities", sceneSweepAgainstEntities},
        {"scene-level queries", sceneLevelQueries},
    };
    for (auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    Log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
