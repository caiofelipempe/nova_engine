#pragma once

/// @file
/// @brief Ray picking of scene entities (editor selection, placement tools).

#include "nova/scene/Scene.h"

#include <functional>
#include <limits>
#include <optional>

namespace nova::editor {

/// @brief What a pick ray hit.
struct PickHit {
    Entity entity;           ///< Hit entity.
    GlobalPosition point;    ///< World hit point (exact far from the origin).
    glm::vec3 normal{0.0f};  ///< World-space surface normal (facing the ray).
    f32 distance = 0.0f;     ///< Distance from the ray origin.
};

/// @brief Resolves a mesh handle to its CPU geometry (normally Renderer::meshData).
using MeshLookup = std::function<const MeshData*(MeshHandle)>;

/// @brief Options for pick().
struct PickOptions {
    /// @brief Radius of the pick sphere used for entities without a mesh (lights, cameras, empties). 0 = not pickable.
    f32 markerRadius = 0.3f;
    /// @brief Entities for which this returns true are ignored (e.g. the ones a brush is painting).
    std::function<bool(Entity)> ignore;
    f32 maxDistance = std::numeric_limits<f32>::infinity(); ///< Maximum ray length.
};

/// @brief Closest entity along a ray: exact triangles for meshes, a small sphere for the rest.
/// Geometry is tested relative to `origin`, so picking stays precise far from the world origin.
std::optional<PickHit> pick(Scene& scene, const MeshLookup& meshes, const GlobalPosition& origin,
                            const glm::vec3& direction, const PickOptions& options = {});

} // namespace nova::editor
