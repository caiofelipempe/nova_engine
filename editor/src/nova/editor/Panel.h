#pragma once

/// @file
/// @brief Base class of dockable editor windows.

#include "nova/core/Base.h"

#include <string>

namespace nova::rhi { class CommandList; }

namespace nova::editor {

class EditorContext;

/// @brief Where a panel is docked in the default layout (also used by "Window/Reset Layout").
enum class DockSlot {
    Left,        ///< Left column (e.g. Hierarchy).
    Right,       ///< Right column, top (e.g. Inspector).
    RightBottom, ///< Right column, bottom (e.g. Renderer settings).
    Bottom,      ///< Below the center (e.g. Console).
    Center,      ///< Main area (e.g. Viewport).
    Floating,    ///< Not docked.
};

/// @brief A dockable editor window.
///
/// Subclass it, implement onImGui() and register it with Registry::panel(). The framework opens
/// the ImGui window (`ImGui::Begin/End`) around onImGui(), lists the panel in the *Window* menu
/// and docks it in the default layout according to @ref dockSlot.
/// @code
/// class WavesPanel : public nova::editor::Panel {
/// public:
///     WavesPanel() : Panel("Waves", nova::editor::DockSlot::Right) {}
///     void onImGui(nova::editor::EditorContext& ctx) override {
///         ImGui::Text("%zu enemies", ctx.scene().view<Enemy>().size());
///     }
/// };
/// @endcode
class Panel {
public:
    /// @param title Window title; also the panel's unique id.
    /// @param slot Default dock position.
    explicit Panel(std::string title, DockSlot slot = DockSlot::Floating)
        : dockSlot(slot), m_title(std::move(title)) {}
    virtual ~Panel() = default;

    /// @brief Called once the editor (and its context) is ready, or immediately when registered later.
    virtual void onAttach(EditorContext& /*ctx*/) {}
    /// @brief Called when the editor shuts down or the panel is removed.
    virtual void onDetach(EditorContext& /*ctx*/) {}
    /// @brief Per-frame logic, before UI (also while the window is closed only if @ref updateWhenClosed).
    virtual void onUpdate(EditorContext& /*ctx*/, f32 /*dt*/) {}
    /// @brief Draws the panel's contents (inside `ImGui::Begin/End`). Only called while open.
    virtual void onImGui(EditorContext& ctx) = 0;
    /// @brief Records GPU work (e.g. rendering a scene view). Only called while open.
    virtual void onRender(EditorContext& /*ctx*/, rhi::CommandList& /*cmd*/) {}

    /// @brief Window title and unique id.
    const std::string& title() const { return m_title; }

    DockSlot dockSlot;             ///< Default dock position.
    bool open = true;              ///< Visible; toggled from the *Window* menu and the window's close button.
    bool noPadding = false;        ///< Remove window padding (viewports, canvases).
    int windowFlags = 0;           ///< Extra `ImGuiWindowFlags`.
    bool updateWhenClosed = false; ///< Keep calling onUpdate() while the window is closed.

private:
    std::string m_title;
};

} // namespace nova::editor
