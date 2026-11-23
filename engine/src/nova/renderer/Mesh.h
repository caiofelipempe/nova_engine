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

/// @brief How a vertex follows a skeleton (skinned meshes): up to four joints and their weights.
struct SkinVertex {
    u32 joints01 = 0;          ///< Joints 0 and 1 (16 bits each, low first): indices into the skin's joints.
    u32 joints23 = 0;          ///< Joints 2 and 3.
    u32 _pad[2] = {0, 0};
    glm::vec4 weights{1.0f, 0.0f, 0.0f, 0.0f}; ///< Weights of the four joints (sum 1).

    /// @brief Sets the four joints.
    void setJoints(u32 a, u32 b, u32 c, u32 d) {
        joints01 = (a & 0xFFFFu) | ((b & 0xFFFFu) << 16);
        joints23 = (c & 0xFFFFu) | ((d & 0xFFFFu) << 16);
    }
};
static_assert(sizeof(SkinVertex) == 32);

/// @brief CPU-side indexed triangle mesh. Triangles are counter-clockwise when seen from the front.
struct MeshData {
    std::vector<Vertex> vertices; ///< Vertex array.
    std::vector<u32> indices;     ///< Triangle list, 3 indices per triangle.
    /// @brief Skinned meshes: one per vertex (empty: a rigid mesh). The vertices are in the bind
    ///        pose; a DrawItem with joint matrices deforms them (SkinComponent).
    std::vector<SkinVertex> skin;

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
    /// @brief A skinned mesh in a pose: the vertices moved by `joints` (one matrix per joint, mesh
    ///        space, as Scene::skinPose gives), the same math as the shaders; no skin in the copy.
    ///        A rigid mesh (or no joints) comes back as it is.
    MeshData posed(const std::vector<glm::mat4>& joints) const;
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
/// @brief How a material's alpha is used (as glTF's alphaMode).
enum class AlphaMode : u8 {
    Opaque, ///< Alpha ignored.
    Mask,   ///< Cut out where alpha < Material::alphaCutoff (foliage, fences).
    Blend,  ///< See-through by alpha (glass): drawn after the opaque, back to front, casting no ray-traced shadow.
};

struct Material {
    glm::vec4 baseColor{1.0f};    ///< Linear RGBA albedo (metals: specular color); alpha for AlphaMode::Mask / Blend.
    f32 metallic = 0.0f;          ///< 0 = dielectric, 1 = metal.
    f32 roughness = 0.5f;         ///< 0 = mirror, 1 = fully rough (clamped to 0.04 in shaders).
    glm::vec3 emissive{0.0f};     ///< Emitted linear RGB color.
    f32 emissiveStrength = 0.0f;  ///< Multiplier for `emissive` (HDR; values > 1 bloom-ready).
    bool checker = false;         ///< Procedural world-space checker pattern (handy for floors).
    rhi::TextureHandle baseColorTexture; ///< Optional albedo texture (sRGB; multiplies baseColor), sampled with the mesh UVs.
    rhi::TextureHandle normalTexture;    ///< Optional tangent-space normal map (linear; glTF convention: +Y up).
    f32 normalScale = 1.0f;              ///< Strength of the normal map's tilt (glTF normalTexture.scale).
    rhi::TextureHandle metallicRoughnessTexture; ///< Optional (linear; glTF): G multiplies roughness, B metallic.
    rhi::TextureHandle occlusionTexture; ///< Optional ambient occlusion (linear; R): darkens the ambient light.
    f32 occlusionStrength = 1.0f;        ///< 0 = no occlusion, 1 = all of it.
    rhi::TextureHandle emissiveTexture;  ///< Optional (sRGB): multiplies emissive.
    AlphaMode alphaMode = AlphaMode::Opaque; ///< How alpha is used.
    f32 alphaCutoff = 0.5f;              ///< AlphaMode::Mask: cut where alpha is below this.
    bool doubleSided = false;            ///< Both faces drawn (otherwise back faces are culled).
    /// @brief Optional game material: a fragment shader written against `nova/material.glsl`
    ///        (invalid = the engine's default shading).
    ShaderHandle shader;
    glm::vec4 customParams{0.0f};     ///< Free parameters for `shader` (`Surface::custom` in GLSL).
    rhi::TextureHandle customTexture; ///< Extra texture for `shader` (`nova_customTexture(uv)` in GLSL).
};

} // namespace nova
