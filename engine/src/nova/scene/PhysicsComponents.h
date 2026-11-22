#pragma once

/// @file
/// @brief Physics components (frontend): what games put on entities. Scene turns them into bodies
///        of a physics::PhysicsWorld; no backend type appears here.

#include "nova/physics/PhysicsWorld.h"
#include "nova/renderer/Mesh.h"

#include <array>
#include <string>
#include <string_view>

namespace nova {

/// @brief Makes the entity a physics rigid body while the scene is simulating.
///
/// The shape comes from a Box/Sphere/CapsuleColliderComponent on the same entity; without one,
/// a box matching the entity's scale is used. Collider sizes are multiplied by the transform scale.
/// Body properties are read when the body is created (Scene::startSimulation or the first
/// update after adding the component); later edits apply on the next simulation start.
struct RigidBodyComponent {
    physics::BodyType type = physics::BodyType::Dynamic; ///< Static, kinematic (follows the transform) or dynamic.
    f32 mass = 1.0f;                                     ///< Mass in kg (dynamic only).
    f32 friction = 0.5f;                                 ///< Friction coefficient.
    f32 restitution = 0.1f;                              ///< Bounciness (0..1).
    f32 linearDamping = 0.05f;                           ///< Linear velocity damping.
    f32 angularDamping = 0.05f;                          ///< Angular velocity damping.
    f32 gravityFactor = 1.0f;                            ///< Gravity multiplier.
    bool isSensor = false;                               ///< Trigger: reports overlaps (Script::onTriggerEnter/Exit), no response.
    u32 layer = 0;                                       ///< Collision layer (Scene::physicsSettings() says which collide).

    physics::BodyId body; ///< Runtime body; valid while the scene is simulating. Managed by Scene.
};

/// @brief Box collision shape for a RigidBodyComponent.
struct BoxColliderComponent {
    glm::vec3 halfExtents{0.5f}; ///< Half size per axis, before scaling (0.5 = matches the unit cube mesh).
    glm::vec3 offset{0.0f};      ///< Center relative to the entity origin, before scaling.
};

/// @brief Sphere collision shape for a RigidBodyComponent.
struct SphereColliderComponent {
    f32 radius = 0.5f;      ///< Radius before scaling (multiplied by the largest scale axis).
    glm::vec3 offset{0.0f}; ///< Center relative to the entity origin, before scaling.
};

/// @brief Capsule collision shape (aligned with local Y) for a RigidBodyComponent.
struct CapsuleColliderComponent {
    f32 radius = 0.5f;      ///< Radius before scaling (multiplied by max(scale.x, scale.z)).
    f32 halfHeight = 0.5f;  ///< Half length of the cylindrical part, before scaling (multiplied by scale.y).
    glm::vec3 offset{0.0f}; ///< Center relative to the entity origin, before scaling.
};

/// @brief Cylinder collision shape (axis = local Y) for a RigidBodyComponent.
struct CylinderColliderComponent {
    f32 radius = 0.5f;      ///< Radius before scaling (multiplied by max(scale.x, scale.z)).
    f32 halfHeight = 0.5f;  ///< Half height before scaling (multiplied by scale.y).
    glm::vec3 offset{0.0f}; ///< Center relative to the entity origin, before scaling.
};

/// @brief Convex polyhedron collision shape: the convex hull of a mesh's vertices (scaled by the
///        transform). Works for any body type, dynamic included.
struct ConvexHullColliderComponent {
    MeshHandle mesh;        ///< Source mesh; invalid = the entity's MeshComponent mesh.
    glm::vec3 offset{0.0f}; ///< Shift relative to the entity origin, before scaling.
};

/// @brief Exact triangle-mesh collision shape (level geometry, glTF scenery). Static and kinematic
///        bodies only: on a dynamic body it falls back to the mesh's convex hull.
struct MeshColliderComponent {
    MeshHandle mesh; ///< Source mesh; invalid = the entity's MeshComponent mesh.
};

/// @brief Scene-wide physics settings (saved with the scene).
struct PhysicsSettings {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f}; ///< Gravity acceleration (m/s²).
    physics::CollisionLayers layers;       ///< Which layers collide (default: all).
    /// @brief Display names; empty = "Layer N" (layer 0 is "Default", the last one is the world edge).
    std::array<std::string, physics::kCollisionLayerCount> layerNames;

    /// @brief Name to show for layer `i`.
    std::string layerName(u32 i) const {
        if (i >= physics::kCollisionLayerCount) return {};
        if (!layerNames[i].empty()) return layerNames[i];
        if (i == 0) return "Default";
        if (i == physics::kWorldBoundaryLayer) return "World Edge";
        return "Layer " + std::to_string(i);
    }
    /// @brief Index of the layer with this name (as layerName() shows it), or −1.
    int findLayer(std::string_view name) const {
        for (u32 i = 0; i < physics::kCollisionLayerCount; ++i)
            if (layerName(i) == name) return static_cast<int>(i);
        return -1;
    }
    /// @brief Mask of the named layers (unknown names are ignored).
    u32 maskOf(std::initializer_list<std::string_view> names) const {
        u32 m = 0;
        for (std::string_view n : names)
            if (const int i = findLayer(n); i >= 0) m |= physics::layerBit(static_cast<u32>(i));
        return m;
    }
    bool operator==(const PhysicsSettings&) const = default;
};

} // namespace nova
