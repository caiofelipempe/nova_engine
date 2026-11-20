#pragma once

/// @file
/// @brief Free-flying scene camera used by the editor viewport.

#include "nova/renderer/Renderer.h"

#include <glm/glm.hpp>

namespace nova::editor {

/// @brief Unreal/Unity-style fly camera: hold the right mouse button to look, WASD/QE to move,
///        Shift to go faster, mouse wheel to change speed.
class EditorCamera {
public:
    glm::vec3 position{0.0f, 4.0f, 12.0f}; ///< World position.
    f32 yaw = 0.0f;                        ///< Radians; 0 looks down −Z.
    f32 pitch = -0.3f;                     ///< Radians; positive looks up.
    f32 fovY = glm::radians(60.0f);        ///< Vertical field of view in radians.
    f32 speed = 6.0f;                      ///< Movement speed in units per second.

    /// @brief World-space view direction.
    glm::vec3 forward() const;
    /// @brief World-space right direction (horizontal).
    glm::vec3 right() const;

    /// @brief Applies mouse/keyboard input.
    /// @param dt Frame time in seconds.
    /// @param hovered Whether the viewport is hovered (flying can only start from it).
    /// @return True while the camera is being controlled (the caller should capture the cursor).
    bool update(f32 dt, bool hovered);

    /// @brief Moves the camera so `target` is in front of it at `distance`.
    void focus(const glm::vec3& target, f32 distance = 6.0f) { position = target - forward() * distance; }

    /// @brief Camera data for rendering.
    /// @param aspect Width / height of the viewport.
    CameraData data(f32 aspect) const;

private:
    bool m_active = false;
};

} // namespace nova::editor
