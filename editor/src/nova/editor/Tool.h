#pragma once

/// @file
/// @brief Viewport tools (transform gizmos, brushes, placement...).

#include "nova/renderer/DebugDraw.h"
#include "nova/renderer/Renderer.h"

#include <glm/glm.hpp>

#include <string>

namespace nova::editor {

class EditorContext;

/// @brief What a tool knows about the viewport this frame.
struct ViewportState {
    glm::vec2 min{0.0f};        ///< Top-left of the scene image, in screen pixels.
    glm::vec2 size{0.0f};       ///< Size of the scene image in pixels.
    glm::vec2 mouse{0.0f};      ///< Mouse position relative to `min`.
    bool hovered = false;       ///< The mouse is over the scene image (and no other window covers it).
    CameraData camera;          ///< Camera the image was rendered with (Vulkan clip space).
    GlobalPosition rayOrigin;   ///< World ray under the mouse: exact origin...
    glm::vec3 rayDirection{0.0f, 0.0f, -1.0f}; ///< ...and normalized direction.
};

/// @brief An interaction mode of the scene viewport. Exactly one tool is active.
///
/// Register with Registry::tool(); the tool appears in the viewport toolbar and in
/// *View/Tools* (with its shortcut). Built-in tools: Move (W), Rotate (E), Scale (R).
/// @code
/// class PaintTool : public nova::editor::Tool {
/// public:
///     PaintTool() : Tool("Paint", "B") {}
///     bool clickSelects() const override { return false; } // left mouse paints instead
///     void onViewport(EditorContext& ctx, const ViewportState& vp) override {
///         if (vp.hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left)) { /* raycast + spawn */ }
///     }
/// };
/// @endcode
/// Wrap scene changes in EditorContext::beginEdit / beginCreation so they are undoable.
class Tool {
public:
    /// @param name Toolbar label and unique id.
    /// @param shortcut Optional KeyChord text (e.g. "W", "Ctrl+B").
    explicit Tool(std::string name, std::string shortcut = {}) : m_name(std::move(name)), m_shortcut(std::move(shortcut)) {}
    virtual ~Tool() = default;

    /// @brief Called when the tool becomes active.
    virtual void onActivate(EditorContext& /*ctx*/) {}
    /// @brief Called when another tool becomes active.
    virtual void onDeactivate(EditorContext& /*ctx*/) {}
    /// @brief Every frame while active, inside the viewport window after the scene image:
    ///        handle the mouse, draw ImGui overlays or gizmos (e.g. ImGuizmo).
    virtual void onViewport(EditorContext& /*ctx*/, const ViewportState& /*viewport*/) {}
    /// @brief Every frame while active: queue world-space preview lines.
    virtual void onDebugDraw(EditorContext& /*ctx*/, DebugDraw& /*draw*/) {}
    /// @brief If true, left-clicking the viewport selects the entity under the cursor
    ///        (Ctrl toggles, Shift adds). Return false when the tool uses the left button itself.
    virtual bool clickSelects() const { return true; }
    /// @brief True while the tool is in the middle of an interaction (e.g. dragging a gizmo):
    ///        automatic undo tracking waits until it ends.
    virtual bool isInteracting() const { return false; }

    /// @brief Toolbar label and unique id.
    const std::string& name() const { return m_name; }
    /// @brief Shortcut text ("" if none).
    const std::string& shortcut() const { return m_shortcut; }

private:
    std::string m_name;
    std::string m_shortcut;
};

} // namespace nova::editor
