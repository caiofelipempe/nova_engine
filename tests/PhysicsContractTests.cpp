// PhysicsWorld contract tests: the same checks run against every backend, through the global
// interface only (what the Scene sees). What a backend does not declare in caps() is skipped, so a
// new backend (e.g. a custom integer engine grown from Backend::Skeleton) passes from day one and
// gains checks as it declares more. Run: ctest --preset ... or NovaPhysicsContractTests.exe
#include <nova/Nova.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::physics;

namespace {

int g_failures = 0;
int g_checks = 0;
int g_skipped = 0;

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
// Skips the rest of a test when the backend does not declare what it needs.
#define REQUIRE_CAP(cond)    \
    do {                     \
        if (!(cond)) {       \
            ++g_skipped;     \
            throw Skip{#cond}; \
        }                    \
    } while (0)

struct Skip {
    const char* what;
};

struct Restore {
    ~Restore() { WorldSpace::setActive(WorldSpace()); }
};

const glm::quat kIdentity{1, 0, 0, 0};
const Duration kStep = Duration::perSecond(60);
// 10,000 km out: absolute floats are 1 m apart here, so everything must stay global.
const GlobalPosition kBase = GlobalPosition::fromUnits({10'000, 0, -10'000}, LengthUnit::Kilometer);

// A world with its focus at `focus` (floating-origin backends place their region there).
Scope<PhysicsWorld> makeWorld(Backend backend, const GlobalPosition& focus = kBase, f64 radius = 4096.0) {
    auto w = PhysicsWorld::create(backend);
    SimulationFocus f;
    f.foci = {focus};
    f.radius = radius;
    w->setFocus(f);
    return w;
}

ShapeDesc sphere(f32 r = 0.5f) {
    ShapeDesc s;
    s.type = ShapeType::Sphere;
    s.radius = r;
    return s;
}
ShapeDesc box(glm::vec3 half) {
    ShapeDesc s;
    s.type = ShapeType::Box;
    s.halfExtents = half;
    return s;
}

BodyDesc body(const ShapeDesc& shape, const GlobalPosition& p, BodyType type = BodyType::Dynamic, u64 userData = 0) {
    BodyDesc d;
    d.shape = shape;
    d.position = p;
    d.type = type;
    d.userData = userData;
    d.linearDamping = 0.0f;
    d.angularDamping = 0.0f;
    return d;
}

// Static 100 × 1 × 100 box whose top face is at `top`.y.
BodyId addGround(PhysicsWorld& w, const GlobalPosition& top, u64 userData = 1000) {
    BodyDesc d = body(box({50.0f, 0.5f, 50.0f}), top + glm::vec3(0, -0.5f, 0), BodyType::Static, userData);
    d.restitution = 0.0f;
    return w.createBody(d);
}

GlobalPosition positionOf(const PhysicsWorld& w, BodyId b) {
    GlobalPosition p;
    glm::quat q;
    w.getTransform(b, p, q);
    return p;
}

void run(PhysicsWorld& w, int steps) {
    for (int i = 0; i < steps; ++i) w.step(kStep);
}

// ----------------------------------------------------------------- bodies --
void bodiesAndIds(Backend backend) {
    auto w = makeWorld(backend);
    const BodyId a = w->createBody(body(sphere(), kBase));
    const BodyId b = w->createBody(body(box({1, 1, 1}), kBase + glm::vec3(5, 0, 0), BodyType::Static));
    CHECK(a.valid() && b.valid() && !(a == b));
    CHECK(w->bodyCount() == 2);
    w->destroyBody(b);
    CHECK(w->bodyCount() == 1);
    w->destroyBody(BodyId{}); // invalid ids are ignored
    CHECK(w->bodyCount() == 1);
    // Invalid shapes are refused.
    CHECK(!w->createBody(body(sphere(0.0f), kBase)).valid());
    ShapeDesc mesh;
    mesh.type = ShapeType::Mesh;
    CHECK(!w->createBody(body(mesh, kBase)).valid());
    CHECK(w->bodyCount() == 1);
}

void exactPoses(Backend backend) {
    // Static poses go in and come out exactly, far from the world origin; moving bodies too on
    // native integer backends (float backends: to float precision around their origin).
    auto w = makeWorld(backend);
    const bool exact = w->caps().globalPositions;
    const GlobalPosition p = kBase + GlobalOffset(123'456'789, -987'654'321, 42);
    const BodyId s = w->createBody(body(box({1, 1, 1}), p, BodyType::Static));
    const BodyId d = w->createBody(body(sphere(), p + glm::vec3(0, 10, 0)));
    CHECK(positionOf(*w, s) == p);
    const GlobalPosition q = p + glm::vec3(0, 10, 0) + GlobalOffset(1, 2, 3); // a few world steps off
    w->setTransform(d, q, kIdentity);
    CHECK(exact ? positionOf(*w, d) == q : isWithin(positionOf(*w, d), q, 1e-6));
    w->setGravity({0, 0, 0});
    run(*w, 10);
    CHECK(positionOf(*w, s) == p);      // static: never moves
    CHECK(isWithin(positionOf(*w, d), q, 1e-6)); // at rest, no gravity: stays (to float precision)
}

// ----------------------------------------------------------------- motion --
void freeFall(Backend backend) {
    auto w = makeWorld(backend);
    CHECK_VEC(w->gravity(), glm::vec3(0, -9.81f, 0), 1e-6f);
    const BodyId b = w->createBody(body(sphere(), kBase));
    run(*w, 60);
    // Semi-implicit Euler: fallen = g/2 · t² · (1 + 1/60).
    const glm::dvec3 moved = (positionOf(*w, b) - kBase).toMeters();
    CHECK_NEAR(moved.y, -9.81 / 2.0 * (61.0 / 60.0), 2e-3);
    CHECK_NEAR(moved.x, 0.0, 1e-6);
    CHECK_NEAR(moved.z, 0.0, 1e-6);
    CHECK_NEAR(w->linearVelocity(b).y, -9.81f, 1e-3f);
    CHECK(w->isActive(b));
}

void velocitiesForcesImpulses(Backend backend) {
    auto w = makeWorld(backend);
    w->setGravity({0, 0, 0});
    CHECK_VEC(w->gravity(), glm::vec3(0.0f), 0.0f);
    BodyDesc d = body(sphere(), kBase);
    d.mass = 2.0f;
    const BodyId b = w->createBody(d);
    w->setLinearVelocity(b, {2, 0, 0});
    CHECK_VEC(w->linearVelocity(b), glm::vec3(2, 0, 0), 1e-5f);
    run(*w, 60); // 1 s at 2 m/s
    CHECK_NEAR((positionOf(*w, b) - kBase).toMeters().x, 2.0, 1e-3);

    w->addImpulse(b, {0, 4, 0}); // Δv = J / m, right away
    CHECK_VEC(w->linearVelocity(b), glm::vec3(2, 2, 0), 1e-4f);
    w->addForce(b, {0, 0, 120}); // Δv = F / m · dt, during the next step
    w->step(kStep);
    CHECK_VEC(w->linearVelocity(b), glm::vec3(2, 2, 1), 1e-3f);
    w->step(kStep); // forces last one step
    CHECK_VEC(w->linearVelocity(b), glm::vec3(2, 2, 1), 1e-3f);

    w->setAngularVelocity(b, {0, 3, 0});
    CHECK_VEC(w->angularVelocity(b), glm::vec3(0, 3, 0), 1e-5f);
    w->setAngularVelocity(b, {0, 0, 0});
    w->addTorque(b, {0, 1, 0});
    w->step(kStep);
    CHECK(w->angularVelocity(b).y > 0.0f); // spins up around +Y
}

void gravityFactorAndStatic(Backend backend) {
    auto w = makeWorld(backend);
    BodyDesc floating = body(sphere(), kBase);
    floating.gravityFactor = 0.0f;
    const BodyId f = w->createBody(floating);
    const BodyId s = w->createBody(body(sphere(), kBase + glm::vec3(10, 0, 0), BodyType::Static));
    run(*w, 30);
    CHECK(isWithin(positionOf(*w, f), kBase, 1e-6));
    CHECK(positionOf(*w, s) == kBase + glm::vec3(10, 0, 0));
    CHECK(!w->isActive(s));
    w->setGravity({0, 0, 0});
    BodyDesc heavy = body(sphere(), kBase + glm::vec3(0, 5, 0));
    heavy.gravityFactor = 2.0f;
    const BodyId h = w->createBody(heavy);
    w->setGravity({0, -1, 0});
    run(*w, 60);
    CHECK_NEAR(w->linearVelocity(h).y, -2.0f, 1e-3f);
}

void kinematic(Backend backend) {
    auto w = makeWorld(backend);
    const BodyId k = w->createBody(body(box({0.5f, 0.5f, 0.5f}), kBase, BodyType::Kinematic));
    const GlobalPosition target = kBase + glm::vec3(1, 0, 0);
    const glm::quat turn = glm::angleAxis(0.1f, glm::vec3(0, 1, 0));
    w->moveKinematic(k, target, turn, kStep);
    CHECK_NEAR(w->linearVelocity(k).x, 60.0f, 1e-2f); // derived: 1 m in 1/60 s
    w->step(kStep);
    GlobalPosition p;
    glm::quat q;
    w->getTransform(k, p, q);
    CHECK(isWithin(p, target, 1e-4));
    CHECK(std::abs(glm::dot(q, turn)) > 0.9999f);
}

// ------------------------------------------------------------- collisions --
void sphereLandsOnBox(Backend backend) {
    auto w = makeWorld(backend);
    REQUIRE_CAP(w->caps().collidesWith(ShapeType::Sphere, ShapeType::Box));
    const BodyId ground = addGround(*w, kBase);
    const BodyId ball = w->createBody(body(sphere(), kBase + glm::vec3(0, 2, 0), BodyType::Dynamic, 7));
    std::vector<ContactEvent> events;
    for (int i = 0; i < 180; ++i) {
        w->step(kStep);
        for (const ContactEvent& e : w->drainContactEvents()) events.push_back(e);
    }
    const glm::dvec3 rest = (positionOf(*w, ball) - kBase).toMeters();
    CHECK_NEAR(rest.y, 0.5, 0.02);
    CHECK_NEAR(rest.x, 0.0, 1e-3);
    CHECK(!events.empty());
    if (!events.empty()) {
        const ContactEvent& e = events.front();
        const bool groundIsA = e.bodyA == ground;
        CHECK((groundIsA && e.bodyB == ball) || (e.bodyA == ball && e.bodyB == ground));
        CHECK((groundIsA ? e.userDataB : e.userDataA) == 7);
        CHECK((groundIsA ? e.userDataA : e.userDataB) == 1000);
        // Normal from A to B: up when the ground is A.
        CHECK_VEC(e.normal, glm::vec3(0, groundIsA ? 1.0f : -1.0f, 0), 0.05f);
        const GlobalPosition groundPoint = groundIsA ? e.pointA : e.pointB;
        CHECK_NEAR((groundPoint - kBase).toMeters().y, 0.0, 0.03);
        CHECK_VEC(groundIsA ? e.surfaceNormalA : e.surfaceNormalB, glm::vec3(0, 1, 0), 0.05f);
    }
    CHECK(w->drainContactEvents().empty()); // draining cleared them
}

void spheresCollide(Backend backend) {
    auto w = makeWorld(backend);
    REQUIRE_CAP(w->caps().collidesWith(ShapeType::Sphere, ShapeType::Sphere));
    w->setGravity({0, 0, 0});
    BodyDesc da = body(sphere(), kBase + glm::vec3(-2, 0, 0), BodyType::Dynamic, 1);
    BodyDesc db = body(sphere(), kBase + glm::vec3(2, 0, 0), BodyType::Dynamic, 2);
    da.restitution = db.restitution = 0.0f;
    da.linearVelocity = {3, 0, 0};
    db.linearVelocity = {-3, 0, 0};
    const BodyId a = w->createBody(da);
    const BodyId b = w->createBody(db);
    std::vector<ContactEvent> events;
    for (int i = 0; i < 60; ++i) {
        w->step(kStep);
        for (const ContactEvent& e : w->drainContactEvents()) events.push_back(e);
    }
    // Equal masses, inelastic: both stop, touching.
    const f64 gap = distance(positionOf(*w, a), positionOf(*w, b)) - 1.0;
    CHECK_NEAR(gap, 0.0, 0.03);
    CHECK(glm::length(w->linearVelocity(a)) < 0.05f && glm::length(w->linearVelocity(b)) < 0.05f);
    CHECK(events.size() == 1);
    if (!events.empty()) {
        const ContactEvent& e = events.front();
        const GlobalPosition pa = positionOf(*w, e.bodyA), pb = positionOf(*w, e.bodyB);
        const glm::vec3 aToB = glm::normalize((pb - pa).toVec3());
        CHECK(glm::dot(e.normal, aToB) > 0.99f);
        CHECK(glm::dot(e.surfaceNormalA, aToB) > 0.99f && glm::dot(e.surfaceNormalB, -aToB) > 0.99f);
        CHECK(distance(e.pointA, e.pointB) < 0.05);
    }
}

void sensors(Backend backend) {
    auto w = makeWorld(backend);
    REQUIRE_CAP(w->caps().collidesWith(ShapeType::Sphere, ShapeType::Box));
    BodyDesc trigger = body(box({2, 2, 2}), kBase, BodyType::Static, 50);
    trigger.isSensor = true;
    const BodyId t = w->createBody(trigger);
    const BodyId ball = w->createBody(body(sphere(), kBase + glm::vec3(0, 4, 0), BodyType::Dynamic, 51));
    bool reported = false;
    for (int i = 0; i < 120; ++i) {
        w->step(kStep);
        for (const ContactEvent& e : w->drainContactEvents())
            reported |= (e.bodyA == t && e.bodyB == ball) || (e.bodyA == ball && e.bodyB == t);
    }
    CHECK(reported);
    CHECK((positionOf(*w, ball) - kBase).toMeters().y < -2.0); // fell straight through
}

// ---------------------------------------------------------------- queries --
void raycasts(Backend backend) {
    auto w = makeWorld(backend);
    const PhysicsCaps c = w->caps();
    REQUIRE_CAP((c.raycastShapes & shapeBit(ShapeType::Sphere)) && (c.raycastShapes & shapeBit(ShapeType::Box)));
    addGround(*w, kBase);
    const BodyId ball = w->createBody(body(sphere(), kBase + glm::vec3(10, 3, 0), BodyType::Static, 5));
    RaycastHit hit;
    const GlobalPosition from = kBase + glm::vec3(0, 5, 0);
    CHECK(w->raycast(from, {0, -2, 0}, 10.0f, hit)); // direction need not be normalized
    CHECK(hit.userData == 1000 && hit.body.valid());
    CHECK_NEAR(hit.distance, 5.0f, 1e-3f);
    CHECK_NEAR(hit.fraction, 0.5f, 1e-3f);
    CHECK_NEAR((hit.point - kBase).toMeters().y, 0.0, 1e-3);
    CHECK_VEC(hit.normal, glm::vec3(0, 1, 0), 1e-3f);
    CHECK_VEC(hit.hitSurfaceNormal, glm::vec3(0, 1, 0), 1e-3f);
    CHECK(hit.sweptPoint == from);
    CHECK(!w->raycast(from, {0, -1, 0}, 4.0f, hit)); // too short
    CHECK(!w->raycast(from, {0, 1, 0}, 100.0f, hit)); // nothing above
    CHECK(!w->raycast(from, {0, 0, 0}, 100.0f, hit)); // no direction

    // Sideways into the ball: hits its surface 0.5 before its center.
    CHECK(w->raycast(kBase + glm::vec3(0, 3, 0), {1, 0, 0}, 20.0f, hit));
    CHECK(hit.body == ball && hit.userData == 5);
    CHECK_NEAR(hit.distance, 9.5f, 1e-3f);
    CHECK_VEC(hit.normal, glm::vec3(-1, 0, 0), 1e-3f);
    // Ignoring it, the ray goes on.
    QueryFilter ignore;
    ignore.ignoreBody = ball;
    CHECK(!w->raycast(kBase + glm::vec3(0, 3, 0), {1, 0, 0}, 20.0f, hit, ignore));

    // Sensors are opt-in.
    BodyDesc s = body(box({0.5f, 0.5f, 0.5f}), kBase + glm::vec3(4, 3, 0), BodyType::Static, 6);
    s.isSensor = true;
    w->createBody(s);
    CHECK(w->raycast(kBase + glm::vec3(0, 3, 0), {1, 0, 0}, 20.0f, hit) && hit.userData == 5);
    QueryFilter withSensors;
    withSensors.includeSensors = true;
    CHECK(w->raycast(kBase + glm::vec3(0, 3, 0), {1, 0, 0}, 20.0f, hit, withSensors) && hit.userData == 6);
    CHECK_NEAR(hit.distance, 3.5f, 1e-3f);

    // Destroyed bodies are gone from queries.
    w->destroyBody(ball);
    CHECK(!w->raycast(kBase + glm::vec3(0, 3, 0), {1, 0, 0}, 20.0f, hit));
}

void sphereSweeps(Backend backend) {
    auto w = makeWorld(backend);
    const PhysicsCaps c = w->caps();
    REQUIRE_CAP((c.sweepShapes & shapeBit(ShapeType::Sphere)) && (c.sweepTargets & shapeBit(ShapeType::Box)) &&
                (c.sweepTargets & shapeBit(ShapeType::Sphere)));
    addGround(*w, kBase);
    const GlobalPosition from = kBase + glm::vec3(0, 5, 0);
    SweepHit hit;
    CHECK(w->sweep(sphere(0.5f), from, kIdentity, {0, -1, 0}, 100.0f, hit));
    CHECK(hit.userData == 1000);
    CHECK_NEAR(hit.distance, 4.5f, 0.02f); // center stops 0.5 above the floor
    CHECK_NEAR((hit.point - kBase).toMeters().y, 0.0, 0.02);
    CHECK_VEC(hit.normal, glm::vec3(0, 1, 0), 0.02f);
    CHECK_VEC(hit.hitSurfaceNormal, glm::vec3(0, 1, 0), 0.02f);
    CHECK_NEAR((hit.sweptPoint - kBase).toMeters().y, 0.0, 0.02);
    CHECK_VEC(hit.sweptSurfaceNormal, glm::vec3(0, -1, 0), 0.02f);
    CHECK(!w->sweep(sphere(0.5f), from, kIdentity, {0, -1, 0}, 4.0f, hit));

    // Several bodies in a row: sweepAll reports each once, closest first.
    for (int i = 1; i <= 3; ++i)
        w->createBody(body(sphere(0.5f), kBase + glm::vec3(3.0f * i, 2, 0), BodyType::Static, static_cast<u64>(i)));
    const auto hits = w->sweepAll(sphere(0.25f), kBase + glm::vec3(0, 2, 0), kIdentity, {1, 0, 0}, 20.0f);
    CHECK(hits.size() == 3);
    if (hits.size() == 3) {
        CHECK(hits[0].userData == 1 && hits[1].userData == 2 && hits[2].userData == 3);
        CHECK_NEAR(hits[0].distance, 2.25f, 0.02f);
        CHECK(hits[0].distance < hits[1].distance && hits[1].distance < hits[2].distance);
    }

    // sweepBody uses the body's own shape and never hits itself.
    const BodyId self = w->createBody(body(sphere(0.5f), kBase + glm::vec3(-10, 3, 0), BodyType::Dynamic, 9));
    CHECK(w->sweepBody(self, {0, -1, 0}, 10.0f, hit));
    CHECK(hit.userData == 1000);
    CHECK_NEAR(hit.distance, 2.5f, 0.02f);
    CHECK(!w->sweepBody(self, {0, 1, 0}, 10.0f, hit));
}

// ------------------------------------------------------------ world space --
void barrierEdges(Backend backend) {
    Restore restore;
    WorldSpace::setActive(WorldSpace(WorldSpaceDesc{}.setUnit(1, LengthUnit::Millimeter)
                                         .setAxis(0, 100, LengthUnit::Meter, WorldBoundary::Barrier)));
    auto w = makeWorld(backend, GlobalPosition(40.0, 0.0, 0.0));
    REQUIRE_CAP(w->caps().raycastShapes & shapeBit(ShapeType::Sphere));
    BodyDesc d = body(sphere(), GlobalPosition(40.0, 0.0, 0.0));
    d.gravityFactor = 0.0f;
    d.restitution = 0.0f;
    d.linearVelocity = {20, 0, 0};
    const BodyId ball = w->createBody(d);
    std::vector<ContactEvent> events;
    for (int i = 0; i < 90; ++i) {
        w->step(kStep);
        for (const ContactEvent& e : w->drainContactEvents()) events.push_back(e);
    }
    // Stopped by the +X edge (50 m): the surface rests against it.
    CHECK_NEAR(positionOf(*w, ball).toMeters().x, 49.5, 0.05);
    bool edge = false;
    for (const ContactEvent& e : events) {
        const bool wallIsA = isWorldBoundary(e.userDataA);
        if (!wallIsA && !isWorldBoundary(e.userDataB)) continue;
        edge = true;
        const u64 code = wallIsA ? e.userDataA : e.userDataB;
        CHECK((code & 0xFF) == 1); // axis 0, high edge
        CHECK(!(wallIsA ? e.bodyA : e.bodyB).valid());
        CHECK(((wallIsA ? e.bodyB : e.bodyA)) == ball);
        CHECK_NEAR((wallIsA ? e.pointA : e.pointB).toMeters().x, 50.0, 0.05);
    }
    CHECK(edge);
    // Queries see the edges too, as hits without a body.
    RaycastHit hit;
    CHECK(w->raycast(GlobalPosition(40.0, 0.0, 3.0), {-1, 0, 0}, 100.0f, hit));
    CHECK(!hit.body.valid() && isWorldBoundary(hit.userData) && (hit.userData & 0xFF) == 0);
    CHECK_NEAR(hit.point.toMeters().x, -50.0, 0.01);
    CHECK_VEC(hit.normal, glm::vec3(1, 0, 0), 1e-3f);
}

void wrapSeam(Backend backend) {
    Restore restore;
    WorldSpace::setActive(WorldSpace(WorldSpaceDesc{}.setUnit(1, LengthUnit::Millimeter)
                                         .setAxis(0, 100, LengthUnit::Meter, WorldBoundary::Wrap)));
    auto w = makeWorld(backend, GlobalPosition(50.0, 0.0, 0.0));
    w->setGravity({0, 0, 0});
    // A free ball crosses the seam and keeps going on the other side: 48 + 5 m → −47.
    BodyDesc d = body(sphere(), GlobalPosition(48.0, 5.0, 0.0));
    d.linearVelocity = {10, 0, 0};
    const BodyId free = w->createBody(d);
    run(*w, 30);
    CHECK_NEAR(positionOf(*w, free).toMeters().x, -47.0, 0.01);
    CHECK_NEAR(w->linearVelocity(free).x, 10.0f, 1e-3f);

    // Neighbours across the seam collide.
    if (!w->caps().collidesWith(ShapeType::Sphere, ShapeType::Box)) return;
    BodyDesc wall = body(box({0.5f, 0.5f, 0.5f}), GlobalPosition(-49.0, 0.0, 0.0), BodyType::Static);
    wall.restitution = 0.0f;
    const BodyId boxId = w->createBody(wall);
    BodyDesc rolling = body(sphere(), GlobalPosition(47.0, 0.0, 0.0));
    rolling.restitution = 0.0f;
    rolling.linearVelocity = {10, 0, 0};
    const BodyId ball = w->createBody(rolling);
    run(*w, 60);
    // Box face at −49.5, radius 0.5: the center rests at −50 ≡ +50.
    CHECK_NEAR((positionOf(*w, ball) - positionOf(*w, boxId)).toMeters().x, -1.0, 0.05);
}

void dormancy(Backend backend) {
    auto w = makeWorld(backend, kBase, 100.0);
    REQUIRE_CAP(w->caps().dormancy);
    w->setGravity({0, 0, 0});
    BodyDesc d = body(sphere(), kBase);
    d.linearVelocity = {2, 0, 0};
    const BodyId b = w->createBody(d);
    w->step(kStep);
    CHECK(!w->isDormant(b));
    // The focus leaves: the body freezes, keeping its velocity...
    SimulationFocus away;
    away.foci = {kBase + glm::vec3(20'000.0f, 0, 0)};
    away.radius = 100.0;
    w->setFocus(away);
    w->step(kStep);
    CHECK(w->isDormant(b) && !w->isActive(b));
    const GlobalPosition frozen = positionOf(*w, b);
    run(*w, 60);
    CHECK(positionOf(*w, b) == frozen);
    CHECK_NEAR(w->linearVelocity(b).x, 2.0f, 1e-4f);
    // ...and resumes when the focus comes back.
    SimulationFocus back;
    back.foci = {kBase};
    back.radius = 100.0;
    w->setFocus(back);
    run(*w, 60);
    CHECK(!w->isDormant(b));
    CHECK_NEAR((positionOf(*w, b) - frozen).toMeters().x, 2.0, 0.01);
}

void globalPositions(Backend backend) {
    auto w = makeWorld(backend);
    REQUIRE_CAP(w->caps().globalPositions);
    // Native integer backends: one region at the world origin, wherever the action is.
    w->createBody(body(sphere(), kBase));
    w->step(kStep);
    CHECK(w->regionCount() == 1 && w->regionOrigin(0) == GlobalPosition());
    CHECK(!w->caps().regions);
}

void determinism(Backend backend) {
    // Same inputs, same results, bit for bit (lockstep, replays).
    auto build = [backend] {
        auto w = makeWorld(backend);
        addGround(*w, kBase);
        for (int i = 0; i < 16; ++i) {
            BodyDesc d = body(sphere(0.4f), kBase + glm::vec3((i % 4) * 0.7f - 1.0f, 0.5f + (i / 4) * 0.9f, (i % 3) * 0.2f));
            d.restitution = 0.3f;
            w->createBody(d);
        }
        return w;
    };
    auto a = build(), b = build();
    bool same = true;
    for (int s = 0; s < 120; ++s) {
        a->step(kStep);
        b->step(kStep);
        for (u32 i = 0; i < 17; ++i) {
            GlobalPosition pa, pb;
            glm::quat qa, qb;
            a->getTransform({i}, pa, qa);
            b->getTransform({i}, pb, qb);
            const glm::vec3 va = a->linearVelocity({i}), vb = b->linearVelocity({i});
            same &= pa == pb && std::memcmp(&qa, &qb, sizeof(qa)) == 0 && std::memcmp(&va, &vb, sizeof(va)) == 0;
        }
    }
    CHECK(same);
}

} // namespace

int main() {
    Log::init();
    Log::core().set_level(spdlog::level::critical); // invalid shapes are refused with an error on purpose

    const std::vector<std::pair<const char*, void (*)(Backend)>> tests = {
        {"bodies and ids", bodiesAndIds},
        {"exact poses far from the origin", exactPoses},
        {"free fall", freeFall},
        {"velocities, forces, impulses, torques", velocitiesForcesImpulses},
        {"gravity factor, gravity, static bodies", gravityFactorAndStatic},
        {"kinematic bodies", kinematic},
        {"sphere lands on a box (contact event)", sphereLandsOnBox},
        {"spheres collide head-on", spheresCollide},
        {"sensors report, never block", sensors},
        {"raycasts", raycasts},
        {"sphere sweeps", sphereSweeps},
        {"barrier edges", barrierEdges},
        {"wrap seam", wrapSeam},
        {"dormancy", dormancy},
        {"native global positions", globalPositions},
        {"determinism", determinism},
    };
    const std::vector<Backend> backends = {Backend::Jolt, Backend::Skeleton};
    for (Backend backend : backends) {
        const PhysicsCaps caps = PhysicsWorld::create(backend)->caps();
        std::printf("== %s\n", caps.name);
        for (auto& [name, fn] : tests) {
            const int before = g_failures;
            const char* skipped = nullptr;
            try {
                fn(backend);
            } catch (const Skip& s) {
                skipped = s.what;
            }
            if (skipped) std::printf("[SKIP] %s (needs %s)\n", name, skipped);
            else std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
        }
    }
    std::printf("\n%d checks, %d failed, %d skipped\n", g_checks, g_failures, g_skipped);
    Log::shutdown();
    return g_failures == 0 ? 0 : 1;
}
