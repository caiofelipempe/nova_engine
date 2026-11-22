#pragma once

// Meshes the renderer has no built-in for, matching the physics collider shapes (all centered on the
// origin, axis = Y, sized like the collider components' defaults).
#include <nova/Nova.h>

namespace shapes {

/// Capsule: cylinder of half height `halfHeight` with hemispheres of `radius`, centered at `centerY`.
nova::MeshData capsule(float radius = 0.5f, float halfHeight = 0.5f, float centerY = 0.0f, unsigned segments = 32);
/// Cylinder with flat caps.
nova::MeshData cylinder(float radius = 0.5f, float halfHeight = 0.5f, unsigned segments = 32);
/// A faceted gem (a few sides): an obviously convex, non-primitive shape for convex hull colliders.
nova::MeshData gem();
/// A `size` × `size` bumpy terrain patch on XZ (heights 0 .. `height`), for triangle mesh colliders.
nova::MeshData terrain(float size = 24.0f, unsigned cells = 48, float height = 1.6f);

} // namespace shapes
