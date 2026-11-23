#include "Shapes.h"

#include <cmath>
#include <vector>

namespace shapes {
namespace {

constexpr float kPi = 3.14159265358979f;

// One point of a lathe profile: radius, height and the (radial, vertical) normal there.
struct ProfilePoint {
    float r, y, nr, ny;
};

// Revolves a profile (bottom to top) around Y. Consecutive points are joined; a repeated point with
// another normal makes a hard edge (its degenerate triangles cover no pixels).
nova::MeshData lathe(const std::vector<ProfilePoint>& profile, unsigned segments) {
    nova::MeshData m;
    const unsigned rings = static_cast<unsigned>(profile.size());
    for (unsigned j = 0; j < rings; ++j) {
        const ProfilePoint& p = profile[j];
        for (unsigned s = 0; s <= segments; ++s) {
            const float a = 2.0f * kPi * static_cast<float>(s) / static_cast<float>(segments);
            const float sa = std::sin(a), ca = std::cos(a);
            nova::Vertex v;
            v.position = {p.r * sa, p.y, p.r * ca};
            v.normal = glm::normalize(glm::vec3(p.nr * sa, p.ny, p.nr * ca));
            v.u = static_cast<float>(s) / static_cast<float>(segments);
            v.v = static_cast<float>(j) / static_cast<float>(rings - 1);
            m.vertices.push_back(v);
        }
    }
    const unsigned row = segments + 1;
    for (unsigned j = 0; j + 1 < rings; ++j)
        for (unsigned s = 0; s < segments; ++s) {
            const unsigned a = j * row + s, b = a + 1, c = a + row + 1, d = a + row;
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d}); // counter-clockwise seen from outside
        }
    return m;
}

} // namespace

nova::MeshData capsule(float radius, float halfHeight, float centerY, unsigned segments) {
    std::vector<ProfilePoint> p;
    constexpr int kCap = 10;
    for (int i = 0; i <= kCap; ++i) { // bottom hemisphere, pole to equator
        const float phi = -kPi / 2 + kPi / 2 * static_cast<float>(i) / kCap;
        p.push_back({radius * std::cos(phi), centerY - halfHeight + radius * std::sin(phi), std::cos(phi), std::sin(phi)});
    }
    for (int i = 0; i <= kCap; ++i) { // top hemisphere, equator to pole
        const float phi = kPi / 2 * static_cast<float>(i) / kCap;
        p.push_back({radius * std::cos(phi), centerY + halfHeight + radius * std::sin(phi), std::cos(phi), std::sin(phi)});
    }
    return lathe(p, segments);
}

nova::MeshData cylinder(float radius, float halfHeight, unsigned segments) {
    const float h = halfHeight;
    return lathe({{0, -h, 0, -1}, {radius, -h, 0, -1}, // bottom cap
                  {radius, -h, 1, 0}, {radius, h, 1, 0}, // side
                  {radius, h, 0, 1}, {0, h, 0, 1}},      // top cap
                 segments);
}

nova::MeshData gem() {
    // Pavilion, girdle and crown: six sides, so the convex hull has clear facets.
    auto n = [](float r, float y) { return glm::normalize(glm::vec2(r, y)); };
    const glm::vec2 lower = n(0.45f, -0.5f), side = n(1.0f, 0.0f), upper = n(0.3f, 0.35f);
    return lathe({{0.0f, -0.5f, lower.x, lower.y}, {0.5f, 0.0f, lower.x, lower.y},
                  {0.5f, 0.0f, side.x, side.y}, {0.5f, 0.1f, side.x, side.y},
                  {0.5f, 0.1f, upper.x, upper.y}, {0.28f, 0.4f, upper.x, upper.y},
                  {0.28f, 0.4f, 0, 1}, {0.0f, 0.4f, 0, 1}},
                 6);
}

nova::MeshData terrain(float size, unsigned cells, float height) {
    auto heightAt = [&](float x, float z) {
        return height * 0.5f * (1.0f + std::sin(x * 0.45f) * std::cos(z * 0.35f + 0.6f));
    };
    nova::MeshData m;
    const float step = size / static_cast<float>(cells);
    for (unsigned j = 0; j <= cells; ++j)
        for (unsigned i = 0; i <= cells; ++i) {
            const float x = -size / 2 + step * static_cast<float>(i), z = -size / 2 + step * static_cast<float>(j);
            const float e = 0.01f;
            const glm::vec3 dx(2 * e, heightAt(x + e, z) - heightAt(x - e, z), 0);
            const glm::vec3 dz(0, heightAt(x, z + e) - heightAt(x, z - e), 2 * e);
            nova::Vertex v;
            v.position = {x, heightAt(x, z), z};
            v.normal = glm::normalize(glm::cross(dz, dx));
            v.u = static_cast<float>(i) / static_cast<float>(cells);
            v.v = static_cast<float>(j) / static_cast<float>(cells);
            m.vertices.push_back(v);
        }
    const unsigned row = cells + 1;
    for (unsigned j = 0; j < cells; ++j)
        for (unsigned i = 0; i < cells; ++i) {
            const unsigned a = j * row + i, b = a + 1, c = a + row + 1, d = a + row;
            m.indices.insert(m.indices.end(), {a, d, c, a, c, b}); // counter-clockwise seen from above
        }
    return m;
}

nova::MeshData bendableColumn(float radius, float height, float bendAt, unsigned rings, unsigned segments) {
    nova::MeshData m;
    for (unsigned r = 0; r <= rings; ++r) {
        const float y = height * static_cast<float>(r) / static_cast<float>(rings);
        // Weight of joint 1: none below the bend's blend zone, all above it.
        const float t = glm::clamp((y - (bendAt - 0.4f)) / 0.8f, 0.0f, 1.0f);
        const float w1 = t * t * (3.0f - 2.0f * t);
        for (unsigned k = 0; k <= segments; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(segments);
            nova::Vertex v;
            v.normal = {std::cos(a), 0.0f, std::sin(a)};
            v.position = {radius * v.normal.x, y, radius * v.normal.z};
            v.u = static_cast<float>(k) / static_cast<float>(segments);
            v.v = y / height;
            m.vertices.push_back(v);
            nova::SkinVertex sv;
            sv.setJoints(0, 1, 0, 0);
            sv.weights = {1.0f - w1, w1, 0.0f, 0.0f};
            m.skin.push_back(sv);
        }
    }
    const unsigned row = segments + 1;
    for (unsigned r = 0; r < rings; ++r)
        for (unsigned k = 0; k < segments; ++k) {
            const unsigned a = r * row + k, b = a + 1, c = a + row, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

} // namespace shapes
