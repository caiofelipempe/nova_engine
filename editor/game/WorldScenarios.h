#pragma once

// Small worlds to see the world's edges in action: a closed one (barriers on every axis) and an
// open one (X and Z wrap around). Built by the editor commands Game/Scenarios/... and by the
// headless ScenarioTests, from the same code.

#include <nova/Nova.h>

/// Meshes the scenarios use (the renderer's built-ins in the editor; any handles in tests).
struct ScenarioMeshes {
    nova::MeshHandle cube;
    nova::MeshHandle sphere;
    nova::MeshHandle plane;
};

/// Which small world to build.
enum class SmallWorld {
    Closed, ///< 60 × 40 × 60 m, barrier on every axis: things bounce off the edges.
    Open,   ///< X and Z wrap every 60 m, Y is a 40 m barrier: things loop around.
};

/// Side of both small worlds along X and Z, in meters.
constexpr double kSmallWorldSize = 60.0;

/// Flashes when its body touches a world edge (Script::onWorldBoundary) and counts the touches.
struct EdgeFlash : nova::Script {
    float glow = 0.0f;
    int touches = 0;
    void onWorldBoundary(const nova::WorldBoundaryHit&) override {
        glow = 1.0f;
        ++touches;
    }
    void onUpdate(float dt) override;
};

/// Keeps its body moving: tops the velocity up along `direction` to `speed` (m/s); if `kickEvery`
/// is positive, also kicks it in a new pseudo-random direction that often (deterministic).
struct Mover : nova::Script {
    glm::vec3 direction{1.0f, 0.0f, 0.0f};
    float speed = 6.0f;
    float kickEvery = 0.0f;
    float sinceKick = 0.0f;
    unsigned seed = 1;
    void onUpdate(float dt) override;
    /// Bounces off world edges like a billiard ball (mirrors `direction` on the touched axis).
    void onWorldBoundary(const nova::WorldBoundaryHit& hit) override;
};

/// Registers EdgeFlash and Mover for the inspector and scene files.
void reflectScenarioTypes();

/// Replaces the scene's content with a small world (also sets its WorldSpace).
/// @return The primary camera entity.
nova::Entity buildSmallWorld(nova::Scene& scene, const ScenarioMeshes& meshes, SmallWorld kind);
