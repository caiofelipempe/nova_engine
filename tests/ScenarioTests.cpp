// Headless runs of the small-world scenarios (the same code the editor's Game/Scenarios commands
// use): closed edges stop and report bodies; open edges let them loop forever.
#include <nova/Nova.h>

#include "../editor/game/WorldScenarios.h"

#include <cmath>
#include <cstdio>
#include <functional>
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

const ScenarioMeshes kMeshes{MeshHandle{1}, MeshHandle{2}, MeshHandle{3}}; // no renderer needed

struct Restore {
    ~Restore() { WorldSpace::setActive(WorldSpace()); }
};

std::vector<Entity> withScript(Scene& scene, const char* prefix) {
    std::vector<Entity> out;
    for (auto [e, name] : scene.view<NameComponent>().each())
        if (name.name.rfind(prefix, 0) == 0) out.push_back({e, &scene});
    return out;
}

void closedWorld() {
    Restore restore;
    Scene scene;
    buildSmallWorld(scene, kMeshes, SmallWorld::Closed);
    for (int i = 0; i < 3; ++i) CHECK(WorldSpace::active().axis(i).boundary == WorldBoundary::Barrier);
    CHECK(std::abs(WorldSpace::active().sizeMeters(0) - kSmallWorldSize) < 1e-6);

    auto balls = withScript(scene, "Ball ");
    auto crates = withScript(scene, "Crate ");
    CHECK(balls.size() == 12 && crates.size() == 8);
    scene.startSimulation();
    bool inside = true, aboveFloor = true;
    const double half = kSmallWorldSize / 2;
    for (int step = 0; step < 60 * 20; ++step) { // 20 seconds
        scene.update(Duration::perSecond(60));
        for (Entity e : balls) {
            const glm::dvec3 p = e.transform().position.toMeters();
            inside &= std::abs(p.x) <= half && std::abs(p.z) <= half;
            aboveFloor &= p.y > 0.3; // radius 0.5 on the floor at y = 0
        }
    }
    CHECK(inside);
    CHECK(aboveFloor);
    int touches = 0, ballsThatTouched = 0;
    for (Entity e : balls) {
        auto* sc = e.tryGet<ScriptComponent>();
        for (auto& s : sc->scripts)
            if (auto* flash = dynamic_cast<EdgeFlash*>(s.get())) {
                touches += flash->touches;
                ballsThatTouched += flash->touches > 0;
            }
    }
    std::printf("  closed world: %d edge touches by %d of 12 balls in 20 s\n", touches, ballsThatTouched);
    CHECK(ballsThatTouched >= 6); // the edges are hit, reported, and hold
    for (Entity c : crates) {
        const glm::dvec3 p = c.transform().position.toMeters();
        CHECK(std::abs(p.x) <= half && std::abs(p.z) <= half && p.y > 0.2);
    }
}

void openWorld() {
    Restore restore;
    Scene scene;
    Entity cam = buildSmallWorld(scene, kMeshes, SmallWorld::Open);
    CHECK(WorldSpace::active().axis(0).boundary == WorldBoundary::Wrap);
    CHECK(WorldSpace::active().axis(2).boundary == WorldBoundary::Wrap);
    CHECK(WorldSpace::active().axis(1).boundary == WorldBoundary::Barrier);

    auto runners = withScript(scene, "Runner ");
    CHECK(runners.size() == 8);
    scene.startSimulation();
    std::vector<glm::dvec3> travelled(runners.size(), glm::dvec3(0.0));
    std::vector<GlobalPosition> last;
    for (Entity e : runners) last.push_back(e.transform().position);
    bool onFloor = true;
    for (int step = 0; step < 60 * 30; ++step) { // 30 seconds
        scene.update(Duration::perSecond(60));
        for (size_t i = 0; i < runners.size(); ++i) {
            const GlobalPosition now = runners[i].transform().position;
            travelled[i] += (now - last[i]).toMeters(); // shortest way: crossing a seam is a small step
            last[i] = now;
            onFloor &= now.toMeters().y > 0.3;
        }
    }
    // Every runner went around the world at least twice, never falling through the floor while
    // crossing the seams (the float window follows them and re-wraps bodies).
    CHECK(onFloor);
    double minLaps = 1e9;
    for (const glm::dvec3& d : travelled) minLaps = std::min(minLaps, glm::length(glm::dvec2(d.x, d.z)) / kSmallWorldSize);
    std::printf("  open world: every runner travelled at least %.1f laps in 30 s\n", minLaps);
    CHECK(minLaps >= 2.0);

    // Rendering across the seam: from x = 29, a floor tile centred at x = −25 is 6 m ahead.
    const CameraData eye = CameraData::relativeLookAt(GlobalPosition(29.0, 2.0, 0.0), {1, 0, 0}, 1.0f, 1.0f, 0.1f, 200.0f);
    RenderScene rs;
    scene.buildRenderScene(rs, 1.0f, &eye);
    bool ahead = false;
    for (const DrawItem& d : rs.draws)
        if (std::abs(d.transform[3].x - 6.0f) < 1e-3f && std::abs(d.transform[3].y + 2.0f) < 1e-3f) ahead = true;
    CHECK(ahead);
    CHECK(cam.valid());
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Log::init();
    Log::core().set_level(spdlog::level::warn);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"small world, closed edges (barriers)", closedWorld},
        {"small world, open edges (wrap-around)", openWorld},
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
