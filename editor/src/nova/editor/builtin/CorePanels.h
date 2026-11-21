#pragma once

/// @file
/// @brief The editor's built-in panels (registered by CorePanelsPlugin).
///
/// Games can tweak them through Registry::findPanel<T>() or replace them by registering a panel
/// with the same title (or removing them with Registry::removePanel).

#include "nova/editor/EditorContext.h"
#include "nova/editor/Panel.h"
#include "nova/editor/Registry.h"
#include "nova/renderer/Renderer.h"

namespace nova::editor::builtin {

/// @brief Scene view: renders from the editor camera, hosts the active Tool, click-to-select.
class ViewportPanel final : public Panel {
public:
    ViewportPanel();
    void onAttach(EditorContext& ctx) override;
    void onDetach(EditorContext& ctx) override;
    void onUpdate(EditorContext& ctx, f32 dt) override;
    void onImGui(EditorContext& ctx) override;
    void onRender(EditorContext& ctx, rhi::CommandList& cmd) override;

    bool useSceneCamera = false; ///< Render from the scene's primary camera instead of the editor camera.
    /// @brief True while the mouse is over the rendered image.
    bool hovered() const { return m_hovered; }
    /// @brief Viewport state of the last frame (rect, camera, mouse ray).
    const ViewportState& state() const { return m_state; }

private:
    Scope<RenderView> m_view;
    RenderScene m_renderScene;
    ViewportState m_state;
    glm::vec2 m_size{1280.0f, 720.0f};
    bool m_hovered = false;
};

/// @brief Entity list: click to select, Ctrl+click toggles, Shift+click selects a range.
///        Context menu: focus, delete, and "Create" entries from every `Entity/...` command.
class HierarchyPanel final : public Panel {
public:
    HierarchyPanel();
    void onImGui(EditorContext& ctx) override;

private:
    u64 m_anchor = 0; // UUID of the last plain/ctrl click, for Shift ranges
};

/// @brief Edits the primary selected entity through the Registry's component editors.
class InspectorPanel final : public Panel {
public:
    InspectorPanel();
    void onImGui(EditorContext& ctx) override;
};

/// @brief GPU info, render settings, statistics.
class RendererPanel final : public Panel {
public:
    RendererPanel();
    void onImGui(EditorContext& ctx) override;
};

/// @brief World settings of the scene: step size (unit), per-axis size and boundary.
///
/// Edits are pending until Apply, which goes through EditorContext::setWorldSpace (one undo step).
class WorldPanel final : public Panel {
public:
    WorldPanel();
    void onImGui(EditorContext& ctx) override;

private:
    WorldSpaceDesc m_pending;       // what the fields show
    WorldSpaceDesc m_loaded;        // scene settings m_pending was loaded from
    bool m_hasLoaded = false;
    LengthUnit m_unitShown = LengthUnit::Nanometer;
    LengthUnit m_sizeShown[3] = {LengthUnit::Kilometer, LengthUnit::Kilometer, LengthUnit::Kilometer};
    f64 m_maxSize = 10.0;           // "every dimension at most" helper
    LengthUnit m_maxSizeUnit = LengthUnit::Kilometer;
};

/// @brief Recent log messages.
class ConsolePanel final : public Panel {
public:
    ConsolePanel();
    void onImGui(EditorContext& ctx) override;
    bool autoScroll = true; ///< Keep the newest message visible.
};

/// @brief Undo history: every step, the current position; click a step to jump there.
class HistoryPanel final : public Panel {
public:
    HistoryPanel();
    void onImGui(EditorContext& ctx) override;
};

/// @brief Registers the built-in panels.
class CorePanelsPlugin final : public Plugin {
public:
    void onRegister(Registry& registry) override;
};

} // namespace nova::editor::builtin
