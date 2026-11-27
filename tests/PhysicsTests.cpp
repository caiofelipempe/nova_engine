// Headless physics tests (no window / GPU). Run: ctest --preset ... or NovaTests.exe
#include <nova/Nova.h>
#include <nova/physics/backend/LocalPhysicsWorld.h> // backend-level float contract (Jolt)

#include <cmath>
#include <cstdio>
#include <functional>
#include <optional>
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

constexpr f32 kEps = 0.02f;
const glm::quat kIdentity{1, 0, 0, 0};
const glm::vec3 kDown{0, -1, 0};

// Static ground: a 100x1x100 box whose top face is at y = 0.
BodyId addGround(local::World& w) {
    local::BodyDesc d;
    d.type = BodyType::Static;
    d.shape.halfExtents = {50.0f, 0.5f, 50.0f};
    d.position = {0.0f, -0.5f, 0.0f};
    d.userData = 1000;
    return w.createBody(d);
}

BodyId addBox(local::World& w, glm::vec3 pos, BodyType type = BodyType::Static, u64 userData = 0, bool sensor = false) {
    local::BodyDesc d;
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
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    local::SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, kDown, 100.0f, hit));
    CHECK_NEAR(hit.distance, 4.5f, kEps); // centre stops 0.5 above the floor
    CHECK_NEAR(hit.point.y, 0.0f, kEps);
    CHECK_NEAR(hit.normal.y, 1.0f, kEps);
    CHECK(hit.userData == 1000);
}

void sweepMissesAndRespectsMaxDistance() {
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    local::SweepHit hit;
    CHECK(!w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, {0, 1, 0}, 100.0f, hit)); // away from the floor
    CHECK(!w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, kDown, 4.0f, hit));       // too short
    CHECK(!w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, glm::vec3(0.0f), 10.0f, hit)); // zero direction
}

void boxAndCapsuleSweeps() {
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    ShapeDesc box;
    box.type = ShapeType::Box;
    box.halfExtents = {1.0f, 0.25f, 1.0f};
    local::SweepHit hit;
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
    auto w = local::World::create(Backend::Jolt);
    addBox(*w, {5, 0, 0}, BodyType::Static, 7);
    local::SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, hit));
    CHECK_NEAR(hit.distance, 4.0f, kEps); // box face at x=4.5, sphere radius 0.5
    CHECK_NEAR(hit.normal.x, -1.0f, kEps);
    CHECK_NEAR(hit.point.x, 4.5f, kEps);
    CHECK(hit.userData == 7);
}

void sweepAllSortedAndFiltered() {
    auto w = local::World::create(Backend::Jolt);
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
    local::SweepHit hit;
    CHECK(w->sweep(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, hit, ignoreFirst));
    CHECK(hit.userData == 2);

    local::RaycastHit ray;
    CHECK(w->raycast({0, 0, 0}, {1, 0, 0}, 20.0f, ray, ignoreFirst));
    CHECK(ray.userData == 2);
    CHECK_NEAR(ray.distance, 5.5f, kEps);
}

void sensorsAreOptIn() {
    auto w = local::World::create(Backend::Jolt);
    addBox(*w, {3, 0, 0}, BodyType::Static, 1, /*sensor*/ true);
    addBox(*w, {6, 0, 0}, BodyType::Static, 2);
    local::SweepHit hit;
    CHECK(w->sweep(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, hit));
    CHECK(hit.userData == 2);
    QueryFilter withSensors;
    withSensors.includeSensors = true;
    CHECK(w->sweep(sphere(0.25f), {0, 0, 0}, kIdentity, {1, 0, 0}, 20.0f, hit, withSensors));
    CHECK(hit.userData == 1);
}

void sweepBodyIgnoresItself() {
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    BodyId box = addBox(*w, {0, 3, 0}, BodyType::Dynamic, 5);
    local::SweepHit hit;
    CHECK(w->sweepBody(box, kDown, 10.0f, hit));
    CHECK_NEAR(hit.distance, 2.5f, kEps);
    CHECK(hit.userData == 1000);
    CHECK(!w->sweepBody(box, {0, 1, 0}, 10.0f, hit)); // nothing above, and never itself
}

void overlappingAtStart() {
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    local::SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), {0, 0.2f, 0}, kIdentity, kDown, 5.0f, hit));
    CHECK_NEAR(hit.fraction, 0.0f, 1e-4f);
}

// ------------------------------------------------------------------ normals --
const glm::vec3 kUp{0, 1, 0};
const f32 kInvSqrt2 = 0.70710678f;

bool isAxisAligned(const glm::vec3& n) {
    return std::abs(std::max({std::abs(n.x), std::abs(n.y), std::abs(n.z)}) - 1.0f) < kEps;
}

void sweepNormalsOnFace() {
    // Sphere onto a flat face: every normal agrees.
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    local::SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), {0, 5, 0}, kIdentity, kDown, 100.0f, hit));
    CHECK_VEC(hit.normal, kUp, kEps);
    CHECK_VEC(hit.hitSurfaceNormal, kUp, kEps);
    CHECK_VEC(hit.sweptSurfaceNormal, -kUp, kEps);
    CHECK_VEC(hit.point, glm::vec3(0.0f), kEps);
    CHECK_VEC(hit.sweptPoint, glm::vec3(0.0f), kEps); // bottom of the sphere at the moment of impact
}

void sweepNormalsOnEdge() {
    // Sphere grazing the top edge of a box: the contact normal is diagonal, the box face is not.
    auto w = local::World::create(Backend::Jolt);
    addBox(*w, {0, 0, 0});
    local::SweepHit hit;
    CHECK(w->sweep(sphere(0.25f), {-3, 0.6f, 0}, kIdentity, {1, 0, 0}, 10.0f, hit));
    CHECK(hit.normal.x < -0.3f && hit.normal.y > 0.2f);           // diagonal, back towards the sphere
    CHECK(isAxisAligned(hit.hitSurfaceNormal));                     // a face normal of the box
    CHECK(glm::dot(hit.normal, hit.hitSurfaceNormal) < 0.97f);      // ...that differs from the contact normal
    CHECK_VEC(hit.sweptSurfaceNormal, -hit.normal, 0.05f);          // a sphere's normal is always along the contact
    CHECK(glm::length(hit.sweptPoint - hit.point) < 0.05f);         // touching
}

void sweepNormalsTiltedBoxCorner() {
    // Box rotated 45deg around Z swept down: it lands on an edge. The ground normal is up, but the
    // box face that touched is tilted 45deg — only sweptSurfaceNormal reveals that.
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    ShapeDesc box;
    box.type = ShapeType::Box;
    box.halfExtents = glm::vec3(0.5f);
    glm::quat tilt = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1));
    local::SweepHit hit;
    CHECK(w->sweep(box, {0, 3, 0}, tilt, kDown, 10.0f, hit));
    CHECK_VEC(hit.normal, kUp, kEps);
    CHECK_VEC(hit.hitSurfaceNormal, kUp, kEps);
    CHECK_NEAR(hit.sweptSurfaceNormal.y, -kInvSqrt2, 0.05f);
    CHECK_NEAR(std::abs(hit.sweptSurfaceNormal.x), kInvSqrt2, 0.05f);
    CHECK_NEAR(hit.sweptSurfaceNormal.z, 0.0f, kEps);
    CHECK_NEAR(hit.sweptPoint.y, 0.0f, 0.03f);
}

void sweepNormalsCubeOnVertex() {
    // Cube with its body diagonal pointing straight down lands on a single vertex where three faces
    // meet. Each of them is tilted equally (normal.y = -1/sqrt(3)); any is a valid answer, a side face is not.
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    ShapeDesc box;
    box.type = ShapeType::Box;
    box.halfExtents = glm::vec3(0.5f);
    glm::quat onVertex = glm::rotation(glm::normalize(glm::vec3(1, 1, 1)), glm::vec3(0, -1, 0));
    local::SweepHit hit;
    CHECK(w->sweep(box, {0, 3, 0}, onVertex, kDown, 10.0f, hit));
    CHECK_VEC(hit.hitSurfaceNormal, kUp, kEps);
    CHECK_NEAR(hit.sweptSurfaceNormal.y, -0.57735f, 0.05f);
    CHECK_NEAR(glm::length(hit.sweptSurfaceNormal), 1.0f, 1e-3f);
}

void sweepBodyNormals() {
    // sweepBody uses the body's real shape and pose: same tilted-box case through a body.
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    local::BodyDesc d;
    d.type = BodyType::Dynamic;
    d.shape.halfExtents = glm::vec3(0.5f);
    d.position = {0, 3, 0};
    d.rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1));
    BodyId box = w->createBody(d);
    local::SweepHit hit;
    CHECK(w->sweepBody(box, kDown, 10.0f, hit));
    CHECK_VEC(hit.hitSurfaceNormal, kUp, kEps);
    CHECK_NEAR(hit.sweptSurfaceNormal.y, -kInvSqrt2, 0.05f);
}

void raycastNormals() {
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    local::RaycastHit hit;
    CHECK(w->raycast({0, 5, 0}, kDown, 10.0f, hit));
    CHECK_VEC(hit.normal, kUp, kEps);
    CHECK_VEC(hit.hitSurfaceNormal, kUp, kEps);
    CHECK_VEC(hit.sweptSurfaceNormal, glm::vec3(0.0f), 1e-6f); // rays have no surface
    CHECK_VEC(hit.sweptPoint, glm::vec3(0.0f), 1e-6f);
}

std::optional<local::ContactEvent> stepUntilContact(local::World& w, int maxSteps = 180) {
    for (int i = 0; i < maxSteps; ++i) {
        w.step(1.0f / 60.0f);
        auto events = w.drainContactEvents();
        if (!events.empty()) return events.front();
    }
    return std::nullopt;
}

// Returns the event with body A being the one with `userDataA` (swapping sides flips the normal).
local::ContactEvent orient(local::ContactEvent e, u64 userDataA) {
    if (e.userDataA == userDataA) return e;
    std::swap(e.bodyA, e.bodyB);
    std::swap(e.userDataA, e.userDataB);
    std::swap(e.pointA, e.pointB);
    std::swap(e.surfaceNormalA, e.surfaceNormalB);
    e.normal = -e.normal;
    return e;
}

void contactNormalsSphere() {
    auto w = local::World::create(Backend::Jolt);
    addGround(*w); // userData 1000
    local::BodyDesc d;
    d.shape.type = ShapeType::Sphere;
    d.shape.radius = 0.5f;
    d.position = {0, 2, 0};
    d.userData = 9;
    w->createBody(d);

    auto ev = stepUntilContact(*w);
    CHECK(ev.has_value());
    if (!ev) return;
    local::ContactEvent e = orient(*ev, 1000); // A = ground, B = sphere
    CHECK(e.userDataB == 9);
    CHECK_VEC(e.normal, kUp, kEps); // from A (ground) towards B (sphere)
    CHECK_VEC(e.surfaceNormalA, kUp, kEps);
    CHECK_VEC(e.surfaceNormalB, -kUp, kEps);
    CHECK_NEAR(e.pointA.y, 0.0f, 0.05f);
    CHECK_NEAR(e.pointB.y, 0.0f, 0.05f);
}

void contactNormalsTiltedBox() {
    auto w = local::World::create(Backend::Jolt);
    addGround(*w);
    local::BodyDesc d;
    d.shape.halfExtents = glm::vec3(0.5f);
    d.position = {0, 2, 0};
    d.rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0, 0, 1));
    d.userData = 9;
    w->createBody(d);

    auto ev = stepUntilContact(*w);
    CHECK(ev.has_value());
    if (!ev) return;
    local::ContactEvent e = orient(*ev, 9); // A = box, B = ground
    // The reported point sits at an end of the landing edge (a vertex of the box): a point-based
    // normal there is ambiguous (it used to return the +Z side face). The face towards the ground must win.
    CHECK_VEC(e.normal, -kUp, 0.05f);                     // from the box down into the ground
    CHECK_VEC(e.surfaceNormalB, kUp, kEps);               // ground face
    CHECK_NEAR(e.surfaceNormalA.y, -kInvSqrt2, 0.05f);    // tilted box face
    CHECK_NEAR(std::abs(e.surfaceNormalA.x), kInvSqrt2, 0.05f);
}

struct RecordCollision : Script {
    std::optional<Collision> first;
    void onCollision(const Collision& c) override {
        if (!first) first = c;
    }
};

void scriptCollisionNormals() {
    Scene scene("test");
    Entity ground = scene.createEntity("Ground");
    ground.transform().position = {0, -0.5f, 0};
    ground.transform().scale = {100, 1, 100};
    ground.add<RigidBodyComponent>(BodyType::Static);

    Entity ball = scene.createEntity("Ball");
    ball.transform().position = {0, 2, 0};
    ball.add<RigidBodyComponent>();
    ball.add<SphereColliderComponent>();
    auto& rec = ball.addScript<RecordCollision>();
    auto& groundRec = ground.addScript<RecordCollision>();

    scene.startSimulation();
    for (int i = 0; i < 180 && !rec.first; ++i) scene.update(1.0f / 60.0f);

    CHECK(rec.first.has_value() && groundRec.first.has_value());
    if (rec.first) {
        const Collision& c = *rec.first; // seen from the ball
        CHECK(c.other == ground);
        CHECK_VEC(c.normal, kUp, kEps); // pushes the ball up, away from the ground
        CHECK_VEC(c.surfaceNormal, -kUp, kEps);
        CHECK_VEC(c.otherSurfaceNormal, kUp, kEps);
        CHECK_NEAR(c.point.toMeters().y, 0.0f, 0.05f);
    }
    if (groundRec.first) {
        const Collision& c = *groundRec.first; // seen from the ground
        CHECK(c.other == ball);
        CHECK_VEC(c.normal, -kUp, kEps); // pushes the ground down, away from the ball
        CHECK_VEC(c.surfaceNormal, kUp, kEps);
        CHECK_VEC(c.otherSurfaceNormal, -kUp, kEps);
    }

    // Scene-level sweep forwards all normals.
    auto hit = scene.sweepSphere({0, 5, 3}, 0.5f, kDown);
    CHECK(hit && hit->entity == ground);
    if (hit) {
        CHECK_VEC(hit->hitSurfaceNormal, kUp, kEps);
        CHECK_VEC(hit->sweptSurfaceNormal, -kUp, kEps);
    }
    scene.stopSimulation();
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
    CHECK_NEAR(crate.transform().position.toMeters().y, 0.5f, 0.05f);
    hit = scene.sweepSphere({0, 10, 0}, 0.5f, kDown);
    CHECK(hit && hit->entity == crate);
    if (hit) CHECK_NEAR(hit->distance, 8.5f, 0.06f);
    scene.stopSimulation();
}

// ------------------------------------------------------------ floating origin --
// Ground box (top face at base.y), a ball above it and a camera, all around `base`.
struct FarScene {
    Scene scene{"far"};
    Entity ground, ball, camera;
    explicit FarScene(const GlobalPosition& base) {
        ground = scene.createEntity("Ground");
        ground.transform().position = base + glm::vec3(0, -0.5f, 0);
        ground.transform().scale = {100, 1, 100};
        ground.add<RigidBodyComponent>(BodyType::Static);
        ball = scene.createEntity("Ball");
        ball.transform().position = base + glm::vec3(0, 2, 0);
        ball.add<RigidBodyComponent>();
        ball.add<SphereColliderComponent>(); // radius 0.5
        camera = scene.createEntity("Camera");
        camera.add<CameraComponent>();
        camera.transform().position = base + glm::vec3(0, 3, 10);
    }
};

void farFromOrigin() {
    // Near the far corner of the world, where absolute floats are coarsest; the simulation runs
    // around the camera.
    const GlobalPosition base(7900.0, 0.0, -7900.0);
    FarScene f(base);
    f.scene.startSimulation();
    CHECK(f.scene.worldOrigin() == WorldOrigin::snapped(f.camera.transform().position)); // on its grid
    for (int i = 0; i < 180; ++i) f.scene.update(Duration::perSecond(60));
    const glm::dvec3 rest = (f.ball.transform().position - base).toMeters();
    CHECK_NEAR(rest.y, 0.5, 0.02); // resting on the ground
    CHECK_NEAR(rest.x, 0.0, 1e-3);
    CHECK_NEAR(rest.z, 0.0, 1e-3);

    // Queries take and return exact positions.
    auto hit = f.scene.raycast(base + glm::vec3(3, 10, 0), {0, -1, 0}, 50.0f);
    CHECK(hit && hit->entity == f.ground);
    if (hit) CHECK_NEAR((hit->point - base).toMeters().y, 0.0, 1e-3);
    auto sweep = f.scene.sweepSphere(base + glm::vec3(0, 10, 0), 0.25f, {0, -1, 0}, 50.0f);
    CHECK(sweep && sweep->entity == f.ball);
    if (sweep) CHECK_NEAR((sweep->point - base).toMeters().y, 1.0, 0.02); // top of the resting ball
}

void rebase() {
    const GlobalPosition base(2500.0, 0.0, 0.0);
    FarScene f(base);
    f.ball.get<RigidBodyComponent>().gravityFactor = 0.0f;
    f.ball.get<RigidBodyComponent>().linearDamping = 0.0f; // measure the floating origin, not damping
    f.scene.startSimulation();
    f.scene.update(Duration::perSecond(60));
    f.scene.setLinearVelocity(f.ball, {3.0f, 0.0f, 0.0f});
    f.scene.update(Duration::perSecond(60));
    const GlobalPosition before = f.ball.transform().position;
    const GlobalPosition groundBefore = f.ground.transform().position;

    // The focus jumps 5 km: every body shifts, the WorldOrigin follows (to its grid), nothing moves.
    const GlobalPosition focus = base + glm::vec3(5000.0f, 0.0f, 0.0f);
    f.scene.setPhysicsFocus(focus);
    f.scene.update(Duration::perSecond(60));
    CHECK(f.scene.worldOrigin() == WorldOrigin::snapped(focus));
    const f64 moved = (f.ball.transform().position - before).toMeters().x;
    // One more step at 3 m/s, velocity kept. The ball is now 5 km from the physics origin, where
    // floats are 0.5 mm apart: the shift itself rounds by up to half of that.
    CHECK_NEAR(moved, 3.0 / 60.0, 5e-4);
    CHECK(f.ground.transform().position == groundBefore); // static: exactly where it was
    CHECK_NEAR(f.scene.linearVelocity(f.ball).toGlm().x, 3.0f, 1e-4f);

    // Far from the focus, precision drops (a 5 cm step rounds to 102 float steps of 0.49 mm)...
    GlobalPosition mark = f.ball.transform().position;
    for (int i = 0; i < 60; ++i) f.scene.update(Duration::perSecond(60));
    const f64 farError = std::abs((f.ball.transform().position - mark).toMeters().x - 3.0);
    CHECK(farError > 1e-3 && farError < 0.05);
    // ...and comes back when the focus does: a new rebase near the ball, and 60 steps are exact.
    f.scene.clearPhysicsFocus();
    f.scene.update(Duration::perSecond(60));
    CHECK(f.scene.worldOrigin() == WorldOrigin::snapped(f.camera.transform().position));
    mark = f.ball.transform().position;
    for (int i = 0; i < 60; ++i) f.scene.update(Duration::perSecond(60));
    CHECK_NEAR((f.ball.transform().position - mark).toMeters().x, 3.0, 2e-4);
}

void wrapSeam() {
    Scene scene("loop");
    const f64 seam = (WorldSpace::kDefaultSize / 2); // +8 192 m ≡ −8 192 m
    // A box just past the seam and a ball just before it rolling towards it: with the physics
    // focus at the seam they are neighbours for the physics engine.
    Entity box = scene.createEntity("Box");
    box.transform().position = {-seam + 1.0, 0.0, 0.0};
    box.add<RigidBodyComponent>(BodyType::Static).restitution = 0.0f;
    Entity ball = scene.createEntity("Ball");
    ball.transform().position = {seam - 3.0, 0.0, 0.0};
    auto& ballBody = ball.add<RigidBodyComponent>();
    ballBody.gravityFactor = 0.0f;
    ballBody.restitution = 0.0f;
    ball.add<SphereColliderComponent>();
    scene.setPhysicsFocus(GlobalPosition(seam, 0.0, 0.0));
    scene.startSimulation();
    scene.setLinearVelocity(ball, {10.0f, 0.0f, 0.0f});
    for (int i = 0; i < 60; ++i) scene.update(Duration::perSecond(60));
    // Stopped by the box across the seam: box face 0.5 m past it, ball radius 0.5 → center on it.
    const GlobalOffset fromBox = ball.transform().position - box.transform().position;
    CHECK_NEAR(fromBox.toMeters().x, -1.0, 0.05);
    CHECK(std::abs(ball.transform().position.toMeters().x) > seam - 1.0);

    // A free ball crosses the seam and keeps going on the other side.
    Entity free = scene.createEntity("Free");
    free.transform().position = {seam - 2.0, 5.0, 0.0};
    auto& freeBody = free.add<RigidBodyComponent>();
    freeBody.gravityFactor = 0.0f;
    freeBody.linearDamping = 0.0f;
    free.add<SphereColliderComponent>();
    scene.update(Duration::perSecond(60)); // creates the body
    scene.setLinearVelocity(free, {10.0f, 0.0f, 0.0f});
    for (int i = 0; i < 30; ++i) scene.update(Duration::perSecond(60));
    CHECK_NEAR(free.transform().position.toMeters().x, -seam + 3.0, 0.1); // 5 m on, around the torus
}

void determinism() {
    // A pile of boxes and balls, saved once and loaded twice: the two simulations must agree
    // bit for bit at every step (lockstep / replays), rebases included.
    Scene source("source");
    Entity ground = source.createEntity("Ground");
    ground.transform().position = {0.0, -0.5, 0.0};
    ground.transform().scale = {60, 1, 60};
    ground.add<RigidBodyComponent>(BodyType::Static);
    for (int i = 0; i < 24; ++i) {
        Entity e = source.createEntity("Body " + std::to_string(i));
        e.transform().position = {static_cast<f64>(i % 4) * 0.9 - 1.3, 0.6 + (i / 4) * 1.05, static_cast<f64>(i % 3) * 0.3};
        e.transform().setEuler({0.1f * i, 0.2f * i, 0.0f});
        e.add<RigidBodyComponent>();
        if (i % 2) e.add<SphereColliderComponent>();
    }
    const std::string json = serializeScene(source);

    Scene a("a"), b("b");
    CHECK(deserializeScene(a, json) && deserializeScene(b, json));
    CHECK(a.stateHash() == b.stateHash());
    a.startSimulation();
    b.startSimulation();
    bool same = true;
    for (int i = 0; i < 240; ++i) {
        if (i == 120) { // the focus jumps: both recenter their window at the same step
            a.setPhysicsFocus({3000.0, 0.0, 0.0});
            b.setPhysicsFocus({3000.0, 0.0, 0.0});
        }
        a.update(Duration::perSecond(60));
        b.update(Duration::perSecond(60));
        same &= a.stateHash() == b.stateHash();
    }
    CHECK(same);
    // And the hash does notice a difference.
    b.setLinearVelocity(b.findEntity("Body 5"), {0.0f, 1e-3f, 0.0f});
    b.update(Duration::perSecond(60));
    a.update(Duration::perSecond(60));
    CHECK(a.stateHash() != b.stateHash());
}

// The same simulation gives the same bits in every build and on every platform: Jolt is built with
// CROSS_PLATFORM_DETERMINISTIC (overlay port) and the state is fixed point. The reference hash was
// recorded once; Debug and Release must both reach it (a change here means the simulation changed:
// update the value only on purpose).
constexpr u64 kReferenceHash = 0xe583f9e48159e25cull; // Windows x64, MSVC, Debug and Release

void crossPlatformDeterminism() {
    Scene s("reference");
    auto add = [&](const char* name, u64 uuid) {
        Entity e = s.createEntity(name);
        e.get<IdComponent>().uuid = uuid;
        return e;
    };
    Entity ground = add("Ground", 1);
    ground.transform().position = {0.0, -0.5, 0.0};
    ground.transform().scale = {40, 1, 40};
    ground.add<RigidBodyComponent>(BodyType::Static);
    for (int i = 0; i < 16; ++i) {
        Entity e = add("Body", 100 + static_cast<u64>(i));
        e.transform().position = {static_cast<f64>(i % 4) * 0.9 - 1.3, 0.6 + (i / 4) * 1.05, static_cast<f64>(i % 3) * 0.3};
        e.transform().rotate(Angle::fromRadians(0.3 * i), ScalarVec(1.0, 2.0, 0.0));
        e.add<RigidBodyComponent>().friction = 0.4f + 0.02f * static_cast<f32>(i);
        if (i % 2) e.add<SphereColliderComponent>().radius = 0.45f;
    }
    s.startSimulation();
    for (int i = 0; i < 180; ++i) s.update(Duration::perSecond(60));
    const u64 h = s.stateHash();
    std::printf("  reference hash: 0x%016llx\n", static_cast<unsigned long long>(h));
    CHECK(h == kReferenceHash);
}

// ------------------------------------------------------------- more shapes --
// Meshes the scene's colliders read (as Renderer::meshData would provide).
struct TestMeshes {
    MeshData cube = MeshData::cube();
    MeshData plane = MeshData::plane(20.0f, 4);
    MeshData tetra;
    TestMeshes() {
        tetra.vertices = {{{0, 1, 0}}, {{-0.8f, -0.5f, 0.5f}}, {{0.8f, -0.5f, 0.5f}}, {{0, -0.5f, -0.9f}}};
        tetra.indices = {0, 1, 2, 0, 2, 3, 0, 3, 1, 1, 3, 2};
    }
    Scene::MeshSource source() {
        return [this](MeshHandle h) -> const MeshData* {
            return h.id == 1 ? &cube : h.id == 2 ? &plane : h.id == 3 ? &tetra : nullptr;
        };
    }
};

f64 heightOf(Entity e) { return e.transform().position.toMeters().y; }

void moreShapes() {
    TestMeshes meshes;
    Scene scene("shapes");
    scene.setMeshSource(meshes.source());
    // Ground: an exact triangle mesh (static), from the 20 m plane mesh.
    Entity ground = scene.createEntity("Ground");
    ground.add<MeshComponent>(MeshHandle{2});
    ground.add<RigidBodyComponent>(BodyType::Static);
    ground.add<MeshColliderComponent>(); // empty mesh = the entity's MeshComponent
    // A cylinder standing up, a 2×1×2 box as the convex hull of the scaled cube mesh, a tetrahedron hull.
    Entity cyl = scene.createEntity("Cylinder");
    cyl.transform().position = {-4.0, 3.0, 0.0};
    cyl.add<RigidBodyComponent>();
    cyl.add<CylinderColliderComponent>().halfHeight = 1.0f;
    Entity hull = scene.createEntity("Hull");
    hull.transform().position = {0.0, 3.0, 0.0};
    hull.transform().scale = {2.0f, 1.0f, 2.0f};
    hull.add<MeshComponent>(MeshHandle{1});
    hull.add<RigidBodyComponent>();
    hull.add<ConvexHullColliderComponent>();
    Entity tetra = scene.createEntity("Tetra");
    tetra.transform().position = {4.0, 3.0, 0.0};
    tetra.add<RigidBodyComponent>();
    tetra.add<ConvexHullColliderComponent>().mesh = MeshHandle{3};
    // A dynamic body with a MeshCollider falls back to its convex hull.
    Entity fallback = scene.createEntity("Fallback");
    fallback.transform().position = {0.0, 3.0, 5.0};
    fallback.add<MeshComponent>(MeshHandle{1});
    fallback.add<RigidBodyComponent>();
    fallback.add<MeshColliderComponent>();

    scene.startSimulation();
    for (int i = 0; i < 240; ++i) scene.update(Duration::perSecond(60));
    CHECK(scene.physicsBodyCount() == 5);
    CHECK_NEAR(heightOf(cyl), 1.0, 0.03);      // half height 1 on the mesh ground
    CHECK_NEAR(heightOf(hull), 0.5, 0.03);     // cube scaled to 2×1×2: half height 0.5
    CHECK_NEAR(heightOf(fallback), 0.5, 0.03); // hull of the cube
    CHECK(heightOf(tetra) > 0.3 && heightOf(tetra) < 1.0); // resting on a face of the tetrahedron
    CHECK(std::abs(glm::length(cyl.transform().up()) - 1.0f) < 1e-4f && cyl.transform().up().y > 0.99f); // still upright

    // Queries hit the triangle mesh exactly.
    auto hit = scene.raycast(GlobalPosition(7.0, 5.0, 7.0), {0, -1, 0}, 20.0f);
    CHECK(hit && hit->entity == ground);
    if (hit) {
        CHECK_NEAR(hit->point.toMeters().y, 0.0, 1e-4);
        CHECK_VEC(hit->normal, glm::vec3(0, 1, 0), 1e-4f);
    }
    // A sweep with a hull shape (any ShapeDesc works in queries too).
    physics::ShapeDesc probe;
    probe.type = ShapeType::ConvexHull;
    auto geometry = std::make_shared<physics::ShapeGeometry>();
    for (const Vertex& v : meshes.tetra.vertices) geometry->points.push_back(v.position * 0.5f);
    probe.geometry = geometry;
    auto swept = scene.sweep(probe, GlobalPosition(0.0, 6.0, 0.0), glm::quat(1, 0, 0, 0), {0, -1, 0}, 20.0f);
    CHECK(swept && swept->entity == hull);
}

void meshShapesFarAway() {
    // The same mesh ground and hull 10,000 km out: shapes stay local, only bodies are global.
    TestMeshes meshes;
    const GlobalPosition base = GlobalPosition(7500.0, 0.0, -7500.0);
    Scene scene("far shapes");
    scene.setMeshSource(meshes.source());
    Entity cam = scene.createEntity("Camera");
    cam.add<CameraComponent>();
    cam.transform().position = base + glm::vec3(0, 3, 10);
    Entity ground = scene.createEntity("Ground");
    ground.transform().position = base;
    ground.add<MeshComponent>(MeshHandle{2});
    ground.add<RigidBodyComponent>(BodyType::Static);
    ground.add<MeshColliderComponent>();
    Entity hull = scene.createEntity("Hull");
    hull.transform().position = base + glm::vec3(1, 3, 0);
    hull.add<MeshComponent>(MeshHandle{1});
    hull.add<RigidBodyComponent>();
    hull.add<ConvexHullColliderComponent>();
    scene.startSimulation();
    for (int i = 0; i < 180; ++i) scene.update(Duration::perSecond(60));
    const glm::dvec3 rest = (hull.transform().position - base).toMeters();
    CHECK_NEAR(rest.y, 0.5, 0.03);
    CHECK_NEAR(rest.x, 1.0, 1e-3);
}

// A backend that works in global positions natively (like a custom integer engine would):
// implements PhysicsWorld directly — no floating origin, no float positions anywhere.
// Bodies fall under gravity, integrated as exact integer offsets; no collisions.
class IntegerTestWorld final : public PhysicsWorld {
public:
    struct Body {
        BodyDesc desc;
        glm::vec3 velocity{0.0f};
        bool alive = true;
    };
    std::vector<Body> bodies;
    GlobalPosition lastFocus;
    glm::vec3 g{0.0f, -10.0f, 0.0f};

    BodyId createBody(const BodyDesc& d) override {
        bodies.push_back({d, d.linearVelocity});
        return {static_cast<u32>(bodies.size() - 1)};
    }
    void destroyBody(BodyId b) override { bodies[b.value].alive = false; }
    u32 bodyCount() const override { return static_cast<u32>(bodies.size()); }
    void setTransform(BodyId b, const GlobalPosition& p, const glm::quat& q) override {
        bodies[b.value].desc.position = p;
        bodies[b.value].desc.rotation = q;
    }
    void getTransform(BodyId b, GlobalPosition& p, glm::quat& q) const override {
        p = bodies[b.value].desc.position;
        q = bodies[b.value].desc.rotation;
    }
    void moveKinematic(BodyId b, const GlobalPosition& p, const glm::quat& q, Duration) override { setTransform(b, p, q); }
    glm::vec3 linearVelocity(BodyId b) const override { return bodies[b.value].velocity; }
    void setLinearVelocity(BodyId b, const glm::vec3& v) override { bodies[b.value].velocity = v; }
    glm::vec3 angularVelocity(BodyId) const override { return {}; }
    void setAngularVelocity(BodyId, const glm::vec3&) override {}
    void addForce(BodyId, const glm::vec3&) override {}
    void addImpulse(BodyId, const glm::vec3&) override {}
    void addTorque(BodyId, const glm::vec3&) override {}
    bool isActive(BodyId b) const override { return bodies[b.value].desc.type == BodyType::Dynamic; }
    void setGravity(const glm::vec3& gravity) override { g = gravity; }
    glm::vec3 gravity() const override { return g; }
    void setCollisionLayers(const CollisionLayers&) override {}
    void setBodyProperties(BodyId, const BodyDesc&) override {}
    CharacterId createCharacter(const CharacterDesc&) override { return {}; }
    void destroyCharacter(CharacterId) override {}
    void setCharacterVelocity(CharacterId, const glm::vec3&) override {}
    void setCharacterTransform(CharacterId, const GlobalPosition&, const glm::quat&) override {}
    CharacterState characterState(CharacterId) const override { return {}; }
    JointId createJoint(const JointDesc&) override { return {}; }
    void destroyJoint(JointId) override {}
    void setJointMotor(JointId, const JointMotor&) override {}
    f32 jointPosition(JointId) const override { return 0.0f; }
    bool isJointBroken(JointId) const override { return false; }
    std::vector<JointBreakEvent> drainJointBreaks() override { return {}; }
    std::vector<OverlapHit> overlap(const ShapeDesc&, const GlobalPosition&, const glm::quat&,
                                    const QueryFilter&) const override { return {}; }
    void step(Duration dt) override {
        // Semi-implicit Euler with the step taken in exact integer offsets.
        for (Body& b : bodies) {
            if (!b.alive || b.desc.type != BodyType::Dynamic) continue;
            b.velocity += g * b.desc.gravityFactor * dt.secondsF();
            b.desc.position += GlobalOffset::fromMeters(glm::dvec3(b.velocity) * dt.seconds());
        }
    }
    void setFocus(const GlobalPosition& f) override { lastFocus = f; }
    bool raycast(const GlobalPosition&, const glm::vec3&, f32, RaycastHit&, const QueryFilter&) const override { return false; }
    bool sweep(const ShapeDesc&, const GlobalPosition&, const glm::quat&, const glm::vec3&, f32, SweepHit&,
               const QueryFilter&) const override { return false; }
    std::vector<SweepHit> sweepAll(const ShapeDesc&, const GlobalPosition&, const glm::quat&, const glm::vec3&, f32,
                                   const QueryFilter&) const override { return {}; }
    bool sweepBody(BodyId, const glm::vec3&, f32, SweepHit&, const QueryFilter&) const override { return false; }
    std::vector<ContactEvent> drainContactEvents() override { return {}; }
};

void adapterRegressions() {
    // Invalid shapes are refused up front.
    auto w = PhysicsWorld::create(Backend::Jolt);
    w->setFocus(GlobalPosition());
    BodyDesc bad;
    bad.shape.type = ShapeType::Sphere;
    bad.shape.radius = 0.0f;
    CHECK(!w->createBody(bad).valid());
    BodyDesc noPoints;
    noPoints.shape.type = ShapeType::ConvexHull;
    CHECK(!w->createBody(noPoints).valid());
    CHECK(w->bodyCount() == 0);

    // A sweep whose contact lands exactly on the region origin still reports it (it used to be
    // replaced by the query origin, as if it were a raycast).
    BodyDesc ground;
    ground.type = BodyType::Static;
    ground.shape.halfExtents = {50.0f, 0.5f, 50.0f};
    ground.position = GlobalPosition(0.0, -0.5, 0.0);
    CHECK(w->createBody(ground).valid());
    SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), GlobalPosition(0.0, 5.0, 0.0), kIdentity, kDown, 10.0f, hit));
    CHECK_NEAR(hit.sweptPoint.toMeters().y, 0.0, 0.02);
    CHECK_NEAR(hit.point.toMeters().y, 0.0, 0.02);
    RaycastHit ray;
    CHECK(w->raycast(GlobalPosition(0.0, 5.0, 0.0), kDown, 10.0f, ray));
    CHECK(ray.sweptPoint == GlobalPosition(0.0, 5.0, 0.0)); // rays: the origin
}

void nativeIntegerBackend() {
    // The scene feeds a custom backend global positions only — 10,000 km out, exactly.
    const GlobalPosition base = GlobalPosition(7500.0, 1000.0, -7500.0);
    Scene scene("native");
    Entity cam = scene.createEntity("Camera");
    cam.add<CameraComponent>();
    cam.transform().position = base + glm::vec3(0, 2, 10);
    Entity ball = scene.createEntity("Ball");
    ball.transform().position = base;
    ball.add<RigidBodyComponent>();
    ball.add<SphereColliderComponent>();

    auto world = makeScope<IntegerTestWorld>();
    IntegerTestWorld* raw = world.get();
    scene.startSimulation(std::move(world));
    CHECK(scene.physics() == raw && raw->bodies.size() == 1);
    CHECK(raw->bodies[0].desc.position == base); // exact, no float on the way in
    CHECK(raw->bodies[0].desc.shape.type == ShapeType::Sphere && raw->bodies[0].desc.shape.radius == 0.5f); // local shape
    CHECK(raw->lastFocus == cam.transform().position);

    for (int i = 0; i < 60; ++i) scene.update(Duration::perSecond(60));
    // The scene's gravity (−9.81, not the backend's own −10) applies; fell g/2 · t² · (1 + 1/60)
    // (semi-implicit Euler), written back exactly from the backend.
    CHECK(raw->g == SpeedVec(0.0, -9.81, 0.0).toGlm()); // the Q15.16 gravity, as the engine's float
    const f64 fallen = (ball.transform().position - base).toMeters().y;
    CHECK_NEAR(fallen, -9.81 / 2.0 * (61.0 / 60.0), 1e-3);
    CHECK(ball.transform().position == raw->bodies[0].desc.position);
    CHECK((ball.transform().position - base).x == 0 && (ball.transform().position - base).z == 0); // no drift sideways
    CHECK(scene.worldOrigin() == GlobalPosition()); // no window
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
        {"sweep normals: sphere on face", sweepNormalsOnFace},
        {"sweep normals: sphere on box edge", sweepNormalsOnEdge},
        {"sweep normals: tilted box on corner", sweepNormalsTiltedBoxCorner},
        {"sweep normals: cube on vertex", sweepNormalsCubeOnVertex},
        {"sweepBody normals", sweepBodyNormals},
        {"raycast normals", raycastNormals},
        {"contact normals: sphere", contactNormalsSphere},
        {"contact normals: tilted box", contactNormalsTiltedBox},
        {"script Collision normals", scriptCollisionNormals},
        {"scene-level queries", sceneLevelQueries},
        {"floating origin: 10,000 km from the world origin", farFromOrigin},
        {"floating origin: rebase keeps positions and velocities", rebase},
        {"wrap seam: neighbours across it collide", wrapSeam},
        {"determinism: two runs agree bit for bit", determinism},
        {"determinism: the reference hash (every build, every platform)", crossPlatformDeterminism},
        {"global contract: a native integer backend plugs in directly", nativeIntegerBackend},
        {"adapter: invalid shapes refused, sweptPoint on the region origin", adapterRegressions},
        {"shapes: cylinder, convex hull, triangle mesh", moreShapes},
        {"shapes: mesh ground and hull 10,000 km away", meshShapesFarAway},
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
