// Streaming of a large world by chunks (nova/scene/Chunks.h): the grid, loading nearest first with a
// budget, hysteresis on borders, unloading after a delay, streamed entities, and dormant physics.
// Run: ctest --preset ... or NovaChunkTests.exe
#include <nova/Nova.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
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

ChunkSettings streaming() {
    ChunkSettings s;
    s.enabled = true;
    return s;
}

// What the source was asked to do, in order.
struct SourceLog {
    std::vector<ChunkCoord> loads, unloads;
    ChunkSource source() {
        return {[this](Scene&, const ChunkCoord& c) { loads.push_back(c); },
                [this](Scene&, const ChunkCoord& c) { unloads.push_back(c); }};
    }
};

void runUntilLoaded(Scene& scene, int frames = 40) {
    for (int i = 0; i < frames; ++i) scene.update(Duration::perSecond(60));
}

void grid() {
    Scene scene("grid");
    scene.setChunkStreaming(streaming()); // 256 m, X and Z
    CHECK(scene.chunkOf(GlobalPosition(0.0, 0.0, 0.0)) == (ChunkCoord{0, 0, 0}));
    CHECK(scene.chunkOf(GlobalPosition(255.999, 900.0, 0.0)) == (ChunkCoord{0, 0, 0})); // Y is not cut
    CHECK(scene.chunkOf(GlobalPosition(256.0, 0.0, -0.001)) == (ChunkCoord{1, 0, -1}));
    CHECK(scene.chunkOf(GlobalPosition(-8192.0, 0.0, 8191.9)) == (ChunkCoord{-32, 0, 31})); // the 16 km world: 64 a side
    ChunkSettings x = streaming();
    x.axes = {true, false, false};
    scene.setChunkStreaming(x);
    CHECK(scene.chunkOf(GlobalPosition(-600.0, 300.0, 700.0)) == (ChunkCoord{-3, 0, 0}));
    ChunkSettings xyz = streaming();
    xyz.axes = {true, true, true};
    xyz.size = 100.0f;
    scene.setChunkStreaming(xyz);
    CHECK(scene.chunkOf(GlobalPosition(250.0, -50.0, 99.0)) == (ChunkCoord{2, -1, 0}));
}

void loading() {
    Scene scene("load");
    SourceLog log;
    scene.setChunkSource(log.source());
    scene.setChunkStreaming(streaming()); // 5×5 loaded, 2 a frame
    scene.setPhysicsFocus(GlobalPosition(100.0, 0.0, 100.0));
    scene.update(Duration::perSecond(60));
    CHECK(log.loads.size() == 2);                              // the budget
    CHECK(!log.loads.empty() && log.loads[0] == (ChunkCoord{0, 0, 0})); // nearest first: the focus chunk
    runUntilLoaded(scene);
    CHECK(log.loads.size() == 25 && scene.loadedChunks().size() == 25 && log.unloads.empty());
    bool square = true; // exactly the 5×5 around the focus chunk
    for (i64 x = -2; x <= 2; ++x)
        for (i64 z = -2; z <= 2; ++z) square &= scene.isChunkLoaded({x, 0, z});
    CHECK(square);
    // Nearest first all the way: distances never decrease along the loads.
    bool ordered = true;
    for (size_t i = 1; i < log.loads.size(); ++i) {
        auto d2 = [](const ChunkCoord& c) { return c.x * c.x + c.z * c.z; };
        ordered &= d2(log.loads[i - 1]) <= d2(log.loads[i]);
    }
    CHECK(ordered);

    // The axes chosen: X only gives a row, all three a cube.
    for (auto [axes, count] : {std::pair{std::array<bool, 3>{true, false, false}, size_t(5)},
                               std::pair{std::array<bool, 3>{true, true, true}, size_t(125)}}) {
        Scene s("axes");
        SourceLog l;
        s.setChunkSource(l.source());
        ChunkSettings c = streaming();
        c.axes = axes;
        c.loadsPerFrame = 0; // no limit
        s.setChunkStreaming(c);
        s.update(Duration::perSecond(60));
        CHECK(l.loads.size() == count);
    }
}

void seam() {
    // Near the seam of the 16 km world, the rings go around it: x = 31 is next to x = −32.
    Scene scene("seam");
    SourceLog log;
    scene.setChunkSource(log.source());
    scene.setChunkStreaming(streaming());
    scene.setPhysicsFocus(GlobalPosition(8100.0, 0.0, 0.0)); // chunk 31
    runUntilLoaded(scene);
    CHECK(scene.focusChunk() == (ChunkCoord{31, 0, 0}));
    CHECK(scene.isChunkLoaded({-32, 0, 0}) && scene.isChunkLoaded({-31, 0, 0}) && scene.isChunkLoaded({29, 0, 0}));
    CHECK(!scene.isChunkLoaded({-30, 0, 0}) && scene.loadedChunks().size() == 25);
}

void hysteresis() {
    Scene scene("border");
    SourceLog log;
    scene.setChunkSource(log.source());
    scene.setChunkStreaming(streaming());
    scene.setPhysicsFocus(GlobalPosition(250.0, 0.0, 100.0));
    runUntilLoaded(scene);
    const size_t loaded = log.loads.size();
    // Back and forth over the border at x = 256, within 1/8 of a chunk (32 m): nothing changes.
    for (int i = 0; i < 60; ++i) {
        scene.setPhysicsFocus(GlobalPosition(i % 2 ? 280.0 : 240.0, 0.0, 100.0));
        scene.update(Duration::perSecond(10));
    }
    CHECK(scene.focusChunk() == (ChunkCoord{0, 0, 0}) && log.loads.size() == loaded && log.unloads.empty());
    // Past the margin: the focus chunk moves and one column comes in.
    scene.setPhysicsFocus(GlobalPosition(300.0, 0.0, 100.0));
    runUntilLoaded(scene);
    CHECK(scene.focusChunk() == (ChunkCoord{1, 0, 0}) && log.loads.size() == loaded + 5);
    // And back within the margin of the new chunk: still nothing.
    for (int i = 0; i < 60; ++i) {
        scene.setPhysicsFocus(GlobalPosition(i % 2 ? 240.0 : 270.0, 0.0, 100.0));
        scene.update(Duration::perSecond(10));
    }
    CHECK(scene.focusChunk() == (ChunkCoord{1, 0, 0}) && log.loads.size() == loaded + 5 && log.unloads.empty());
}

void unloading() {
    Scene scene("unload");
    SourceLog log;
    bool sawEntity = false;
    ChunkSource source = log.source();
    const auto unload = source.unload;
    source.unload = [&](Scene& s, const ChunkCoord& c) {
        unload(s, c);
        if (c == ChunkCoord{0, 0, 0}) sawEntity = s.findEntity("Rock").valid(); // still there to save
    };
    scene.setChunkSource(source);
    scene.setChunkStreaming(streaming()); // kept up to 7×7, unloaded 2 s after
    scene.setPhysicsFocus(GlobalPosition(10.0, 0.0, 10.0));
    runUntilLoaded(scene);
    Entity rock = scene.createEntity("Rock");
    rock.transform().position = {20.0, 0.0, 20.0};
    rock.add<StreamedComponent>();
    Entity carried = scene.createEntity("Carried");
    carried.transform().position = {30.0, 0.0, 20.0};
    carried.add<StreamedComponent>().unload = false;
    Entity player = scene.createEntity("Player"); // not streamed
    player.transform().position = {40.0, 0.0, 20.0};

    // Far away: chunk 0 is 8 chunks behind, out of the kept ring; unloaded only after 2 s.
    scene.setPhysicsFocus(GlobalPosition(2100.0, 0.0, 10.0));
    for (int i = 0; i < 3; ++i) scene.update(Duration::milliseconds(500));
    CHECK(scene.isChunkLoaded({0, 0, 0}) && rock.valid());
    scene.update(Duration::milliseconds(500));
    CHECK(!scene.isChunkLoaded({0, 0, 0}) && !rock.valid() && sawEntity);
    CHECK(carried.valid() && player.valid()); // kept loaded / not streamed
    CHECK(std::count(log.unloads.begin(), log.unloads.end(), ChunkCoord{0, 0, 0}) == 1);

    // A streamed entity that wandered into a chunk that is not loaded, beyond the kept ring: gone.
    Entity stray = scene.createEntity("Stray");
    stray.transform().position = {-3000.0, 0.0, 0.0};
    stray.add<StreamedComponent>();
    scene.update(Duration::perSecond(60));
    CHECK(!stray.valid());
}

void dormancy() {
    Scene scene("dormant");
    PhysicsSettings ps;
    ps.gravity = {0.0, -9.81, 0.0};
    scene.setPhysicsSettings(ps);
    scene.setChunkStreaming(streaming()); // physics in the 3×3 around the focus
    scene.setPhysicsFocus(GlobalPosition(0.0, 0.0, 0.0));
    Entity nearBox = scene.createEntity("Near");
    nearBox.transform().position = {10.0, 5.0, 0.0};
    nearBox.add<RigidBodyComponent>();
    Entity farBox = scene.createEntity("Far");
    farBox.transform().position = {2000.0, 5.0, 0.0};
    farBox.transform().setEulerDegrees({10, 20, 30});
    auto& rb = farBox.add<RigidBodyComponent>();
    rb.linearDamping = 0;
    rb.angularDamping = 0;
    rb.gravityFactor = 0;
    scene.startSimulation();
    for (int i = 0; i < 30; ++i) scene.update(Duration::perSecond(60));
    // The far one is out of the physics engine, exactly where it was; the near one falls.
    const TransformComponent kept = farBox.transform();
    CHECK(scene.isDormant(farBox) && !scene.isDormant(nearBox) && scene.physicsBodyCount() == 1);
    CHECK(nearBox.transform().position.toMeters().y < 5.0);
    // Velocities set while dormant are kept and applied when it wakes.
    const SpeedVec v = SpeedVec(1.5, 0.0, -0.75);
    const AngleSpeedVec w = AngleSpeedVec(0.0, 0.5, 0.0); // rad/s
    scene.setLinearVelocity(farBox, v);
    scene.setAngularVelocity(farBox, w);
    for (int i = 0; i < 30; ++i) scene.update(Duration::perSecond(60));
    CHECK(farBox.transform().position == kept.position && farBox.transform().rotation == kept.rotation);
    CHECK(scene.linearVelocity(farBox) == v && scene.angularVelocity(farBox) == w);
    // The focus comes near: it is back, with its pose and velocities, and moves on.
    scene.setPhysicsFocus(GlobalPosition(1990.0, 0.0, 0.0));
    scene.update(Duration::perSecond(60));
    CHECK(!scene.isDormant(farBox) && scene.physicsBodyCount() == 1); // the near box left the ring in turn
    CHECK(glm::length(scene.linearVelocity(farBox).toDouble() - v.toDouble()) < 1e-4);
    CHECK(std::abs(scene.angularVelocity(farBox).y.radiansPerSecond() - 0.5) < 1e-4);
    CHECK(farBox.transform().position.toMeters().x > 2000.0); // moved by its velocity
    // The near box is the dormant one now, and streaming off wakes everything.
    CHECK(scene.isDormant(nearBox));
    ChunkSettings off;
    scene.setChunkStreaming(off);
    scene.update(Duration::perSecond(60));
    CHECK(!scene.isDormant(nearBox) && scene.physicsBodyCount() == 2);
}

void dormantCharactersAndJoints() {
    Scene scene("dormant2");
    scene.setChunkStreaming(streaming());
    scene.setPhysicsFocus(GlobalPosition(0.0, 0.0, 0.0));
    Entity ground = scene.createEntity("Ground");
    ground.transform().position = {1000.0, -0.5, 0.0};
    ground.transform().scale = {40, 1, 40};
    ground.add<RigidBodyComponent>(physics::BodyType::Static);
    Entity walker = scene.createEntity("Walker");
    walker.transform().position = {1000.0, 0.0, 0.0};
    walker.add<CharacterControllerComponent>().velocity = {1.0, 0.0, 0.0};
    Entity door = scene.createEntity("Door");
    door.transform().position = {1005.0, 1.0, 0.0};
    door.add<RigidBodyComponent>();
    auto& hinge = door.add<JointComponent>();
    hinge.type = physics::JointType::Hinge;
    scene.startSimulation();
    for (int i = 0; i < 10; ++i) scene.update(Duration::perSecond(60));
    // Far away: the character, the bodies and the joint are out of the engine, kept as they were.
    CHECK(scene.isDormant(walker) && scene.isDormant(door) && !walker.get<CharacterControllerComponent>().character);
    CHECK(!door.get<JointComponent>().joint && walker.transform().position == GlobalPosition(1000.0, 0.0, 0.0));
    // Near: all back; the joint is made again, and the character walks on.
    scene.setPhysicsFocus(GlobalPosition(1000.0, 0.0, 0.0));
    for (int i = 0; i < 30; ++i) scene.update(Duration::perSecond(60));
    CHECK(!scene.isDormant(walker) && walker.get<CharacterControllerComponent>().character);
    CHECK(door.get<JointComponent>().joint && walker.transform().position.toMeters().x > 1000.2);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    nova::Log::init();
    nova::Log::core().set_level(spdlog::level::off);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"grid: chunk of a position, the axes chosen", grid},
        {"loading: the 5x5 nearest first, a budget per frame, any axes", loading},
        {"the seam: rings go around the circular world", seam},
        {"hysteresis: a border walked back and forth changes nothing", hysteresis},
        {"unloading: after a delay out of the kept ring, streamed entities", unloading},
        {"dormancy: out of the physics ring and back as it was", dormancy},
        {"dormancy of characters and joints", dormantCharactersAndJoints},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
