// Headless tests of world positions: Q13.18 fixed point on the 16 km torus (exact arithmetic, the
// lap is the integer overflow, shortest differences, 64-bit squared lengths), conversions, metric
// units, helpers, scenes, files and camera-relative rendering.
#include <nova/Nova.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <vector>

using namespace nova;

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

bool near(f64 a, f64 b, f64 eps) { return std::abs(a - b) <= eps; }

constexpr i32 kMax = std::numeric_limits<i32>::max();
constexpr i32 kMin = std::numeric_limits<i32>::min();

// ------------------------------------------------------------------- tests --
void defaults() {
    CHECK(WorldSpace::kUnit == std::ldexp(1.0, -18)); // 3.8 µm
    CHECK(WorldSpace::kDefaultSize == 16384.0 && WorldSize{}.steps.x == (i64(1) << 32));
    // Q13.18: 1.5 m is exactly 1.5 · 2^18 steps; meters convert back exactly.
    GlobalPosition p = GlobalPosition::fromMeters({1.5, -2.25, 1000.0});
    CHECK(p.raw(0) == 393216 && p.raw(1) == -589824 && p.raw(2) == 262144000);
    CHECK(p.toMeters() == glm::dvec3(1.5, -2.25, 1000.0));
    CHECK(GlobalPosition() == GlobalPosition::fromMeters({0, 0, 0}));
    // Every raw value is a position, and its meters are exact.
    CHECK(GlobalPosition::fromRaw(kMax, kMin, 1).toMeters() == glm::dvec3(kMax * WorldSpace::kUnit, -8192.0, WorldSpace::kUnit));
    // Same bits as the fixed-point Length type.
    CHECK(p.toFixed().x == toFixed<Length>(1.5) && GlobalOffset::fromFixed(p.toFixed()) == GlobalOffset(p.raw()));
}

void exactness() {
    // a + (b - a) == b, bit for bit, for random points anywhere on the torus.
    std::mt19937 rng(1234);
    std::uniform_int_distribution<i32> any(kMin, kMax);
    bool exact = true, shortest = true;
    for (int i = 0; i < 20000; ++i) {
        const GlobalPosition a = GlobalPosition::fromRaw(any(rng), any(rng), any(rng));
        const GlobalPosition b = GlobalPosition::fromRaw(any(rng), any(rng), any(rng));
        exact &= a + (b - a) == b && b - (b - a) == a;
        // The offset is the shortest way: never more than half a lap on an axis.
        const glm::dvec3 d = (b - a).toMeters();
        for (int k = 0; k < 3; ++k) shortest &= std::abs(d[k]) <= (WorldSpace::kDefaultSize / 2);
    }
    CHECK(exact && shortest);
    // A walker taking a million steps of "1 mm" arrives exactly a million steps away: sums never
    // drift. (1 mm is 262.144 steps, rounded to 262: the step itself is what float would round.)
    GlobalPosition w;
    const GlobalOffset mm = GlobalOffset::fromMeters(glm::dvec3(0.001, 0, 0));
    CHECK(mm.x == 262);
    for (int i = 0; i < 1'000'000; ++i) w += mm;
    CHECK(w == GlobalPosition() + mm * 1'000'000 && w.raw(0) == 262'000'000);
}

void torus() {
    // The lap is the integer's own overflow: past +8 192 m comes −8 192 m.
    GlobalPosition edge = GlobalPosition::fromRaw(kMax, 0, 0);
    edge += GlobalOffset(1, 0, 0);
    CHECK(edge.raw(0) == kMin && near(edge.toMeters().x, -8192.0, 0.0));
    // Meters beyond the lap loop around, whole laps drop out.
    CHECK(GlobalPosition(8192.0 + 10.0, 0.0, 0.0) == GlobalPosition(-8192.0 + 10.0, 0.0, 0.0));
    CHECK(GlobalPosition(5 * 16384.0 + 1.25, 0.0, 0.0) == GlobalPosition(1.25, 0.0, 0.0));
    CHECK(GlobalPosition(-16384.0 * 3 - 2.0, 0.0, 0.0) == GlobalPosition(-2.0, 0.0, 0.0));
    CHECK(GlobalOffset::fromMeters(glm::dvec3(16384.0, 0, 0)) == GlobalOffset());
    // Across the seam, neighbours are near: the shortest way is 2 m, not 16 382 m.
    const GlobalPosition a(8191.0, 0.0, 0.0), b(-8191.0, 0.0, 0.0);
    CHECK(near((b - a).toMeters().x, 2.0, 1e-9) && near(distance(a, b), 2.0, 1e-9));
    CHECK(a + GlobalOffset::fromMeters(glm::dvec3(2.0, 0, 0)) == b);
    // Moving keeps looping, in every axis.
    GlobalPosition p(0.0, 8000.0, -8000.0);
    for (int i = 0; i < 100; ++i) p += glm::vec3(0.0f, 10.0f, -10.0f);
    CHECK(near(p.toMeters().y, 9000.0 - 16384.0, 1e-3) && near(p.toMeters().z, -9000.0 + 16384.0, 1e-3));
}

void offsets() {
    // Squared lengths are exact in 64 bits, even for the longest offset (a torus diagonal).
    const GlobalOffset longest(kMin, kMin, kMin);
    CHECK(longest.lengthSquared() == 3ull * (u64(1) << 62));
    CHECK(near(longest.length(), 8192.0 * std::sqrt(3.0), 1e-6));
    CHECK(GlobalOffset(3, 4, 0).lengthSquared() == 25 && compareLength(GlobalOffset(3, 4, 0), GlobalOffset(0, 0, 5)) == 0);
    CHECK(compareLength(GlobalOffset(3, 4, 0), GlobalOffset(0, 0, 6)) < 0);
    // isShorterThan is exact at the boundary.
    const GlobalOffset five(3, 4, 0); // 5 steps
    CHECK(five.isShorterThan(5.0001 * WorldSpace::kUnit) && !five.isShorterThan(5.0 * WorldSpace::kUnit));
    CHECK(longest.isShorterThan(20000.0) && !longest.isShorterThan(14000.0));
    // Offsets add and multiply around the lap.
    CHECK(GlobalOffset(kMax, 0, 0) + GlobalOffset(1, 0, 0) == GlobalOffset(kMin, 0, 0));
    CHECK(-GlobalOffset(kMin, 0, 0) == GlobalOffset(kMin, 0, 0)); // half a lap is both ways
    CHECK(GlobalOffset(-7, 6, 0) * -3 == GlobalOffset(21, -18, 0));
    const GlobalOffset h = GlobalOffset::fromMeters(glm::dvec3(10.0, -4.0, 0.5));
    CHECK(h.scaled(0.5) == GlobalOffset::fromMeters(glm::dvec3(5.0, -2.0, 0.25)) && h.scaled(1.0) == h);
}

void units() {
    CHECK(convertLength(2.5, LengthUnit::Kilometer, LengthUnit::Meter) == 2500.0);
    CHECK(near(convertLength(1.0, LengthUnit::Meter, LengthUnit::Nanometer), 1e9, 1e-3));
    CHECK(toMeters(25.0, LengthUnit::Centimeter) == 0.25);
    CHECK(fromMeters(1500.0, LengthUnit::Kilometer) == 1.5);
    CHECK(readableUnit(1500.0) == LengthUnit::Kilometer && readableUnit(0.02) == LengthUnit::Centimeter &&
          readableUnit(3e-8) == LengthUnit::Nanometer);
    CHECK(std::string(unitSymbol(LengthUnit::Millimeter)) == "mm");

    GlobalPosition p = GlobalPosition::fromUnits({1.5, 0.25, -2.0}, LengthUnit::Kilometer);
    CHECK(p == GlobalPosition::fromMeters({1500.0, 250.0, -2000.0}));
    const glm::dvec3 km = p.toUnits(LengthUnit::Kilometer);
    CHECK(near(km.x, 1.5, 1e-12) && near(km.y, 0.25, 1e-12));
    GlobalOffset d = GlobalOffset::fromUnits({0, 0, 250}, LengthUnit::Millimeter);
    CHECK(d == GlobalOffset::fromMeters(glm::dvec3(0, 0, 0.25)));
    CHECK(near(d.length(LengthUnit::Centimeter), 25.0, 1e-9));
    CHECK(near(d.toUnits(LengthUnit::Micrometer).z, 250'000.0, 1e-6));
    CHECK(near(distance(GlobalPosition(), p, LengthUnit::Kilometer), std::sqrt(1.5 * 1.5 + 0.25 * 0.25 + 4.0), 1e-9));
}

void helpers() {
    GlobalPosition a = GlobalPosition::fromMeters({7000.0, 0, 0});
    GlobalPosition b = GlobalPosition::fromMeters({7010.0, 0, 0});
    CHECK(lerp(a, b, 0.0) == a && lerp(a, b, 1.0) == b);
    CHECK(lerp(a, b, 0.5) == GlobalPosition::fromMeters({7005.0, 0, 0}));
    CHECK(moveTowards(a, b, 4.0) == GlobalPosition::fromMeters({7004.0, 0, 0}));
    CHECK(moveTowards(a, b, 100.0) == b);
    CHECK(isWithin(a, b, 10.5) && !isWithin(a, b, 9.5));

    GlobalPosition r = rotateAround(b, a, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
    const glm::dvec3 off = (r - a).toMeters();
    CHECK(near(off.x, 0.0, 1e-5) && near(off.z, -10.0, 1e-5)); // +X rotated 90° about +Y → −Z

    // Across the seam, lerp takes the short way.
    GlobalPosition e = GlobalPosition::fromMeters({8191.0, 0, 0}), f = GlobalPosition::fromMeters({-8191.0, 0, 0});
    CHECK(lerp(e, f, 0.5) == GlobalPosition::fromRaw(kMin, 0, 0)); // the seam, 1 m from each
}

void worldSizes() {
    // Any size: exact relations and shortest offsets, in a tiny world, the default one and a big one.
    for (const f64 size : {48.0, WorldSpace::kDefaultSize, 1.0e6}) {
        ScopedWorldSize scope(WorldSize::cube(size));
        const i64 lap = WorldSize::active().steps.x, half = lap / 2;
        std::mt19937_64 rng(77);
        std::uniform_int_distribution<i64> any(-half, half - 1);
        bool exact = true, shortest = true, inside = true;
        for (int i = 0; i < 20000; ++i) {
            const GlobalPosition a = GlobalPosition::fromRaw(any(rng), any(rng), any(rng));
            const GlobalPosition b = GlobalPosition::fromRaw(any(rng), any(rng), any(rng));
            const GlobalOffset d = b - a;
            exact &= a + d == b && b - d == a;
            for (int k = 0; k < 3; ++k) {
                shortest &= d[k] >= -half && d[k] < half;
                inside &= (a + d * 3).raw(k) >= -half && (a + d * 3).raw(k) < half;
            }
        }
        CHECK(exact && shortest && inside);
        // Leaving one side enters the other: from just below +size/2, one more step is −size/2.
        GlobalPosition edge = GlobalPosition::fromRaw(half - 1, 0, 0);
        edge += GlobalOffset(1, 0, 0);
        CHECK(edge.raw(0) == -half && edge.toMeters().x == -size / 2);
        // Meters loop: a whole lap is nothing, a lap and a half is the opposite side.
        CHECK(GlobalPosition(size, 0.0, 0.0) == GlobalPosition() && GlobalOffset::fromMeters(glm::dvec3(size * 1.5, 0, 0)).x == -half);
    }

    // The 1000 km world: across the seam, two points 200 km apart (the long way is 800 km).
    {
        ScopedWorldSize scope(WorldSize::cube(1.0e6));
        const GlobalPosition east(400'000.0, 0.0, 0.0), west(-400'000.0, 0.0, 0.0);
        CHECK((west - east).toMeters().x == 200'000.0);
        CHECK(distance(east, west) == 200'000.0);
        CHECK(lerp(east, west, 0.5) == GlobalPosition(-500'000.0, 0.0, 0.0)); // the seam, the short way
        CHECK(!(west - east).isShort() && (west - east).lengthSquared() == ~u64(0));
        CHECK(compareLength(west - east, GlobalOffset::fromMeters(glm::dvec3(199'999.0, 0, 0))) > 0);
        CHECK(isWithin(east, west, 200'001.0) && !isWithin(east, west, 199'999.0));
    }

    // The largest world (2⁴⁰ m per axis): positions beyond a billion meters, exact in steps.
    {
        ScopedWorldSize scope(WorldSize::cube(WorldSpace::kMaxSize));
        const GlobalPosition far = GlobalPosition::fromRaw(i64(1) << 56, -(i64(1) << 56), 12345);
        CHECK(far.toMeters().x == 274877906944.0); // 2⁵⁶ steps = 2³⁸ m
        CHECK(far + (GlobalPosition() - far) == GlobalPosition());
        CHECK((far - GlobalPosition()) * 4 == GlobalOffset(0, 0, 12345 * 4)); // 2⁵⁸ is a whole lap
    }

    // Integer multiples loop around without overflow.
    {
        ScopedWorldSize scope(WorldSize::cube(48.0));
        const GlobalOffset m = GlobalOffset::fromMeters(glm::dvec3(1.0, 0, 0));
        CHECK(m * 50 == GlobalOffset::fromMeters(glm::dvec3(2.0, 0, 0)) && m * -50 == GlobalOffset::fromMeters(glm::dvec3(-2.0, 0, 0)));
        CHECK(m * 4'000'000'000'000'000 == GlobalOffset::fromMeters(glm::dvec3(4e15 - 48.0 * 83'333'333'333'333, 0, 0)));
    }
}

void worldOrigin() {
    // Default world: cells of 256 m; the origin moves when the focus is more than 160 m away.
    CHECK(WorldOrigin::cellSteps(0) == (i64(1) << 26));
    CHECK(WorldOrigin::snapped(GlobalPosition(130.0, 0.0, -130.0)) == GlobalPosition(256.0, 0.0, -256.0));
    CHECK(WorldOrigin::snapped(GlobalPosition(127.0, 5.0, -127.0)) == GlobalPosition());
    WorldOrigin o;
    CHECK(!o.needsMove(GlobalPosition(150.0, -150.0, 150.0)));
    CHECK(o.needsMove(GlobalPosition(170.0, 0.0, 0.0)));
    const glm::vec3 shift = o.follow(GlobalPosition(170.0, 0.0, 0.0));
    CHECK(shift == glm::vec3(256.0f, 0.0f, 0.0f) && o.position() == GlobalPosition(256.0, 0.0, 0.0)); // exact
    CHECK(o.follow(GlobalPosition(120.0, 0.0, 0.0)) == glm::vec3(0.0f)); // hysteresis: back across the border, no move
    CHECK(o.toGlobal(o.toLocal(GlobalPosition(300.5, 2.0, 0.0))) == GlobalPosition(300.5, 2.0, 0.0));

    // Far away: the origin is an exact i64 position; local floats are small.
    WorldOrigin far(GlobalPosition(8000.0, 0.0, -8000.0));
    CHECK(far.position() == GlobalPosition(7936.0, 0.0, -7936.0)); // 31 cells
    CHECK(far.toLocal(GlobalPosition(8000.0, 1.0, -8000.0)) == glm::vec3(64.0f, 1.0f, -64.0f));

    // Across the seam of the default world: the shift is the short way.
    WorldOrigin seam(GlobalPosition(7950.0, 0.0, 0.0)); // 7 936 m
    const glm::vec3 across = seam.follow(GlobalPosition(-8150.0, 0.0, 0.0));
    CHECK(across.x == 256.0f && seam.position() == GlobalPosition(-8192.0, 0.0, 0.0));

    // A tiny world: cells of an eighth of the axis at most (48 m → 4 m), so the seam stays far.
    ScopedWorldSize tiny(WorldSize::cube(48.0));
    CHECK(WorldOrigin::cellSteps(0) == (i64(1) << 20));
    WorldOrigin t;
    CHECK(t.follow(GlobalPosition(23.0, 0.0, 0.0)) == glm::vec3(-24.0f, 0.0f, 0.0f)); // +24 m ≡ −24 m: either way
    CHECK(t.position() == GlobalPosition(-24.0, 0.0, 0.0)); // +24 m is −24 m in this world
    CHECK(t.toLocal(GlobalPosition(-23.0, 0.0, 0.0)) == glm::vec3(1.0f, 0.0f, 0.0f));
}

void sceneIntegration() {
    // The transform stores a GlobalPosition; meters assign naturally.
    Scene a("Far");
    Entity near0 = a.createEntity("Near");
    near0.transform().position = {1, 2, 3};
    Entity far = a.createEntity("Far");
    far.transform().position = GlobalPosition(8191.999, 0.5, -8191.25);
    far.transform().position += GlobalOffset(1, 0, 0); // one step (3.8 µm): still exact in the file
    Entity odd = a.createEntity("Odd");
    odd.transform().position = GlobalPosition::fromRaw(1234567890, kMin, 7); // not a short decimal

    const std::string json = serializeScene(a);
    CHECK(json.find("\"world\"") != std::string::npos);
    Scene b;
    CHECK(deserializeScene(b, json));
    CHECK(b.findEntity("Near").transform().position == GlobalPosition(1, 2, 3));
    CHECK(b.findEntity("Far").transform().position == far.transform().position);
    CHECK(b.findEntity("Odd").transform().position == odd.transform().position);
    CHECK(serializeScene(b) == json);

    // Older files load by their meters: plain arrays, and {raw, meters} with steps of another unit.
    Scene old;
    const char* legacy = R"({"format": "nova.scene", "version": 1, "name": "Old",
        "world": {"unit": 2.3283064365386963e-10, "axes": [{"size": 0, "boundary": "Barrier"}, {}, {}]},
        "entities": [
        {"components": {"Id": {"uuid": 5}, "Name": {"name": "Crate"},
                        "Transform": {"position": [1.5, 0.25, -3.0], "rotation": [0, 0, 0], "scale": [1, 1, 1]}}},
        {"components": {"Id": {"uuid": 6}, "Name": {"name": "Raw"},
                        "Transform": {"position": {"raw": [4294967296, 0, 0], "meters": [1.0, 0.0, 0.0]}}}}]})";
    CHECK(deserializeScene(old, legacy));
    CHECK(old.findEntity("Crate").transform().position == GlobalPosition(1.5, 0.25, -3.0));
    CHECK(old.findEntity("Raw").transform().position == GlobalPosition(1.0, 0.0, 0.0));

    // Prefabs store offsets from the first entity: placed anywhere, the layout stays exact (also
    // across the seam).
    Scene d;
    Entity p0 = d.createEntity("P0"), p1 = d.createEntity("P1");
    p0.transform().position = {100.0, 0.0, 0.0};
    p1.transform().position = GlobalPosition(100.0, 0.0, 0.0) + GlobalOffset(1, 2, 3);
    const std::string prefab = serializePrefab({p0, p1});
    const GlobalPosition spot = GlobalPosition::fromRaw(kMax, 1, -500);
    auto placed = instantiatePrefab(d, prefab, spot);
    CHECK(placed.size() == 2 && placed[0].transform().position == spot);
    CHECK(placed.size() == 2 && placed[1].transform().position - placed[0].transform().position == GlobalOffset(1, 2, 3));

    // Matrices relative to a nearby origin keep precision that the absolute float matrix loses.
    TransformComponent t;
    t.position = GlobalPosition(8000.0, 0.0, 0.0) + GlobalOffset::fromMeters(glm::dvec3(0.0001, 0, 0));
    const GlobalPosition camera(8000.0, 0.0, 0.0);
    CHECK(near(t.matrixRelativeTo(camera)[3].x, 0.0001, 1e-6));
    CHECK(t.matrix()[3].x == 8000.0f); // the tenth of a millimeter is lost in an absolute float
    t.lookAt(t.position + glm::vec3(0, 0, -5)); // look down −Z
    CHECK(near(t.forward().z, -1.0, 1e-6));
}

void cameraRelativeRendering() {
    // A scene at the far corner of the world, across the seam from part of itself.
    const GlobalPosition base = GlobalPosition::fromRaw(kMax, 1 << 20, kMin);
    Scene scene;
    Entity cam = scene.createEntity("Camera");
    cam.add<CameraComponent>();
    cam.transform().position = base + glm::vec3(0.0f, 2.0f, 10.0f);
    Entity box = scene.createEntity("Box");
    box.add<MeshComponent>(MeshHandle{0});
    box.transform().position = base + GlobalOffset::fromMeters(glm::dvec3(0.0001, 0.0, 0.0)); // 0.1 mm off, across the seam
    Entity lamp = scene.createEntity("Lamp");
    lamp.add<PointLightComponent>();
    lamp.transform().position = base + glm::vec3(1.0f, 3.0f, 0.0f);

    RenderScene rs;
    CHECK(scene.buildRenderScene(rs, 1.0f));
    CHECK(rs.camera.origin == cam.transform().position); // the camera is the origin
    CHECK(rs.camera.position == glm::vec3(0.0f));
    CHECK(rs.draws.size() == 1 && rs.pointLights.size() == 1);
    if (rs.draws.size() == 1) {
        const glm::vec3 t(rs.draws[0].transform[3]);
        CHECK(near(t.x, 0.0001, 1e-6) && near(t.y, -2.0, 1e-6) && near(t.z, -10.0, 1e-6)); // 0.1 mm survives
    }
    if (rs.pointLights.size() == 1) CHECK(near(rs.pointLights[0].position.x, 1.0, 1e-6) && near(rs.pointLights[0].position.y, 1.0, 1e-6));

    // An explicit camera (e.g. the editor's) brings its own origin.
    const CameraData editorCam = CameraData::relativeLookAt(base + glm::vec3(0, 0, 5), {0, 0, -1}, 1.0f, 1.0f, 0.1f, 100.0f);
    CHECK(scene.buildRenderScene(rs, 1.0f, &editorCam));
    CHECK(rs.camera.origin == editorCam.origin && near(rs.draws[0].transform[3].z, -5.0, 1e-6));

    // Debug lines are recorded relative to their own origin, exact anywhere.
    DebugDraw dd;
    dd.setOrigin(base);
    dd.line(base + glm::vec3(0, 0, 0), base + GlobalOffset::fromMeters(glm::dvec3(0.0005, 0, 0)), {1, 1, 1, 1});
    CHECK(dd.lineCount() == 1 && near(dd.vertices(false)[1].position.x, 0.0005, 1e-6));
}

bool sameRotation(const glm::quat& a, const glm::quat& b, f32 eps = 1e-5f) {
    return std::abs(std::abs(glm::dot(a, b)) - 1.0f) <= eps; // q and −q are the same rotation
}

void rotations() {
    const EulerOrder orders[] = {EulerOrder::XYZ, EulerOrder::XZY, EulerOrder::YXZ,
                                 EulerOrder::YZX, EulerOrder::ZXY, EulerOrder::ZYX};
    std::mt19937 rng(7);
    std::uniform_real_distribution<f32> any(-3.0f, 3.0f), middle(-1.4f, 1.4f);
    bool roundTrip = true, anglesBack = true;
    for (EulerOrder order : orders)
        for (int i = 0; i < 200; ++i) {
            // Angles in the canonical range: the middle rotation of the order within ±80°.
            glm::vec3 e(any(rng), any(rng), any(rng));
            const char* name = eulerOrderName(order);
            e[name[1] - 'X'] = middle(rng);
            const glm::quat q = quatFromEuler(e, order);
            const glm::vec3 back = eulerFromQuat(q, order);
            roundTrip &= sameRotation(quatFromEuler(back, order), q);
            anglesBack &= glm::length(back - e) < 1e-3f;
        }
    CHECK(roundTrip && anglesBack);

    // The old convention (glm::quat(vec3), used by older scene files) is ZYX.
    bool legacy = true;
    for (int i = 0; i < 100; ++i) {
        const glm::vec3 e(any(rng), any(rng), any(rng));
        legacy &= sameRotation(quatFromEuler(e, EulerOrder::ZYX), glm::quat(e));
    }
    CHECK(legacy);

    // Known values: 90° yaw turns forward (−Z) to −X; the order matters once two axes are involved.
    const glm::quat yaw90 = quatFromEulerDegrees({0, 90, 0});
    CHECK(glm::length(yaw90 * glm::vec3(0, 0, -1) - glm::vec3(-1, 0, 0)) < 1e-5f);
    CHECK(!sameRotation(quatFromEulerDegrees({30, 60, 0}, EulerOrder::XYZ), quatFromEulerDegrees({30, 60, 0}, EulerOrder::YXZ)));
    // Yaw then pitch (YXZ): pitch is around the camera's own right axis, so forward keeps its heading.
    const glm::vec3 f = quatFromEulerDegrees({-30, 90, 0}) * glm::vec3(0, 0, -1);
    CHECK(f.x < -0.8f && f.y < -0.4f && std::abs(f.z) < 1e-5f);

    // Gimbal lock: pitch ±90° in YXZ; the angles still rebuild the same rotation.
    const glm::quat locked = quatFromEulerDegrees({90, 40, 25});
    const glm::vec3 lockedBack = eulerDegreesFromQuat(locked);
    CHECK(std::abs(lockedBack.x - 90.0f) < 1e-2f && lockedBack.z == 0.0f);
    CHECK(sameRotation(quatFromEulerDegrees(lockedBack), locked, 1e-4f));

    // TransformComponent: stored as a quaternion, viewed and set as Euler in any order.
    TransformComponent t;
    t.setEulerDegrees({10, 20, 30}, EulerOrder::ZXY);
    const glm::vec3 d = t.eulerDegrees(EulerOrder::ZXY);
    CHECK(glm::length(d - glm::vec3(10, 20, 30)) < 1e-3f);
    CHECK(sameRotation(t.orientation(), quatFromEulerDegrees({10, 20, 30}, EulerOrder::ZXY)));
    t.rotation = glm::quat(1, 0, 0, 0);
    t.rotate(glm::radians(90.0f), {0, 1, 0});
    t.rotateLocal(glm::radians(90.0f), {1, 0, 0}); // then pitch up around its own X
    CHECK(glm::length(t.forward() - glm::vec3(0, 1, 0)) < 1e-5f);
    t.position = {0.0, 0.0, 0.0};
    t.lookAt(GlobalPosition(3.0, 0.0, -3.0));
    CHECK(glm::length(t.forward() - glm::normalize(glm::vec3(1, 0, -1))) < 1e-5f);

    // Files: quaternions as [x, y, z, w]; older files (Euler radians) still load, same orientation.
    Scene scene;
    Entity e = scene.createEntity("Rot");
    e.transform().setEulerDegrees({15, -70, 5});
    const std::string json = serializeScene(scene);
    Scene back;
    CHECK(deserializeScene(back, json));
    CHECK(sameRotation(back.findEntity("Rot").transform().orientation(), e.transform().orientation(), 1e-6f));
    Scene old;
    const char* legacyFile = R"({"format": "nova.scene", "version": 1, "entities": [
        {"components": {"Name": {"name": "Old"}, "Transform": {"position": [0, 0, 0], "rotation": [0.3, -1.2, 0.1], "scale": [1, 1, 1]}}}]})";
    CHECK(deserializeScene(old, legacyFile));
    CHECK(sameRotation(old.findEntity("Old").transform().orientation(), glm::quat(glm::vec3(0.3f, -1.2f, 0.1f))));
}

void quaternionUnitAndInterpolation() {
    // renormalize: free for unit quaternions, fixes drift, rescues degenerate input.
    const glm::quat q = quatFromEulerDegrees({20, 40, 60});
    const glm::quat same = renormalize(q);
    CHECK(same.x == q.x && same.y == q.y && same.z == q.z && same.w == q.w);
    CHECK(std::abs(glm::length(renormalize(q * 1.0004f)) - 1.0f) < 1e-6f); // Newton step
    CHECK(std::abs(glm::length(renormalize(q * 1.003f)) - 1.0f) < 1e-6f);  // larger drift: full normalization
    CHECK(std::abs(glm::length(renormalize(q * 3.0f)) - 1.0f) < 1e-6f);   // full normalization
    CHECK(renormalize(glm::quat(0, 0, 0, 0)) == glm::quat(1, 0, 0, 0));

    // A million small rotations stay unit length.
    TransformComponent t;
    for (int i = 0; i < 1'000'000; ++i) t.rotateLocal(1e-3f, {0.3f, 1.0f, 0.2f});
    CHECK(std::abs(glm::length(t.rotation.toDouble()) - 1.0) < 1e-6);
    t.setRotation(glm::quat(2, 0, 0, 0));
    CHECK(std::abs(glm::length(t.rotation.toDouble()) - 1.0) < 1e-6);

    // slerp: endpoints, constant angular speed, shortest arc whatever the sign.
    const glm::quat a = quatFromEulerDegrees({0, 0, 0});
    const glm::quat b = quatFromEulerDegrees({0, 120, 0});
    CHECK(sameRotation(slerp(a, b, 0.0f), a) && sameRotation(slerp(a, b, 1.0f), b));
    CHECK(std::abs(angleBetween(a, slerp(a, b, 0.25f)) - glm::radians(30.0f)) < 1e-4f);
    CHECK(std::abs(angleBetween(a, slerp(a, b, 0.5f)) - glm::radians(60.0f)) < 1e-4f);
    CHECK(sameRotation(slerp(a, -b, 0.5f), slerp(a, b, 0.5f)));
    CHECK(std::abs(glm::length(slerp(a, b, 0.37f)) - 1.0f) < 1e-6f);
    // nlerp: same path and endpoints, unit length, not constant speed.
    CHECK(sameRotation(nlerp(a, b, 1.0f), b) && std::abs(glm::length(nlerp(a, b, 0.5f)) - 1.0f) < 1e-6f);
    CHECK(sameRotation(nlerp(a, b, 0.5f), slerp(a, b, 0.5f))); // the midpoint coincides
    // rotateTowards: limited steps, then exactly the target.
    const glm::quat step = rotateTowards(a, b, glm::radians(10.0f));
    CHECK(std::abs(angleBetween(a, step) - glm::radians(10.0f)) < 1e-4f);
    CHECK(rotateTowards(a, b, glm::radians(200.0f)) == b);

    // Interpolating whole transforms: exact position, slerped rotation, mixed scale.
    TransformComponent from, to;
    from.position = GlobalPosition::fromUnits({7, 0, 0}, LengthUnit::Kilometer);
    to.position = from.position + GlobalOffset::fromMeters(glm::dvec3(10.0, 0.0, 0.002));
    to.rotation = b;
    to.scale = glm::vec3(3.0f);
    const TransformComponent mid = TransformComponent::lerp(from, to, 0.5f);
    const GlobalOffset half = (mid.position - from.position) - GlobalOffset::fromMeters(glm::dvec3(5.0, 0.0, 0.001));
    CHECK(std::abs(half.x) <= 1 && half.y == 0 && std::abs(half.z) <= 1); // within one step (rounding of the half)
    CHECK(std::abs(angleBetween(mid.orientation(), a) - glm::radians(60.0f)) < 1e-4f);
    CHECK(mid.scale == glm::vec3(2.0f));
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Log::init();
    Log::core().set_level(spdlog::level::off); // the tests trigger expected warnings
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"defaults: Q45.18, the 16 km world", defaults},
        {"exact arithmetic, shortest offsets", exactness},
        {"the default world: 2^32 steps per axis", torus},
        {"offsets: 64-bit squared lengths, looping sums", offsets},
        {"world sizes: tiny, default, 1000 km, the largest", worldSizes},
        {"WorldOrigin: grid, hysteresis, exact moves", worldOrigin},
        {"metric units", units},
        {"helpers (lerp, moveTowards, rotateAround)", helpers},
        {"scenes: files, older files, prefabs, relative matrices", sceneIntegration},
        {"camera-relative rendering at the far corner", cameraRelativeRendering},
        {"rotations: quaternions, Euler in six orders", rotations},
        {"rotations: always unit, slerp / nlerp / rotateTowards", quaternionUnitAndInterpolation},
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
