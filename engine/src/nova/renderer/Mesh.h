#pragma once

/// @file
/// @brief Vertex format, CPU mesh data, mesh handles and materials.

#include "nova/core/Base.h"

#include <glm/glm.hpp>

#include <vector>

namespace nova {

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
};

/// @brief Lightweight reference to a GPU mesh owned by the Renderer.
struct MeshHandle {
    u32 id = ~0u; ///< Slot in the renderer's mesh table; `~0u` = invalid.
    /// @brief True unless default-constructed.
    bool valid() const { return id != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const MeshHandle&) const = default;
};

/// @brief Metallic-roughness PBR surface parameters.
struct Material {
    glm::vec4 baseColor{1.0f};    ///< Linear RGBA albedo (metals: specular color).
    f32 metallic = 0.0f;          ///< 0 = dielectric, 1 = metal.
    f32 roughness = 0.5f;         ///< 0 = mirror, 1 = fully rough (clamped to 0.04 in shaders).
    glm::vec3 emissive{0.0f};     ///< Emitted linear RGB color.
    f32 emissiveStrength = 0.0f;  ///< Multiplier for `emissive` (HDR; values > 1 bloom-ready).
    bool checker = false;         ///< Procedural world-space checker pattern (handy for floors).
};

} // namespace nova
