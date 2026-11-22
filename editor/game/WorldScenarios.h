#pragma once

// Two scenarios to see the world's shape in action: a closed arena (walls all around) and the
// seam of the 16 km torus (X and Z wrap: leaving +8 192 m enters at −8 192 m). Built by the editor
// commands Game/Scenarios/... and by the headless ScenarioTests, from the same code.

#include <nova/Nova.h>

/// Meshes the scenarios use (the renderer's built-ins in the editor; any handles in tests).
struct ScenarioMeshes {
    nova::MeshHandle cube;
    nova::MeshHandle sphere;
    nova::MeshHandle plane;
};

/// Which scenario to build.
enum class SmallWorld {
    Closed, ///< A 60 × 60 m arena with walls: things bounce off them.
    Open,   ///< A floor across the torus seam on X: things run through it and keep going.
};

/// Side of the closed arena, in meters.
constexpr double kSmallWorldSize = 60.0;
/// Length of the floor across the seam (along X, centered on the seam), in meters.
constexpr double kSeamFloorLength = 400.0;

/// Flashes when its body touches a wall of the arena and counts the touches.
struct EdgeFlash : nova::Script {
    float glow = 0.0f;
    int touches = 0;
    void onCollision(const nova::Collision& c) override;
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
    /// Bounces off walls like a billiard ball (mirrors `direction` across the wall).
    void onCollision(const nova::Collision& c) override;
};

/// True for the arena's walls.
bool isWall(nova::Entity e);

/// Registers EdgeFlash and Mover for the inspector and scene files.
void reflectScenarioTypes();

/// Replaces the scene's content with a scenario.
/// @return The primary camera entity.
nova::Entity buildSmallWorld(nova::Scene& scene, const ScenarioMeshes& meshes, SmallWorld kind);
