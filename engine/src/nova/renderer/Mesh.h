#pragma once

/// @file
/// @brief Vertex format, CPU mesh data, mesh handles and materials.

#include "nova/core/Base.h"
#include "nova/rhi/RHITypes.h"

#include <glm/glm.hpp>

#include <limits>
#include <optional>
#include <vector>

namespace nova {

/// @brief Axis-aligned bounding box.
struct Bounds {
    glm::vec3 min{0.0f}; ///< Minimum corner.
    glm::vec3 max{0.0f}; ///< Maximum corner.
    /// @brief Center point.
    glm::vec3 center() const { return (min + max) * 0.5f; }
    /// @brief Half size per axis.
    glm::vec3 extents() const { return (max - min) * 0.5f; }
};

/// @brief Result of raycastMesh().
struct MeshRayHit {
    f32 distance = 0.0f;     ///< World-space distance from the ray origin.
    glm::vec3 point{0.0f};   ///< World-space hit point.
    glm::vec3 normal{0.0f};  ///< World-space geometric normal of the hit triangle (facing the ray).
    u32 triangle = 0;        ///< Index of the hit triangle.
};

/// @brief 32-byte vertex, read in shaders through a buffer device address ("vertex pulling").
/// @note Layout mirrors `Vertex` in shaders/common.glsl (std430).
struct Vertex {
    glm::vec3 position{0.0f};            ///< Object-space position.
    f32 u = 0.0f;                        ///< Texture coordinate U.
    glm::vec3 normal{0.0f, 1.0f, 0.0f};  ///< Object-space normal (unit length).
    f32 v = 0.0f;                        ///< Texture coordinate V.
};
static_assert(sizeof(Vertex) == 32);

/// @brief CPU-side indexed triangle mesh. Triangles are counter-clockwise when seen from the front.
struct MeshData {
    std::vector<Vertex> vertices; ///< Vertex array.
    std::vector<u32> indices;     ///< Triangle list, 3 indices per triangle.

    /// @brief Axis-aligned cube centered at the origin with flat face normals.
    /// @param size Edge length.
    static MeshData cube(f32 size = 1.0f);
    /// @brief UV sphere centered at the origin.
    /// @param radius Sphere radius.
    /// @param segments Subdivisions around the Y axis.
    /// @param rings Subdivisions from pole to pole.
    static MeshData sphere(f32 radius = 0.5f, u32 segments = 48, u32 rings = 24);
    /// @brief Square on the XZ plane, centered at the origin, facing +Y.
    /// @param size Edge length.
    /// @param subdivisions Quads per side.
    static MeshData plane(f32 size = 1.0f, u32 subdivisions = 1);

    /// @brief Bounding box of the vertices (zero box when empty).
    Bounds bounds() const;
};

/// @brief Exact ray/mesh intersection (CPU): bounding-box early-out, then every triangle.
///
/// Used for editor picking and surface placement; not meant for per-frame gameplay queries
/// (use the physics scene for those).
/// @param mesh Geometry in object space.
/// @param transform Object → world (may include non-uniform scale).
/// @param origin Ray origin (world).
/// @param direction Ray direction (world; need not be normalized).
/// @param maxDistance Maximum world distance.
/// @return Closest hit, or `std::nullopt`.
std::optional<MeshRayHit> raycastMesh(const MeshData& mesh, const glm::mat4& transform, const glm::vec3& origin,
                                      const glm::vec3& direction,
                                      f32 maxDistance = std::numeric_limits<f32>::infinity());

/// @brief Lightweight reference to a GPU mesh owned by the Renderer.
struct MeshHandle {
    u32 id = ~0u; ///< Slot in the renderer's mesh table; `~0u` = invalid.
    /// @brief True unless default-constructed.
    bool valid() const { return id != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const MeshHandle&) const = default;
};

/// @brief Reference to a shader loaded by the ShaderLibrary (Renderer::shaders()).
struct ShaderHandle {
    u32 id = ~0u; ///< Slot in the shader library; `~0u` = invalid.
    /// @brief True unless default-constructed.
    bool valid() const { return id != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const ShaderHandle&) const = default;
};

/// @brief Metallic-roughness PBR surface parameters.
struct Material {
    glm::vec4 baseColor{1.0f};    ///< Linear RGBA albedo (metals: specular color).
    f32 metallic = 0.0f;          ///< 0 = dielectric, 1 = metal.
    f32 roughness = 0.5f;         ///< 0 = mirror, 1 = fully rough (clamped to 0.04 in shaders).
    glm::vec3 emissive{0.0f};     ///< Emitted linear RGB color.
    f32 emissiveStrength = 0.0f;  ///< Multiplier for `emissive` (HDR; values > 1 bloom-ready).
    bool checker = false;         ///< Procedural world-space checker pattern (handy for floors).
    rhi::TextureHandle baseColorTexture; ///< Optional albedo texture (multiplies baseColor), sampled with the mesh UVs.
    /// @brief Optional game material: a fragment shader written against `nova/material.glsl`
    ///        (invalid = the engine's default shading).
    ShaderHandle shader;
    glm::vec4 customParams{0.0f};     ///< Free parameters for `shader` (`Surface::custom` in GLSL).
    rhi::TextureHandle customTexture; ///< Extra texture for `shader` (`nova_customTexture(uv)` in GLSL).
};

} // namespace nova
