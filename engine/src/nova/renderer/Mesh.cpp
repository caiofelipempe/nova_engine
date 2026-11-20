#include "nova/renderer/Mesh.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace nova {

namespace {
// Adds a quad facing `n`, with u x v == n so the winding is CCW seen from the front.
void addFace(MeshData& m, glm::vec3 center, glm::vec3 n, glm::vec3 u, glm::vec3 v, u32 subdiv) {
    const u32 base = static_cast<u32>(m.vertices.size());
    const u32 stride = subdiv + 1;
    for (u32 y = 0; y <= subdiv; ++y) {
        for (u32 x = 0; x <= subdiv; ++x) {
            f32 fx = static_cast<f32>(x) / subdiv;
            f32 fy = static_cast<f32>(y) / subdiv;
            Vertex vert;
            vert.position = center + u * (fx * 2.0f - 1.0f) + v * (fy * 2.0f - 1.0f);
            vert.normal = n;
            vert.u = fx;
            vert.v = 1.0f - fy;
            m.vertices.push_back(vert);
        }
    }
    for (u32 y = 0; y < subdiv; ++y) {
        for (u32 x = 0; x < subdiv; ++x) {
            u32 i0 = base + y * stride + x;
            u32 i1 = i0 + 1;
            u32 i2 = i0 + stride + 1;
            u32 i3 = i0 + stride;
            m.indices.insert(m.indices.end(), {i0, i1, i2, i0, i2, i3});
        }
    }
}
} // namespace

MeshData MeshData::cube(f32 size) {
    MeshData m;
    const f32 h = size * 0.5f;
    const glm::vec3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
    addFace(m, X * h, X, -Z * h, Y * h, 1);
    addFace(m, -X * h, -X, Z * h, Y * h, 1);
    addFace(m, Y * h, Y, X * h, -Z * h, 1);
    addFace(m, -Y * h, -Y, X * h, Z * h, 1);
    addFace(m, Z * h, Z, X * h, Y * h, 1);
    addFace(m, -Z * h, -Z, -X * h, Y * h, 1);
    return m;
}

MeshData MeshData::plane(f32 size, u32 subdivisions) {
    MeshData m;
    const f32 h = size * 0.5f;
    addFace(m, glm::vec3(0.0f), {0, 1, 0}, {h, 0, 0}, {0, 0, -h}, std::max(1u, subdivisions));
    return m;
}

MeshData MeshData::sphere(f32 radius, u32 segments, u32 rings) {
    MeshData m;
    for (u32 r = 0; r <= rings; ++r) {
        f32 theta = glm::pi<f32>() * r / rings; // 0 at the top
        for (u32 s = 0; s <= segments; ++s) {
            f32 phi = glm::two_pi<f32>() * s / segments;
            glm::vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            Vertex v;
            v.position = n * radius;
            v.normal = n;
            v.u = static_cast<f32>(s) / segments;
            v.v = static_cast<f32>(r) / rings;
            m.vertices.push_back(v);
        }
    }
    const u32 stride = segments + 1;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            u32 a = r * stride + s;        // top, phi
            u32 b = (r + 1) * stride + s;  // bottom, phi
            u32 c = b + 1;                 // bottom, phi+
            u32 d = a + 1;                 // top, phi+
            m.indices.insert(m.indices.end(), {a, d, c, a, c, b});
        }
    }
    return m;
}

} // namespace nova
