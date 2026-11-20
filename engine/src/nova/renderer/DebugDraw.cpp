#include "nova/renderer/DebugDraw.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace nova {

u32 DebugDraw::pack(const glm::vec4& c) {
    auto b = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | (b(c.a) << 24); // matches GLSL unpackUnorm4x8
}

void DebugDraw::line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color, bool onTop) {
    auto& list = onTop ? m_onTop : m_depthTested;
    if (lineCount() >= kMaxLines) return;
    const u32 c = pack(color);
    list.push_back({a, c});
    list.push_back({b, c});
}

void DebugDraw::box(const glm::vec3& center, const glm::vec3& he, const glm::quat& rotation, const glm::vec4& color,
                    bool onTop) {
    glm::vec3 p[8];
    for (int i = 0; i < 8; ++i) {
        glm::vec3 local{(i & 1) ? he.x : -he.x, (i & 2) ? he.y : -he.y, (i & 4) ? he.z : -he.z};
        p[i] = center + rotation * local;
    }
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                          {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& e : kEdges) line(p[e[0]], p[e[1]], color, onTop);
}

void DebugDraw::box(const glm::mat4& transform, const glm::vec3& localCenter, const glm::vec3& he,
                    const glm::vec4& color, bool onTop) {
    glm::vec3 p[8];
    for (int i = 0; i < 8; ++i) {
        glm::vec3 local = localCenter + glm::vec3{(i & 1) ? he.x : -he.x, (i & 2) ? he.y : -he.y, (i & 4) ? he.z : -he.z};
        p[i] = glm::vec3(transform * glm::vec4(local, 1.0f));
    }
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                          {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& e : kEdges) line(p[e[0]], p[e[1]], color, onTop);
}

void DebugDraw::circle(const glm::vec3& center, const glm::vec3& normal, f32 radius, const glm::vec4& color,
                       u32 segments, bool onTop) {
    const glm::vec3 n = glm::normalize(normal);
    const glm::vec3 helper = std::abs(n.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(n, helper));
    const glm::vec3 v = glm::cross(n, u);
    segments = std::max(3u, segments);
    glm::vec3 prev = center + u * radius;
    for (u32 i = 1; i <= segments; ++i) {
        const f32 a = glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(segments);
        const glm::vec3 p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
        line(prev, p, color, onTop);
        prev = p;
    }
}

void DebugDraw::sphere(const glm::vec3& center, f32 radius, const glm::vec4& color, bool onTop) {
    circle(center, {1, 0, 0}, radius, color, 32, onTop);
    circle(center, {0, 1, 0}, radius, color, 32, onTop);
    circle(center, {0, 0, 1}, radius, color, 32, onTop);
}

void DebugDraw::capsule(const glm::vec3& center, const glm::quat& rotation, f32 radius, f32 halfHeight,
                        const glm::vec4& color, bool onTop) {
    const glm::vec3 up = rotation * glm::vec3(0, 1, 0);
    const glm::vec3 right = rotation * glm::vec3(1, 0, 0);
    const glm::vec3 fwd = rotation * glm::vec3(0, 0, 1);
    const glm::vec3 top = center + up * halfHeight, bottom = center - up * halfHeight;
    circle(top, up, radius, color, 24, onTop);
    circle(bottom, up, radius, color, 24, onTop);
    for (const glm::vec3& side : {right, -right, fwd, -fwd}) line(top + side * radius, bottom + side * radius, color, onTop);
    // Cap arcs (half circles) in the two vertical planes.
    for (const glm::vec3& axis : {right, fwd}) {
        for (int cap = 0; cap < 2; ++cap) {
            const glm::vec3 c = cap ? top : bottom;
            const glm::vec3 dir = cap ? up : -up;
            glm::vec3 prev = c + axis * radius;
            for (int i = 1; i <= 12; ++i) {
                const f32 a = glm::pi<f32>() * static_cast<f32>(i) / 12.0f;
                const glm::vec3 p = c + (axis * std::cos(a) + dir * std::sin(a)) * radius;
                line(prev, p, color, onTop);
                prev = p;
            }
        }
    }
}

void DebugDraw::arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, bool onTop) {
    line(from, to, color, onTop);
    const glm::vec3 d = to - from;
    const f32 len = glm::length(d);
    if (len < 1e-5f) return;
    const glm::vec3 dir = d / len;
    const glm::vec3 helper = std::abs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(dir, helper));
    const glm::vec3 v = glm::cross(dir, u);
    const f32 head = std::min(0.25f * len, 0.5f);
    for (const glm::vec3& side : {u, -u, v, -v}) line(to, to - dir * head + side * head * 0.4f, color, onTop);
}

void DebugDraw::grid(const glm::vec3& center, f32 size, u32 divisions, const glm::vec4& color) {
    divisions = std::max(1u, divisions);
    const f32 half = size * 0.5f, step = size / static_cast<f32>(divisions);
    for (u32 i = 0; i <= divisions; ++i) {
        const f32 o = -half + step * static_cast<f32>(i);
        line(center + glm::vec3(o, 0, -half), center + glm::vec3(o, 0, half), color);
        line(center + glm::vec3(-half, 0, o), center + glm::vec3(half, 0, o), color);
    }
}

void DebugDraw::axes(const glm::mat4& t, f32 length, bool onTop) {
    const glm::vec3 o(t[3]);
    line(o, o + glm::normalize(glm::vec3(t[0])) * length, {1.0f, 0.2f, 0.2f, 1.0f}, onTop);
    line(o, o + glm::normalize(glm::vec3(t[1])) * length, {0.2f, 1.0f, 0.2f, 1.0f}, onTop);
    line(o, o + glm::normalize(glm::vec3(t[2])) * length, {0.3f, 0.5f, 1.0f, 1.0f}, onTop);
}

void DebugDraw::clear() {
    m_depthTested.clear();
    m_onTop.clear();
}

} // namespace nova
