#pragma once

/// @file
/// @brief Physics types shared by the global contract (PhysicsWorld) and the float-local one
///        (local::World): backends, body types, body-local shapes, ids, filters.

#include "nova/core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <vector>

namespace nova::physics {

/// @brief Available physics backends.
enum class Backend {
    Jolt,     ///< Jolt Physics (multi-threaded, float; used through FloatingOriginWorld).
    Skeleton, ///< SkeletonPhysicsWorld: native integer positions, minimal collision (starting point for a custom engine).
};

/// @brief How a body moves.
enum class BodyType : u8 {
    Static,    ///< Never moves (level geometry).
    Kinematic, ///< Moved by code/animation; pushes dynamic bodies but is not pushed back.
    Dynamic,   ///< Fully simulated (gravity, forces, collisions).
};

/// @brief Collision shape kinds.
enum class ShapeType : u8 {
    Box,        ///< ShapeDesc::halfExtents.
    Sphere,     ///< ShapeDesc::radius.
    Capsule,    ///< ShapeDesc::radius and ShapeDesc::halfHeight, aligned with local Y.
    Cylinder,   ///< ShapeDesc::radius and ShapeDesc::halfHeight, aligned with local Y.
    ConvexHull, ///< Convex polyhedron: the hull of ShapeDesc::geometry->points (any body type).
    Mesh,       ///< Triangle mesh: ShapeDesc::geometry points + indices. Static/kinematic bodies only.
};

/// @brief Vertices (and triangles) of a ConvexHull or Mesh shape, local to the body.
///
/// A convex polyhedron is given by its **points**: backends derive what their collision needs
/// (Jolt builds its own hull with faces and planes; a custom engine would build a half-edge
/// structure). Shared by pointer: many bodies and regions can use one geometry without copies.
struct ShapeGeometry {
    std::vector<glm::vec3> points; ///< Local vertices.
    std::vector<u32> indices;      ///< Mesh: 3 per triangle, counter-clockwise from outside. Unused by hulls.
};

/// @brief Collision shape, in coordinates **local to the body** (sizes final, already scaled).
///
/// Shapes stay small and local whatever the body's world position: only the body itself has a
/// global (integer) position.
struct ShapeDesc {
    ShapeType type = ShapeType::Box; ///< Primitive.
    glm::vec3 halfExtents{0.5f};     ///< Box: half size on each axis.
    f32 radius = 0.5f;               ///< Sphere / Capsule radius.
    f32 halfHeight = 0.5f;           ///< Capsule: half length of the cylindrical part (along local Y).
    glm::vec3 offset{0.0f};          ///< Shape center relative to the body origin (local space).
    std::shared_ptr<const ShapeGeometry> geometry; ///< ConvexHull / Mesh vertices (already scaled).
};

/// @brief Bit of a ShapeType in a shape mask (PhysicsCaps).
constexpr u32 shapeBit(ShapeType t) { return 1u << static_cast<u32>(t); }
/// @brief Mask with every shape type.
constexpr u32 kAllShapes = (1u << 6) - 1;

/// @brief What a physics backend supports (PhysicsWorld::caps()).
///
/// The engine and the contract tests use it to pick code paths and to skip what a backend does
/// not do yet; games can show it or refuse a backend that lacks something they need.
struct PhysicsCaps {
    const char* name = "";                   ///< Backend name, for logs and UI.
    /// @brief Works in GlobalPosition natively: no floating origin, a single region at the world
    ///        origin, no float positions anywhere between the scene and the simulation.
    bool globalPositions = false;
    /// @brief Same inputs give bit-identical results on every platform/compiler (integer or
    ///        fixed-point math only). Every backend must at least be deterministic on one build.
    bool crossPlatformDeterministic = false;
    bool regions = false;                    ///< May split the simulation in several regions (regionCount() > 1).
    bool dormancy = false;                   ///< Honours SimulationFocus::radius (far bodies become dormant).
    bool angularMotion = false;              ///< Contacts make bodies spin (torques, friction, off-center hits).
    bool friction = false;                   ///< Contacts apply friction.
    /// @brief `collides[a]`: mask of the shapes (shapeBit) a body of shape `a` collides with and
    ///        reports contacts against. Bodies of other shapes are still created and moved.
    u32 collides[6] = {};
    u32 raycastShapes = 0;                   ///< Shapes raycasts can hit.
    u32 sweepShapes = 0;                     ///< Shapes the sweep queries accept as the moving shape.
    u32 sweepTargets = 0;                    ///< Shapes sweeps can hit.

    /// @brief True if bodies of shapes `a` and `b` collide.
    bool collidesWith(ShapeType a, ShapeType b) const {
        return (collides[static_cast<u32>(a)] & shapeBit(b)) != 0;
    }
};

/// @brief Identifier of a body inside a world.
struct BodyId {
    u32 value = ~0u; ///< Backend-specific id; `~0u` = invalid.
    /// @brief True unless default-constructed. A valid id may refer to a destroyed body.
    bool valid() const { return value != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const BodyId&) const = default;
};

/// @brief Which bodies a query may report.
struct QueryFilter {
    BodyId ignoreBody;           ///< Body to skip, e.g. the caller itself.
    bool includeSensors = false; ///< Report sensor bodies too.
};

/// @brief World creation parameters.
struct WorldDesc {
    glm::vec3 gravity{0.0f, -9.81f, 0.0f}; ///< Gravity acceleration (m/s²).
    u32 maxBodies = 65536;                 ///< Body capacity.
    u32 workerThreads = 0;                 ///< Simulation threads; 0 = hardware threads − 1.
};

/// @brief userData of contacts/hits against a barrier edge of the world (see WorldSpace).
///
/// The top bit marks them; `code & 0xFF` is `axis * 2 + (high edge ? 1 : 0)`. Backends that
/// enforce barriers report them this way; the BodyId of the edge is invalid.
constexpr u64 kWorldBoundaryUserData = 1ull << 63;
/// @brief True if `userData` comes from a barrier edge of the world.
constexpr bool isWorldBoundary(u64 userData) { return (userData & kWorldBoundaryUserData) != 0; }

} // namespace nova::physics
