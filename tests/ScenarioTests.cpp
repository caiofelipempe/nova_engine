// Headless runs of the scenarios (the same code the editor's Game/Scenarios commands use): the
// arena's walls stop and report bodies; at the torus seam bodies run through and keep going.
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

std::vector<Entity> withScript(Scene& scene, const char* prefix) {
    std::vector<Entity> out;
    for (auto [e, name] : scene.view<NameComponent>().each())
        if (name.name.rfind(prefix, 0) == 0) out.push_back({e, &scene});
    return out;
}

void closedWorld() {
    Scene scene;
    buildSmallWorld(scene, kMeshes, SmallWorld::Closed);

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
    std::printf("  arena: %d wall touches by %d of 12 balls in 20 s\n", touches, ballsThatTouched);
    CHECK(ballsThatTouched >= 6); // the walls are hit, reported, and hold
    for (Entity c : crates) {
        const glm::dvec3 p = c.transform().position.toMeters();
        CHECK(std::abs(p.x) <= half && std::abs(p.z) <= half && p.y > 0.2);
    }
}

void openWorld() {
    Scene scene;
    Entity cam = buildSmallWorld(scene, kMeshes, SmallWorld::Open);
    auto runners = withScript(scene, "Runner ");
    CHECK(runners.size() == 8);
    scene.startSimulation();
    std::vector<glm::dvec3> travelled(runners.size(), glm::dvec3(0.0));
    std::vector<GlobalPosition> last;
    for (Entity e : runners) last.push_back(e.transform().position);
    std::vector<bool> crossed(runners.size(), false);
    bool onFloor = true;
    for (int step = 0; step < 60 * 30; ++step) { // 30 seconds
        scene.update(Duration::perSecond(60));
        for (size_t i = 0; i < runners.size(); ++i) {
            const GlobalPosition now = runners[i].transform().position;
            travelled[i] += (now - last[i]).toMeters(); // shortest way: crossing the seam is a small step
            // Crossing the seam: x jumps from about +8 192 m to about −8 192 m.
            crossed[i] = crossed[i] || (last[i].toMeters().x > 8000.0 && now.toMeters().x < -8000.0);
            last[i] = now;
            if (now.toMeters().y <= 0.3 && onFloor)
                std::printf("    runner %zu below the floor at step %d: x %.3f y %.3f z %.3f\n", i, step, now.toMeters().x,
                            now.toMeters().y, now.toMeters().z);
            onFloor &= now.toMeters().y > 0.3;
        }
    }
    // Every runner went through the seam and kept going on the other side, never falling through
    // the floor while crossing it (the float window follows them and re-wraps bodies).
    CHECK(onFloor);
    bool all = true;
    double least = 1e9;
    for (size_t i = 0; i < runners.size(); ++i) {
        all = all && crossed[i];
        least = std::min(least, travelled[i].x);
    }
    std::printf("  torus seam: every runner crossed it, each travelling at least %.0f m along X in 30 s\n", least);
    CHECK(all);
    CHECK(least > 160.0);

    // Rendering across the seam: from x = +8 191 m, a floor tile centered at −8 187 m is 6 m ahead.
    const CameraData eye =
        CameraData::relativeLookAt(GlobalPosition(WorldSpace::kHalfLap - 1.0, 2.0, 5.0), {1, 0, 0}, 1.0f, 1.0f, 0.1f, 200.0f);
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
        {"arena: walls stop and report bodies", closedWorld},
        {"torus seam: bodies run through it", openWorld},
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
