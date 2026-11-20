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

Bounds MeshData::bounds() const {
    if (vertices.empty()) return {};
    Bounds b{vertices[0].position, vertices[0].position};
    for (const Vertex& v : vertices) {
        b.min = glm::min(b.min, v.position);
        b.max = glm::max(b.max, v.position);
    }
    return b;
}

namespace {
// Slab test against an AABB in local space. Returns entry distance (local units) or nullopt.
std::optional<f32> rayBox(const Bounds& b, const glm::vec3& o, const glm::vec3& d) {
    f32 tmin = 0.0f, tmax = std::numeric_limits<f32>::infinity();
    for (int a = 0; a < 3; ++a) {
        if (std::abs(d[a]) < 1e-12f) {
            if (o[a] < b.min[a] - 1e-5f || o[a] > b.max[a] + 1e-5f) return std::nullopt;
            continue;
        }
        f32 t1 = (b.min[a] - o[a]) / d[a], t2 = (b.max[a] - o[a]) / d[a];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax + 1e-6f) return std::nullopt;
    }
    return tmin;
}
} // namespace

std::optional<MeshRayHit> raycastMesh(const MeshData& mesh, const glm::mat4& transform, const glm::vec3& origin,
                                      const glm::vec3& direction, f32 maxDistance) {
    if (mesh.indices.size() < 3 || glm::dot(direction, direction) < 1e-20f) return std::nullopt;
    const glm::vec3 dirWorld = glm::normalize(direction);
    const glm::mat4 inv = glm::inverse(transform);
    // Local ray: t is shared between spaces because the direction is not renormalized.
    const glm::vec3 o = glm::vec3(inv * glm::vec4(origin, 1.0f));
    const glm::vec3 d = glm::vec3(inv * glm::vec4(dirWorld, 0.0f));

    if (!rayBox(mesh.bounds(), o, d)) return std::nullopt;

    f32 bestT = maxDistance;
    std::optional<u32> bestTri;
    glm::vec3 bestNormalLocal{0.0f};
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const glm::vec3& a = mesh.vertices[mesh.indices[i]].position;
        const glm::vec3& b = mesh.vertices[mesh.indices[i + 1]].position;
        const glm::vec3& c = mesh.vertices[mesh.indices[i + 2]].position;
        // Möller–Trumbore (two-sided)
        const glm::vec3 e1 = b - a, e2 = c - a;
        // Skip degenerate (zero-area) triangles, e.g. at sphere poles: their determinant is only
        // "almost" zero after rounding and would produce garbage hits.
        const glm::vec3 faceNormal = glm::cross(e1, e2);
        const f32 area2 = glm::dot(faceNormal, faceNormal);
        if (area2 <= 1e-12f * glm::dot(e1, e1) * glm::dot(e2, e2)) continue;
        const glm::vec3 p = glm::cross(d, e2);
        const f32 det = glm::dot(e1, p);
        if (std::abs(det) < 1e-12f) continue;
        const f32 invDet = 1.0f / det;
        // Small tolerance on the barycentric bounds: rays through a shared vertex/edge must hit at
        // least one of the adjacent triangles despite rounding.
        constexpr f32 kEdgeEps = 1e-5f;
        const glm::vec3 s = o - a;
        const f32 u = glm::dot(s, p) * invDet;
        if (u < -kEdgeEps || u > 1.0f + kEdgeEps) continue;
        const glm::vec3 q = glm::cross(s, e1);
        const f32 v = glm::dot(d, q) * invDet;
        if (v < -kEdgeEps || u + v > 1.0f + kEdgeEps) continue;
        const f32 t = glm::dot(e2, q) * invDet; // world units: |dirWorld| = 1
        if (t < 0.0f || t >= bestT) continue;
        bestT = t;
        bestTri = static_cast<u32>(i / 3);
        bestNormalLocal = faceNormal;
    }
    if (!bestTri) return std::nullopt;

    MeshRayHit hit;
    hit.distance = bestT;
    hit.point = origin + dirWorld * bestT;
    hit.triangle = *bestTri;
    glm::vec3 n = glm::normalize(glm::vec3(glm::transpose(inv) * glm::vec4(bestNormalLocal, 0.0f)));
    hit.normal = glm::dot(n, dirWorld) > 0.0f ? -n : n;
    return hit;
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
