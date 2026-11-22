#pragma once

// The physics test bench's scenarios: each one builds a scene that exercises part of the physics
// (stacking, materials, shapes, joints, CCD, characters, sensors and layers, world edges).
// Builders only use the scene: the same scenario can be built in a hidden scene to check that two
// runs give the same state hash step after step (determinism).
#include <nova/Nova.h>

#include <string>
#include <vector>

/// Meshes the scenarios draw with.
struct BenchMeshes {
    nova::MeshHandle cube, sphere, plane;
    nova::MeshHandle capsule, cylinder, gem, terrain;
    nova::MeshHandle player; ///< The character's capsule, feet at the origin (radius 0.3, 1.8 m tall).
};

/// Where the camera starts in a scenario.
struct View {
    nova::GlobalPosition eye;
    nova::GlobalPosition target;
};

/// A recent contact, drawn as an arrow (contact point, normal) while it fades out.
struct ContactMark {
    nova::GlobalPosition point;
    glm::vec3 normal{0.0f};
    float age = 0.0f;
};
/// Contacts recorded by FlashOnHit (cleared and aged by the bench).
std::vector<ContactMark>& contactMarks();
/// When false, FlashOnHit records nothing (hidden determinism runs).
extern bool gRecordContacts;

/// Glows briefly when its body starts touching something (and records the contact).
struct FlashOnHit : nova::Script {
    float flash = 0.0f;
    glm::vec3 color{1.0f, 0.6f, 0.2f};
    void onCollision(const nova::Collision& c) override;
    void onUpdate(float dt) override;
};

/// One scenario of the bench.
struct Scenario {
    const char* name;
    const char* hint;                                       ///< What to look at / try.
    View (*build)(nova::Scene&, const BenchMeshes&);        ///< Fills an empty scene.
    void (*fire)(nova::Scene&, const BenchMeshes&, std::vector<nova::Entity>& spawned) = nullptr; ///< Optional action.
    const char* fireLabel = nullptr;                        ///< Button text of `fire`.
    float autoFire = 0.0f;                                  ///< Seconds between automatic `fire` calls (0 = never).
    void (*status)(nova::Scene&) = nullptr;                 ///< Optional ImGui lines (measurements).
    bool character = false;                                 ///< A "Player" character the bench drives.
    bool gamepads = false;                                  ///< The bench spawns a ball per gamepad.
};

/// Every scenario, in menu order.
const std::vector<Scenario>& scenarios();
