#pragma once

/// @file
/// @brief Internal: the Layer that hosts the editor framework.

#include "nova/editor/EditorApp.h"

#include <string>
#include <unordered_set>

namespace nova::editor::detail {

/// @brief Hosts the editor inside the Application loop (internal; games use EditorApp / run()).
///
/// Owns the scene, the EditorContext and the Registry; builds the main menu from commands,
/// dispatches shortcuts, draws panels inside a dockspace and runs the command palette.
class EditorLayer final : public Layer {
public:
    EditorLayer(const EditorDesc& desc, Scope<EditorApp> app);
    ~EditorLayer() override;

    void onAttach() override;
    void onDetach() override;
    void onUpdate(f32 dt) override;
    void onImGui() override;
    void onRender(rhi::CommandList& cmd) override;

private:
    void registerFrameworkCommands();
    void dispatchShortcuts();
    void drawMainMenu();
    void drawPanels();
    void dockNewPanel(Panel& panel);
    void buildDefaultLayout(unsigned int dockId);
    void drawCommandPalette();

    EditorDesc m_desc;
    Scope<EditorApp> m_app;
    Scene m_scene{"Untitled"};
    EditorContext m_context{m_scene};
    Registry m_registry{m_context};

    bool m_resetLayout = false;
    bool m_openPalette = false;
    char m_paletteFilter[128] = "";
    int m_paletteSelection = 0;
    std::unordered_set<const Panel*> m_dockChecked;
};

} // namespace nova::editor::detail
