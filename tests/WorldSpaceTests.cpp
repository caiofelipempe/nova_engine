// Headless tests of integer world positions: exact arithmetic, barriers, wrap-around (full and
// arbitrary sizes), saturation, 128-bit lengths, conversions, metric units and helpers.
#include <nova/Nova.h>

#include <cmath>
#include <cstdio>
#include <functional>
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

constexpr i64 kMax = std::numeric_limits<i64>::max();
constexpr i64 kMin = std::numeric_limits<i64>::min();

// Activates a world for the scope of a test, restoring the default afterwards.
struct UseWorld {
    explicit UseWorld(const WorldSpaceDesc& d) { WorldSpace::setActive(WorldSpace(d)); }
    ~UseWorld() { WorldSpace::setActive(WorldSpace()); }
};

// ------------------------------------------------------------------- tests --
void defaults() {
    const WorldSpace& w = WorldSpace::active();
    CHECK(w.unit() == 1.0 / 4294967296.0);
    for (int i = 0; i < 3; ++i) {
        CHECK(w.axis(i).boundary == WorldBoundary::Barrier);
        CHECK(w.axis(i).span == WorldSpace::kMaxBarrierSpan);
        CHECK(w.axis(i).min == -(i64(1) << 62) && w.axis(i).max == (i64(1) << 62) - 1);
    }
    CHECK(near(w.sizeMeters(0), 2147483648.0, 1e-3)); // 2^63 steps of 2^-32 m

    // 32.32: 1.5 m is exactly 1.5 * 2^32 steps; round trips through meters exactly.
    GlobalPosition p = GlobalPosition::fromMeters({1.5, -2.25, 1000.0});
    CHECK(p.raw(0) == 6442450944 && p.raw(1) == -9663676416 && p.raw(2) == 4294967296000);
    CHECK(p.toMeters() == glm::dvec3(1.5, -2.25, 1000.0));
    CHECK(GlobalPosition() == GlobalPosition::fromMeters({0, 0, 0}));
}

void exactness() {
    // a + (b - a) == b, bit for bit, for random points spread over the whole world.
    std::mt19937_64 rng(1234);
    const WorldSpace& w = WorldSpace::active();
    std::uniform_int_distribution<i64> any(w.axis(0).min, w.axis(0).max);
    bool all = true;
    for (int i = 0; i < 2000; ++i) {
        GlobalPosition a = GlobalPosition::fromRaw(any(rng), any(rng), any(rng));
        GlobalPosition b = GlobalPosition::fromRaw(any(rng), any(rng), any(rng));
        all &= (a + (b - a) == b) && (b - (b - a) == a);
    }
    CHECK(all);

    // Far from the origin a nanometer still counts (float would lose it entirely).
    GlobalPosition far = GlobalPosition::fromMeters({1'000'000.0, 0, 0});
    GlobalPosition nudged = far + GlobalOffset::fromMeters(glm::dvec3(1e-9, 0, 0));
    CHECK(nudged != far && (nudged - far).x == 4); // 1 nm ≈ 4.29 steps
    CHECK(static_cast<f32>(1'000'000.0f + 1e-9f) == 1'000'000.0f);

    // Accumulating many small moves is exact: N identical steps = N * step.
    GlobalPosition walker = far;
    const GlobalOffset step = GlobalOffset::fromMeters(glm::vec3(0.001f, -0.0005f, 0.0f));
    for (int i = 0; i < 100'000; ++i) walker += step;
    CHECK(walker - far == step * 100'000);
}

void barrier() {
    UseWorld use(WorldSpaceDesc{}.setAxis(0, 100.0, LengthUnit::Meter, WorldBoundary::Barrier));
    const WorldSpace& w = WorldSpace::active();
    CHECK(near(w.sizeMeters(0), 100.0, 1e-9));
    GlobalPosition p = GlobalPosition::fromMeters({49.0, 0, 0});
    const u8 hit = p.move(glm::vec3(5.0f, 1.0f, 0.0f));
    CHECK(hit == 0b001);           // only X hit its barrier
    CHECK(p.raw(0) == w.axis(0).max); // stopped at the edge
    CHECK(near(p.toMeters().y, 1.0, 1e-9));
    CHECK(GlobalPosition::fromMeters({-500.0, 0, 0}).raw(0) == w.axis(0).min); // clamped on creation

    // Edge to edge: the difference is the whole axis and still exact.
    GlobalPosition lo = GlobalPosition::fromRaw(w.axis(0).min, 0, 0);
    GlobalPosition hi = GlobalPosition::fromRaw(w.axis(0).max, 0, 0);
    CHECK((hi - lo).x == static_cast<i64>(w.axis(0).span - 1));

    // The largest barrier axis (default): opposite edges still subtract without overflow.
    UseWorld big{WorldSpaceDesc{}};
    const WorldSpace& d = WorldSpace::active();
    GlobalPosition a = GlobalPosition::fromRaw(d.axis(1).min, 0, 0), b = GlobalPosition::fromRaw(d.axis(1).max, 0, 0);
    CHECK((b - a).x == kMax && (a - b).x == -kMax);
    CHECK(a + (b - a) == b);
}

void wrapFull() {
    WorldSpaceDesc desc;
    desc.axes[0].boundary = WorldBoundary::Wrap; // full 2^64 span: the integer's own overflow
    UseWorld use(desc);
    const WorldSpace& w = WorldSpace::active();
    CHECK(w.axis(0).full && w.axis(0).min == kMin && w.axis(0).max == kMax);
    CHECK(near(w.sizeMeters(0), 4294967296.0, 1e-3)); // 2^32 m

    GlobalPosition edge = GlobalPosition::fromRaw(kMax, 0, 0);
    GlobalPosition across = edge + GlobalOffset(3, 0, 0);
    CHECK(across.raw(0) == kMin + 2);           // looped
    CHECK((across - edge).x == 3);           // short way across the seam
    CHECK((edge - across).x == -3);
    CHECK(edge.move(GlobalOffset(3, 0, 0)) == 0); // no barrier
    // Meters beyond the world loop too.
    const f64 size = w.sizeMeters(0);
    CHECK(GlobalPosition::fromMeters({size + 1.0, 0, 0}) == GlobalPosition::fromMeters({1.0, 0, 0}));
}

void wrapSized() {
    // A 50 km loop on X and Z (any size, not just powers of two), barrier on Y; 1 mm steps.
    UseWorld use(WorldSpaceDesc{}
                     .setUnit(1, LengthUnit::Millimeter)
                     .setAxis(0, 50, LengthUnit::Kilometer, WorldBoundary::Wrap)
                     .setAxis(1, 4, LengthUnit::Kilometer, WorldBoundary::Barrier)
                     .setAxis(2, 50, LengthUnit::Kilometer, WorldBoundary::Wrap));
    const WorldSpace& w = WorldSpace::active();
    CHECK(w.axis(0).span == 50'000'000 && w.axis(0).min == -25'000'000 && w.axis(0).max == 24'999'999);
    CHECK(near(w.sizeIn(0, LengthUnit::Kilometer), 50.0, 1e-12));
    CHECK(w.unitIn(LengthUnit::Millimeter) == 1.0);

    GlobalPosition east = GlobalPosition::fromMeters({24'999.0, 0, 0});
    GlobalPosition moved = east + glm::vec3(2.0f, 0, 0); // crosses the seam at +25 km
    CHECK(moved == GlobalPosition::fromMeters({-24'999.0, 0, 0}));
    CHECK((moved - east) == GlobalOffset::fromMeters(glm::dvec3(2.0, 0, 0)));
    CHECK((east - moved).x == -2000);
    CHECK(near(distance(east, moved), 2.0, 1e-9));
    CHECK(GlobalPosition::fromMeters({60'000.0, 0, 0}) == GlobalPosition::fromMeters({10'000.0, 0, 0}));
    CHECK(GlobalPosition::fromMeters({-60'000.0, 0, 0}) == GlobalPosition::fromMeters({-10'000.0, 0, 0}));
    // A huge offset loops many times: 3 laps + 1 m lands 1 m away.
    GlobalPosition origin;
    CHECK(origin + GlobalOffset::fromMeters(glm::dvec3(150'001.0, 0, 0)) == GlobalPosition::fromMeters({1.0, 0, 0}));
    CHECK(origin + GlobalOffset::fromMeters(glm::dvec3(-150'001.0, 0, 0)) == GlobalPosition::fromMeters({-1.0, 0, 0}));

    // Exactness holds in the sized loop as well.
    std::mt19937_64 rng(99);
    std::uniform_int_distribution<i64> any(w.axis(0).min, w.axis(0).max);
    bool all = true;
    for (int i = 0; i < 2000; ++i) {
        GlobalPosition a = GlobalPosition::fromRaw(any(rng), 0, any(rng));
        GlobalPosition b = GlobalPosition::fromRaw(any(rng), 0, any(rng));
        const GlobalOffset d = b - a;
        all &= (a + d == b) && std::abs(d.x) <= 25'000'000 && std::abs(d.z) <= 25'000'000; // shortest way
    }
    CHECK(all);

    // Y is a barrier: 4 km tall.
    GlobalPosition up = GlobalPosition::fromMeters({0, 1'999.0, 0});
    CHECK(up.move(glm::vec3(0, 10.0f, 0)) == 0b010);
}

void offsets() {
    // Saturation instead of overflow.
    GlobalOffset big(kMax, kMin, 5);
    CHECK((big + GlobalOffset(1, -1, 1)) == GlobalOffset(kMax, kMin, 6));
    CHECK(-GlobalOffset(kMin, 0, 0) == GlobalOffset(kMax, 0, 0));
    CHECK(GlobalOffset(i64(1) << 62, -(i64(1) << 62), 3) * 4 == GlobalOffset(kMax, kMin, 12));
    CHECK(GlobalOffset(-7, 6, 0) * -3 == GlobalOffset(21, -18, 0));

    // Exact length comparison where double cannot tell the difference.
    GlobalOffset a((i64(1) << 60), 0, 0), b((i64(1) << 60) + 1, 0, 0);
    CHECK(static_cast<f64>(a.x) == static_cast<f64>(b.x));
    CHECK(compareLength(a, b) < 0 && compareLength(b, a) > 0 && compareLength(a, a) == 0);
    CHECK((GlobalOffset(3, 4, 0).lengthSquared() == UInt128{0, 25}));
    CHECK((GlobalOffset(kMin, kMin, kMin).lengthSquared() == UInt128{0xC000000000000000ull, 0})); // 3 * 2^126

    // isShorterThan is exact at the boundary.
    const f64 unit = WorldSpace::active().unit();
    GlobalOffset five(3, 4, 0); // 5 steps
    CHECK(five.isShorterThan(5.0001 * unit) && !five.isShorterThan(5.0 * unit) && !five.isShorterThan(4.9 * unit));

    // Scaling stays within a step even beyond 2^53.
    GlobalOffset huge((i64(1) << 61) + 12345, -((i64(1) << 60) + 7), 1000);
    GlobalOffset half = huge.scaled(0.5);
    CHECK(std::abs(half.x - (huge.x / 2)) <= 1 && std::abs(half.y - (huge.y / 2)) <= 1 && half.z == 500);
    CHECK(huge.scaled(1.0) == huge && huge.scaled(0.0) == GlobalOffset());
    CHECK(GlobalOffset(kMax, 0, 0).scaled(4.0).x == kMax);
}

void units() {
    CHECK(convertLength(2.5, LengthUnit::Kilometer, LengthUnit::Meter) == 2500.0);
    CHECK(near(convertLength(1.0, LengthUnit::Meter, LengthUnit::Nanometer), 1e9, 1e-3));
    CHECK(toMeters(25.0, LengthUnit::Centimeter) == 0.25);
    CHECK(fromMeters(1500.0, LengthUnit::Kilometer) == 1.5);
    CHECK(readableUnit(1500.0) == LengthUnit::Kilometer && readableUnit(0.02) == LengthUnit::Centimeter &&
          readableUnit(3e-8) == LengthUnit::Nanometer);
    CHECK(std::string(unitSymbol(LengthUnit::Millimeter)) == "mm");

    GlobalPosition p = GlobalPosition::fromUnits({1500.0, 0.25, -2.0}, LengthUnit::Kilometer);
    CHECK(p == GlobalPosition::fromMeters({1'500'000.0, 250.0, -2000.0}));
    const glm::dvec3 km = p.toUnits(LengthUnit::Kilometer);
    CHECK(near(km.x, 1500.0, 1e-12) && near(km.y, 0.25, 1e-12));
    GlobalOffset d = GlobalOffset::fromUnits({0, 0, 250}, LengthUnit::Millimeter);
    CHECK(d == GlobalOffset::fromMeters(glm::dvec3(0, 0, 0.25)));
    CHECK(near(d.length(LengthUnit::Centimeter), 25.0, 1e-9));
    CHECK(near(d.toUnits(LengthUnit::Micrometer).z, 250'000.0, 1e-3));
    CHECK(near(distance(GlobalPosition(), p, LengthUnit::Megameter), std::sqrt(1.5 * 1.5 + 0.00025 * 0.00025 + 0.002 * 0.002), 1e-9));
}

void helpers() {
    GlobalPosition a = GlobalPosition::fromMeters({1'000'000.0, 0, 0});
    GlobalPosition b = GlobalPosition::fromMeters({1'000'010.0, 0, 0});
    CHECK(lerp(a, b, 0.0) == a && lerp(a, b, 1.0) == b);
    CHECK(lerp(a, b, 0.5) == GlobalPosition::fromMeters({1'000'005.0, 0, 0}));
    CHECK(moveTowards(a, b, 4.0) == GlobalPosition::fromMeters({1'000'004.0, 0, 0}));
    CHECK(moveTowards(a, b, 100.0) == b);
    CHECK(isWithin(a, b, 10.5) && !isWithin(a, b, 9.5));

    GlobalPosition r = rotateAround(b, a, glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
    const glm::dvec3 off = (r - a).toMeters();
    CHECK(near(off.x, 0.0, 1e-5) && near(off.z, -10.0, 1e-5)); // +X rotated 90° about +Y → -Z

    // Across a wrap seam, lerp takes the short way.
    UseWorld use(WorldSpaceDesc{}.setUnit(1, LengthUnit::Millimeter).setAxis(0, 100, LengthUnit::Meter, WorldBoundary::Wrap));
    GlobalPosition e = GlobalPosition::fromMeters({49.0, 0, 0}), f = GlobalPosition::fromMeters({-49.0, 0, 0});
    CHECK(lerp(e, f, 0.5) == GlobalPosition::fromMeters({-50.0, 0, 0})); // the seam, 1 m from each
}

void configuration() {
    // Invalid settings are repaired, oversized axes clamped.
    WorldSpaceDesc bad;
    bad.unit = -1.0;
    bad.axes[0] = {1e30, WorldBoundary::Barrier};
    bad.axes[1] = {1e30, WorldBoundary::Wrap};
    WorldSpace w(bad);
    CHECK(w.unit() == WorldSpaceDesc{}.unit);
    CHECK(w.axis(0).span == WorldSpace::kMaxBarrierSpan);
    CHECK(w.axis(1).full);
    WorldSpace tiny(WorldSpaceDesc{}.setUnit(1, LengthUnit::Meter).setAxis(2, 0.1, LengthUnit::Meter, WorldBoundary::Wrap));
    CHECK(tiny.axis(2).span == 2); // at least two steps
}

void maxSize() {
    // "Every dimension at most 10 km": all 64 bits spent on 10 km.
    WorldSpaceDesc d = WorldSpaceDesc::withMaxSize(10, LengthUnit::Kilometer);
    CHECK(d.unit == 10'000.0 / 9223372036854775808.0);
    WorldSpace w(d);
    for (int i = 0; i < 3; ++i) {
        CHECK(w.axis(i).boundary == WorldBoundary::Barrier && w.axis(i).span == WorldSpace::kMaxBarrierSpan);
        CHECK(near(w.sizeIn(i, LengthUnit::Kilometer), 10.0, 1e-12));
    }
    CHECK(w.unitIn(LengthUnit::Nanometer) < 2e-6); // ~1.1 femtometers per step

    // Wrap axes hold twice as many steps: the unit is halved for the same size.
    WorldSpace wrap(WorldSpaceDesc::withMaxSize(10, LengthUnit::Kilometer, WorldBoundary::Wrap));
    CHECK(wrap.unit() == d.unit / 2 && wrap.axis(0).full && near(wrap.sizeIn(0, LengthUnit::Kilometer), 10.0, 1e-12));

    // Mixed sizes: the unit fits the largest axis; smaller axes get fewer steps.
    WorldSpaceDesc mixed = WorldSpaceDesc{}
                               .setAxis(0, 40, LengthUnit::Kilometer, WorldBoundary::Wrap)
                               .setAxis(1, 2, LengthUnit::Kilometer, WorldBoundary::Barrier)
                               .setAxis(2, 40, LengthUnit::Kilometer, WorldBoundary::Wrap)
                               .fitUnit();
    WorldSpace m(mixed);
    CHECK(m.axis(0).full && near(m.sizeIn(0, LengthUnit::Kilometer), 40.0, 1e-9));
    CHECK(near(m.sizeIn(1, LengthUnit::Kilometer), 2.0, 1e-9) && m.axis(1).span < WorldSpace::kMaxBarrierSpan);

    // Positions work at that scale: 1 nm moves near the 5 km edge are seen.
    UseWorld use(d);
    GlobalPosition edge = GlobalPosition::fromUnits({4.999, 0, 0}, LengthUnit::Kilometer);
    CHECK((edge + GlobalOffset::fromUnits({1, 0, 0}, LengthUnit::Nanometer)) != edge);
    CHECK(edge.move(glm::vec3(10.0f, 0, 0)) == 0b001); // 1 m short of the 5 km barrier
}

void sceneIntegration() {
    // The transform stores a GlobalPosition; meters assign naturally.
    Scene a("Far");
    a.setWorldSpace(WorldSpaceDesc{}
                        .setUnit(1, LengthUnit::Micrometer)
                        .setAxis(0, 100, LengthUnit::Kilometer, WorldBoundary::Wrap)
                        .setAxis(2, 100, LengthUnit::Kilometer, WorldBoundary::Wrap));
    Entity near0 = a.createEntity("Near");
    near0.transform().position = {1, 2, 3};
    Entity far = a.createEntity("Far");
    far.transform().position = GlobalPosition::fromUnits({49.999, 0.5, -12.25}, LengthUnit::Kilometer);
    far.transform().position += GlobalOffset(1, 0, 0); // one micrometer: still exact in the file
    Entity odd = a.createEntity("Odd");
    odd.transform().position = GlobalPosition::fromRaw(123456789012345, 0, 7); // not a short decimal

    const std::string json = serializeScene(a);
    CHECK(json.find("\"world\"") != std::string::npos && json.find("\"Wrap\"") != std::string::npos);
    Scene b;
    CHECK(deserializeScene(b, json));
    CHECK(b.worldSpace().unit == a.worldSpace().unit);
    CHECK(b.worldSpace().axes[0].boundary == WorldBoundary::Wrap && b.worldSpace().axes[1].boundary == WorldBoundary::Barrier);
    CHECK(b.findEntity("Near").transform().position == GlobalPosition(1, 2, 3));
    CHECK(b.findEntity("Far").transform().position == far.transform().position);
    CHECK(b.findEntity("Odd").transform().position == odd.transform().position);
    CHECK(serializeScene(b) == json);

    // Files from before world spaces (meters, no "world") load into the default world.
    Scene old;
    const char* legacy = R"({"format": "nova.scene", "version": 1, "name": "Old", "entities": [
        {"components": {"Id": {"uuid": 5}, "Name": {"name": "Crate"},
                        "Transform": {"position": [1.5, 0.25, -3.0], "rotation": [0, 0, 0], "scale": [1, 1, 1]}}}]})";
    CHECK(deserializeScene(old, legacy));
    CHECK(old.worldSpace().unit == WorldSpaceDesc{}.unit);
    CHECK(old.findEntity("Crate").transform().position == GlobalPosition(1.5, 0.25, -3.0));

    // Changing the world carries positions over in meters.
    Scene c;
    Entity e = c.createEntity("E");
    e.transform().position = {10.0, -2.0, 4.0};
    c.setWorldSpace(WorldSpaceDesc{}.setUnit(1, LengthUnit::Millimeter));
    CHECK(WorldSpace::active().unitIn(LengthUnit::Millimeter) == 1.0);
    CHECK(e.transform().position.raw(0) == 10'000 && e.transform().position == GlobalPosition(10.0, -2.0, 4.0));
    // update() re-activates the scene's world after another one was made current.
    WorldSpace::setActive(WorldSpace());
    c.update(0.0f);
    CHECK(WorldSpace::active().unitIn(LengthUnit::Millimeter) == 1.0);

    // Prefabs store offsets from the first entity: placed far away, the layout stays exact.
    Scene d;
    Entity p0 = d.createEntity("P0"), p1 = d.createEntity("P1");
    p0.transform().position = {100.0, 0.0, 0.0};
    p1.transform().position = GlobalPosition(100.0, 0.0, 0.0) + GlobalOffset(1, 2, 3);
    const std::string prefab = serializePrefab({p0, p1});
    const GlobalPosition spot = GlobalPosition::fromUnits({2000, 1, -500}, LengthUnit::Kilometer);
    auto placed = instantiatePrefab(d, prefab, spot);
    CHECK(placed.size() == 2 && placed[0].transform().position == spot);
    CHECK(placed.size() == 2 && placed[1].transform().position - placed[0].transform().position == GlobalOffset(1, 2, 3));

    // Matrices relative to a nearby origin keep precision that the absolute float matrix loses.
    WorldSpace::setActive(WorldSpace());
    TransformComponent t;
    t.position = GlobalPosition(1'000'000.0, 0.0, 0.0) + GlobalOffset::fromMeters(glm::dvec3(0.001, 0, 0));
    const GlobalPosition camera(1'000'000.0, 0.0, 0.0);
    CHECK(near(t.matrixRelativeTo(camera)[3].x, 0.001, 1e-9));
    CHECK(t.matrix()[3].x == 1'000'000.0f); // the millimeter is lost in float
    t.lookAt(t.position + glm::vec3(0, 0, -5)); // look down −Z
    CHECK(near(t.forward().z, -1.0, 1e-6));
}

void cameraRelativeRendering() {
    WorldSpace::setActive(WorldSpace());
    // A scene 10,000 km from the origin: in float, positions there are 1 m apart.
    const GlobalPosition base = GlobalPosition::fromUnits({10'000, 0.5, -10'000}, LengthUnit::Kilometer);
    Scene scene;
    Entity cam = scene.createEntity("Camera");
    cam.add<CameraComponent>();
    cam.transform().position = base + glm::vec3(0.0f, 2.0f, 10.0f);
    Entity box = scene.createEntity("Box");
    box.add<MeshComponent>(MeshHandle{0});
    box.transform().position = base + GlobalOffset::fromMeters(glm::dvec3(0.001, 0.0, 0.0)); // 1 mm off the base
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
        CHECK(near(t.x, 0.001, 1e-6) && near(t.y, -2.0, 1e-6) && near(t.z, -10.0, 1e-6)); // the millimeter survives
    }
    if (rs.pointLights.size() == 1) CHECK(near(rs.pointLights[0].position.x, 1.0, 1e-6) && near(rs.pointLights[0].position.y, 1.0, 1e-6));
    // The absolute float matrix cannot even tell the box from the base.
    CHECK(box.transform().matrix()[3].x == static_cast<f32>(base.toMeters().x));

    // An explicit camera (e.g. the editor's) brings its own origin.
    const CameraData editorCam = CameraData::relativeLookAt(base + glm::vec3(0, 0, 5), {0, 0, -1}, 1.0f, 1.0f, 0.1f, 100.0f);
    CHECK(scene.buildRenderScene(rs, 1.0f, &editorCam));
    CHECK(rs.camera.origin == editorCam.origin && near(rs.draws[0].transform[3].z, -5.0, 1e-6));

    // Debug lines are recorded relative to their own origin, exact far away.
    DebugDraw dd;
    dd.setOrigin(base);
    dd.line(base + glm::vec3(0, 0, 0), base + GlobalOffset::fromMeters(glm::dvec3(0.0005, 0, 0)), {1, 1, 1, 1});
    CHECK(dd.lineCount() == 1 && near(dd.vertices(false)[1].position.x, 0.0005, 1e-7));
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
    CHECK(sameRotation(t.rotation, quatFromEulerDegrees({10, 20, 30}, EulerOrder::ZXY)));
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
    CHECK(sameRotation(back.findEntity("Rot").transform().rotation, e.transform().rotation, 1e-6f));
    Scene old;
    const char* legacyFile = R"({"format": "nova.scene", "version": 1, "entities": [
        {"components": {"Name": {"name": "Old"}, "Transform": {"position": [0, 0, 0], "rotation": [0.3, -1.2, 0.1], "scale": [1, 1, 1]}}}]})";
    CHECK(deserializeScene(old, legacyFile));
    CHECK(sameRotation(old.findEntity("Old").transform().rotation, glm::quat(glm::vec3(0.3f, -1.2f, 0.1f))));
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
    CHECK(std::abs(glm::length(t.rotation) - 1.0f) < 1e-5f);
    t.setRotation(glm::quat(2, 0, 0, 0));
    CHECK(std::abs(glm::length(t.rotation) - 1.0f) < 1e-6f);

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
    from.position = GlobalPosition::fromUnits({1000, 0, 0}, LengthUnit::Kilometer);
    to.position = from.position + GlobalOffset::fromMeters(glm::dvec3(10.0, 0.0, 0.002));
    to.rotation = b;
    to.scale = glm::vec3(3.0f);
    const TransformComponent mid = TransformComponent::lerp(from, to, 0.5f);
    const GlobalOffset half = (mid.position - from.position) - GlobalOffset::fromMeters(glm::dvec3(5.0, 0.0, 0.001));
    CHECK(std::abs(half.x) <= 1 && half.y == 0 && std::abs(half.z) <= 1); // within one 32.32 step (rounding of the half)
    CHECK(std::abs(angleBetween(mid.rotation, a) - glm::radians(60.0f)) < 1e-4f);
    CHECK(mid.scale == glm::vec3(2.0f));
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Log::init();
    Log::core().set_level(spdlog::level::off); // the tests trigger expected warnings
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"defaults (32.32, barriers)", defaults},
        {"exact arithmetic", exactness},
        {"barriers", barrier},
        {"wrap over the full integer", wrapFull},
        {"wrap with any size", wrapSized},
        {"offsets: saturation, 128-bit lengths, scaling", offsets},
        {"metric units", units},
        {"helpers (lerp, moveTowards, rotateAround)", helpers},
        {"configuration validation", configuration},
        {"maximum size per dimension (withMaxSize / fitUnit)", maxSize},
        {"scenes: world settings, files, prefabs, relative matrices", sceneIntegration},
        {"camera-relative rendering 10,000 km away", cameraRelativeRendering},
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
