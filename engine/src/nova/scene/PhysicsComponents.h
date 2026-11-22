#pragma once

/// @file
/// @brief Physics components (frontend): what games put on entities. Scene turns them into bodies
///        of a physics::PhysicsWorld; no backend type appears here.

#include "nova/physics/PhysicsWorld.h"
#include "nova/renderer/Mesh.h"

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
    bool isSensor = false;                               ///< Trigger: reports collisions, no response.

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

} // namespace nova
