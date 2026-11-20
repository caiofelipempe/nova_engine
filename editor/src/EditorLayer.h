#pragma once

#include "EditorCamera.h"

#include <nova/Nova.h>

class EditorLayer : public nova::Layer {
public:
    EditorLayer() : nova::Layer("Editor") {}

    void onAttach() override;
    void onDetach() override;
    void onUpdate(float dt) override;
    void onImGui() override;
    void onRender(nova::rhi::CommandList& cmd) override;

private:
    void buildDemoScene();
    void setupDockLayout(unsigned int dockId);

    void drawMenuBar();
    void drawViewport();
    void drawHierarchy();
    void drawInspector();
    void drawRendererPanel();
    void drawConsole();
    void drawImportPopup();

    nova::Entity createPrimitive(const char* name, nova::MeshHandle mesh);

    nova::Application& app() { return nova::Application::get(); }
    nova::Renderer& renderer() { return app().renderer(); }

    nova::Scene m_scene{"Demo"};
    nova::RenderScene m_renderScene;
    nova::Scope<nova::RenderView> m_view;
    EditorCamera m_camera;
    nova::Entity m_selected;

    glm::vec2 m_viewportSize{1280.0f, 720.0f};
    bool m_viewportHovered = false;
    bool m_cameraActive = false;
    bool m_playing = true;
    bool m_useSceneCamera = false;
    int m_gizmoOperation = 0; // 0 translate, 1 rotate, 2 scale
    bool m_gizmoLocal = false;
    bool m_openImport = false;
    char m_importPath[512] = "";
    bool m_consoleAutoScroll = true;
};
