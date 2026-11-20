#pragma once

/// @file
/// @brief Immediate-mode debug lines (shapes, arrows, grids), drawn by the Renderer.

#include "nova/core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace nova {

/// @brief Collects debug lines for the current frame.
///
/// Everything added is drawn by every Renderer::render / renderToBackbuffer call of the frame and
/// then cleared by Renderer::beginFrame, so call it every frame you want the shape visible:
/// @code
/// auto& dd = app().renderer().debugDraw();
/// dd.arrow(player.position, player.position + velocity, {1, 1, 0, 1});
/// if (auto hit = scene().raycast(origin, dir)) dd.sphere(hit->point, 0.1f, {1, 0, 0, 1});
/// @endcode
/// Lines are depth-tested against the scene unless `onTop` is set. Colors are display (sRGB) RGBA.
class DebugDraw {
public:
    /// @brief One line vertex as uploaded to the GPU (16 bytes).
    struct Vertex {
        glm::vec3 position; ///< World position.
        u32 color;          ///< RGBA8, packed.
    };

    /// @brief A line segment.
    void line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color, bool onTop = false);
    /// @brief Oriented box outline (12 edges).
    void box(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, const glm::vec4& color,
             bool onTop = false);
    /// @brief Box outline transformed by `transform` (unit-cube corners at ±0.5 scaled by `localHalfExtents`).
    void box(const glm::mat4& transform, const glm::vec3& localCenter, const glm::vec3& localHalfExtents,
             const glm::vec4& color, bool onTop = false);
    /// @brief Circle in the plane perpendicular to `normal`.
    void circle(const glm::vec3& center, const glm::vec3& normal, f32 radius, const glm::vec4& color,
                u32 segments = 32, bool onTop = false);
    /// @brief Sphere as three orthogonal circles.
    void sphere(const glm::vec3& center, f32 radius, const glm::vec4& color, bool onTop = false);
    /// @brief Capsule along the local Y axis of `rotation` (two caps and four side lines).
    void capsule(const glm::vec3& center, const glm::quat& rotation, f32 radius, f32 halfHeight,
                 const glm::vec4& color, bool onTop = false);
    /// @brief Line with an arrow head at `to`.
    void arrow(const glm::vec3& from, const glm::vec3& to, const glm::vec4& color, bool onTop = false);
    /// @brief Square grid on the XZ plane centered at `center`.
    void grid(const glm::vec3& center, f32 size, u32 divisions, const glm::vec4& color);
    /// @brief Three axis lines (X red, Y green, Z blue) of a transform.
    void axes(const glm::mat4& transform, f32 length, bool onTop = false);

    /// @brief Removes every line (done by Renderer::beginFrame).
    void clear();
    /// @brief Line vertices (pairs), depth-tested or on top.
    const std::vector<Vertex>& vertices(bool onTop) const { return onTop ? m_onTop : m_depthTested; }
    /// @brief Number of lines queued.
    size_t lineCount() const { return (m_depthTested.size() + m_onTop.size()) / 2; }

    /// @brief Packs a 0..1 RGBA color.
    static u32 pack(const glm::vec4& color);

    static constexpr size_t kMaxLines = 200000; ///< Lines beyond this per frame are dropped.

private:
    std::vector<Vertex> m_depthTested;
    std::vector<Vertex> m_onTop;
};

} // namespace nova
