#include "nova/renderer/DebugDraw.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace nova {

namespace {
constexpr int kBoxEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                  {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
}

u32 DebugDraw::pack(const glm::vec4& c) {
    auto b = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return b(c.r) | (b(c.g) << 8) | (b(c.b) << 16) | (b(c.a) << 24); // matches GLSL unpackUnorm4x8
}

void DebugDraw::lineLocal(const glm::vec3& a, const glm::vec3& b, u32 color, bool onTop) {
    auto& list = onTop ? m_onTop : m_depthTested;
    if (lineCount() >= kMaxLines) return;
    list.push_back({a, color});
    list.push_back({b, color});
}

void DebugDraw::line(const GlobalPosition& a, const GlobalPosition& b, const glm::vec4& color, bool onTop) {
    lineLocal(local(a), local(b), pack(color), onTop);
}

void DebugDraw::box(const GlobalPosition& center, const glm::vec3& he, const glm::quat& rotation, const glm::vec4& color,
                    bool onTop) {
    const glm::vec3 c = local(center);
    glm::vec3 p[8];
    for (int i = 0; i < 8; ++i) {
        glm::vec3 corner{(i & 1) ? he.x : -he.x, (i & 2) ? he.y : -he.y, (i & 4) ? he.z : -he.z};
        p[i] = c + rotation * corner;
    }
    const u32 packed = pack(color);
    for (auto& e : kBoxEdges) lineLocal(p[e[0]], p[e[1]], packed, onTop);
}

void DebugDraw::box(const GlobalPosition& origin, const glm::mat4& transform, const glm::vec3& localCenter,
                    const glm::vec3& he, const glm::vec4& color, bool onTop) {
    const glm::vec3 shift = local(origin);
    glm::vec3 p[8];
    for (int i = 0; i < 8; ++i) {
        glm::vec3 corner = localCenter + glm::vec3{(i & 1) ? he.x : -he.x, (i & 2) ? he.y : -he.y, (i & 4) ? he.z : -he.z};
        p[i] = shift + glm::vec3(transform * glm::vec4(corner, 1.0f));
    }
    const u32 packed = pack(color);
    for (auto& e : kBoxEdges) lineLocal(p[e[0]], p[e[1]], packed, onTop);
}

void DebugDraw::circleLocal(const glm::vec3& center, const glm::vec3& normal, f32 radius, u32 color, u32 segments,
                            bool onTop) {
    const glm::vec3 n = glm::normalize(normal);
    const glm::vec3 helper = std::abs(n.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(n, helper));
    const glm::vec3 v = glm::cross(n, u);
    segments = std::max(3u, segments);
    glm::vec3 prev = center + u * radius;
    for (u32 i = 1; i <= segments; ++i) {
        const f32 a = glm::two_pi<f32>() * static_cast<f32>(i) / static_cast<f32>(segments);
        const glm::vec3 p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
        lineLocal(prev, p, color, onTop);
        prev = p;
    }
}

void DebugDraw::circle(const GlobalPosition& center, const glm::vec3& normal, f32 radius, const glm::vec4& color,
                       u32 segments, bool onTop) {
    circleLocal(local(center), normal, radius, pack(color), segments, onTop);
}

void DebugDraw::sphere(const GlobalPosition& center, f32 radius, const glm::vec4& color, bool onTop) {
    const glm::vec3 c = local(center);
    const u32 packed = pack(color);
    circleLocal(c, {1, 0, 0}, radius, packed, 32, onTop);
    circleLocal(c, {0, 1, 0}, radius, packed, 32, onTop);
    circleLocal(c, {0, 0, 1}, radius, packed, 32, onTop);
}

void DebugDraw::capsule(const GlobalPosition& center, const glm::quat& rotation, f32 radius, f32 halfHeight,
                        const glm::vec4& color, bool onTop) {
    const glm::vec3 c0 = local(center);
    const u32 packed = pack(color);
    const glm::vec3 up = rotation * glm::vec3(0, 1, 0);
    const glm::vec3 right = rotation * glm::vec3(1, 0, 0);
    const glm::vec3 fwd = rotation * glm::vec3(0, 0, 1);
    const glm::vec3 top = c0 + up * halfHeight, bottom = c0 - up * halfHeight;
    circleLocal(top, up, radius, packed, 24, onTop);
    circleLocal(bottom, up, radius, packed, 24, onTop);
    for (const glm::vec3& side : {right, -right, fwd, -fwd})
        lineLocal(top + side * radius, bottom + side * radius, packed, onTop);
    // Cap arcs (half circles) in the two vertical planes.
    for (const glm::vec3& axis : {right, fwd}) {
        for (int cap = 0; cap < 2; ++cap) {
            const glm::vec3 c = cap ? top : bottom;
            const glm::vec3 dir = cap ? up : -up;
            glm::vec3 prev = c + axis * radius;
            for (int i = 1; i <= 12; ++i) {
                const f32 a = glm::pi<f32>() * static_cast<f32>(i) / 12.0f;
                const glm::vec3 p = c + (axis * std::cos(a) + dir * std::sin(a)) * radius;
                lineLocal(prev, p, packed, onTop);
                prev = p;
            }
        }
    }
}

void DebugDraw::arrow(const GlobalPosition& from, const GlobalPosition& to, const glm::vec4& color, bool onTop) {
    const glm::vec3 a = local(from), b = local(to);
    const u32 packed = pack(color);
    lineLocal(a, b, packed, onTop);
    const glm::vec3 d = glm::vec3((to - from).toMeters()); // exact direction even far away
    const f32 len = glm::length(d);
    if (len < 1e-5f) return;
    const glm::vec3 dir = d / len;
    const glm::vec3 helper = std::abs(dir.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(dir, helper));
    const glm::vec3 v = glm::cross(dir, u);
    const f32 head = std::min(0.25f * len, 0.5f);
    for (const glm::vec3& side : {u, -u, v, -v}) lineLocal(b, b - dir * head + side * head * 0.4f, packed, onTop);
}

void DebugDraw::grid(const GlobalPosition& center, f32 size, u32 divisions, const glm::vec4& color) {
    const glm::vec3 c = local(center);
    const u32 packed = pack(color);
    divisions = std::max(1u, divisions);
    const f32 half = size * 0.5f, step = size / static_cast<f32>(divisions);
    for (u32 i = 0; i <= divisions; ++i) {
        const f32 o = -half + step * static_cast<f32>(i);
        lineLocal(c + glm::vec3(o, 0, -half), c + glm::vec3(o, 0, half), packed, false);
        lineLocal(c + glm::vec3(-half, 0, o), c + glm::vec3(half, 0, o), packed, false);
    }
}

void DebugDraw::axes(const GlobalPosition& origin, const glm::mat4& t, f32 length, bool onTop) {
    const glm::vec3 o = local(origin) + glm::vec3(t[3]);
    lineLocal(o, o + glm::normalize(glm::vec3(t[0])) * length, pack({1.0f, 0.2f, 0.2f, 1.0f}), onTop);
    lineLocal(o, o + glm::normalize(glm::vec3(t[1])) * length, pack({0.2f, 1.0f, 0.2f, 1.0f}), onTop);
    lineLocal(o, o + glm::normalize(glm::vec3(t[2])) * length, pack({0.3f, 0.5f, 1.0f, 1.0f}), onTop);
}

void DebugDraw::clear() {
    m_depthTested.clear();
    m_onTop.clear();
}

} // namespace nova
