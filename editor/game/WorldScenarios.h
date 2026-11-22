#pragma once

// Scenarios to see the world's edges in action (Scene::worldBounds(), enforced outside the physics
// engine): a small world with barriers, the same world wrapping around, and the seam of the whole
// 16 km torus. Built by the editor commands Game/Scenarios/... and by the headless ScenarioTests,
// from the same code.

#include <nova/Nova.h>

/// Meshes the scenarios use (the renderer's built-ins in the editor; any handles in tests).
struct ScenarioMeshes {
    nova::MeshHandle cube;
    nova::MeshHandle sphere;
    nova::MeshHandle plane;
};

/// Which scenario to build.
enum class SmallWorld {
    Barrier,   ///< A 60 × 60 m world with barriers on X and Z: centers stop at the edges.
    Wrap,      ///< The same 60 × 60 m world wrapping around on X and Z: out one side, in the other.
    TorusSeam, ///< A floor across the seam of the whole 16 km torus on X: things run through it.
};

/// Side of the small worlds (X and Z), in meters.
constexpr double kSmallWorldSize = 60.0;
/// Length of the floor across the seam (along X, centered on the seam), in meters.
constexpr double kSeamFloorLength = 400.0;

/// Flashes when its center reaches an edge of the world and counts it: red for a barrier, cyan
/// for a wrap.
struct EdgeFlash : nova::Script {
    float glow = 0.0f;
    int barrierHits = 0;
    int wraps = 0;
    bool lastWasBarrier = false;
    void onWorldEdge(const nova::WorldEdgeHit& hit) override;
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
    /// Turns back at a barrier (mirrors `direction` along that axis); keeps going through a wrap.
    void onWorldEdge(const nova::WorldEdgeHit& hit) override;
};

/// Registers EdgeFlash and Mover for the inspector and scene files.
void reflectScenarioTypes();

/// Replaces the scene's content with a scenario.
/// @return The primary camera entity.
nova::Entity buildSmallWorld(nova::Scene& scene, const ScenarioMeshes& meshes, SmallWorld kind);
