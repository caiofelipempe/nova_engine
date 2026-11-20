#pragma once

#include "nova/core/Base.h"

#include <glm/glm.hpp>

#include <vector>

namespace nova {

// 32-byte vertex, read in shaders through a buffer device address ("vertex pulling").
struct Vertex {
    glm::vec3 position{0.0f};
    f32 u = 0.0f;
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    f32 v = 0.0f;
};
static_assert(sizeof(Vertex) == 32);

// CPU-side geometry. Triangles are counter-clockwise when seen from the front.
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    static MeshData cube(f32 size = 1.0f);
    static MeshData sphere(f32 radius = 0.5f, u32 segments = 48, u32 rings = 24);
    static MeshData plane(f32 size = 1.0f, u32 subdivisions = 1);
};

// Lightweight reference to a GPU mesh owned by the Renderer.
struct MeshHandle {
    u32 id = ~0u;
    bool valid() const { return id != ~0u; }
    explicit operator bool() const { return valid(); }
    bool operator==(const MeshHandle&) const = default;
};

struct Material {
    glm::vec4 baseColor{1.0f};
    f32 metallic = 0.0f;
    f32 roughness = 0.5f;
    glm::vec3 emissive{0.0f};
    f32 emissiveStrength = 0.0f;
    bool checker = false; // procedural world-space checker (handy for floors)
};

} // namespace nova
