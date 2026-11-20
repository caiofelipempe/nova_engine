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

/// @brief Gizmo mode of the viewport.
enum class GizmoMode { Translate, Rotate, Scale };

/// @brief Scene view: renders the scene from the editor camera, fly controls and transform gizmo.
class ViewportPanel final : public Panel {
public:
    ViewportPanel();
    void onAttach(EditorContext& ctx) override;
    void onDetach(EditorContext& ctx) override;
    void onUpdate(EditorContext& ctx, f32 dt) override;
    void onImGui(EditorContext& ctx) override;
    void onRender(EditorContext& ctx, rhi::CommandList& cmd) override;

    GizmoMode gizmoMode = GizmoMode::Translate; ///< Current gizmo operation.
    bool gizmoLocal = false;                    ///< Gizmo in local (true) or world space.
    bool useSceneCamera = false;                ///< Render from the scene's primary camera instead of the editor camera.
    /// @brief True while the mouse is over the rendered image.
    bool hovered() const { return m_hovered; }

private:
    Scope<RenderView> m_view;
    RenderScene m_renderScene;
    glm::vec2 m_size{1280.0f, 720.0f};
    bool m_hovered = false;
};

/// @brief Entity list: selection, focus, delete, and "Create" entries from every `Entity/...` command.
class HierarchyPanel final : public Panel {
public:
    HierarchyPanel();
    void onImGui(EditorContext& ctx) override;
};

/// @brief Edits the selected entity through the Registry's component editors.
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

/// @brief Recent log messages.
class ConsolePanel final : public Panel {
public:
    ConsolePanel();
    void onImGui(EditorContext& ctx) override;
    bool autoScroll = true; ///< Keep the newest message visible.
};

/// @brief Registers the built-in panels and the viewport's gizmo commands (W / E / R).
class CorePanelsPlugin final : public Plugin {
public:
    void onRegister(Registry& registry) override;
};

} // namespace nova::editor::builtin
