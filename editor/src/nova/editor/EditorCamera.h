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
    GlobalPosition position{0.0, 4.0, 12.0}; ///< Exact world position (the viewport renders relative to it).
    f32 yaw = 0.0f;                        ///< Radians; 0 looks down −Z.
    f32 pitch = -0.3f;                     ///< Radians; positive looks up.
    f32 fovY = glm::radians(60.0f);        ///< Vertical field of view in radians.
    bool orthographic = false;             ///< Orthographic view (no perspective); the mouse wheel zooms it.
    f32 orthoSize = 20.0f;                 ///< Orthographic: height of the view (m).
    f32 speed = 6.0f;                      ///< Movement speed in units per second.
    /// @brief Radians turned per mouse count (raw input while looking). 0.0008 ≈ a common game
    ///        default; higher is faster but each count moves the image further (steppier).
    f32 lookSensitivity = 0.0008f;

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
    void focus(const GlobalPosition& target, f32 distance = 6.0f) { position = target - forward() * distance; }

    /// @brief Camera data for rendering (camera-relative: the camera is the frame's origin).
    /// @param aspect Width / height of the viewport.
    CameraData data(f32 aspect) const;

private:
    bool m_active = false;
};

} // namespace nova::editor
