#pragma once

// Scenarios to see the circular world in action (Scene::worldSize()): a small world wrapping around
// on X and Z, and the seam of the default 16 km world. Built by the editor commands
// Game/Scenarios/... and by the headless ScenarioTests, from the same code.

#include <nova/Nova.h>

/// Meshes the scenarios use (the renderer's built-ins in the editor; any handles in tests).
struct ScenarioMeshes {
    nova::MeshHandle cube;
    nova::MeshHandle sphere;
    nova::MeshHandle plane;
};

/// Which scenario to build.
enum class SmallWorld {
    Wrap,      ///< A 60 × 60 m world (X and Z): out one side, in the other, colliding across the seam.
    TorusSeam, ///< A floor across the X seam of the default 16 km world: things run through it.
};

/// Side of the small worlds (X and Z), in meters.
constexpr double kSmallWorldSize = 60.0;
/// Length of the floor across the seam (along X, centered on the seam), in meters.
constexpr double kSeamFloorLength = 400.0;

/// Flashes (cyan) when the physics carries it across a seam of the world, and counts it.
struct EdgeFlash : nova::Script {
    float glow = 0.0f;
    int wraps = 0;
    void onWorldWrap(const nova::WorldWrapHit& hit) override;
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
};

/// Registers EdgeFlash and Mover for the inspector and scene files.
void reflectScenarioTypes();

/// Replaces the scene's content with a scenario.
/// @return The primary camera entity.
nova::Entity buildSmallWorld(nova::Scene& scene, const ScenarioMeshes& meshes, SmallWorld kind);
