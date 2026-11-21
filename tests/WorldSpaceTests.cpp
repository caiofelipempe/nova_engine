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
