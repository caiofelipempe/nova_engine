#pragma once

#include "nova/core/Application.h"
#include "nova/scene/Scene.h"

namespace nova {

// Ready-made layer for games: owns a Scene, updates it and renders it to the screen with
// the scene's primary camera. Subclass it and build your world in onStart():
//
//   struct MyGame : nova::GameLayer {
//       void onStart() override {
//           auto cam = scene().createEntity("Camera");
//           cam.add<nova::CameraComponent>();
//           cam.transform().position = {0, 2, 6};
//
//           auto box = scene().createEntity("Box");
//           box.add<nova::MeshComponent>(renderer().cube());
//       }
//   };
class GameLayer : public Layer {
public:
    explicit GameLayer(std::string name = "Game") : Layer(std::move(name)) {}

    virtual void onStart() {}
    virtual void onTick(f32 /*dt*/) {}
    virtual void onStop() {}

    Scene& scene() { return m_scene; }
    Renderer& renderer() { return Application::get().renderer(); }
    Application& app() { return Application::get(); }

    void onAttach() override {
        onStart();
        m_scene.startSimulation(); // physics runs for every entity with a RigidBodyComponent
    }
    void onDetach() override {
        onStop();
        m_scene.clear();
        m_scene.stopSimulation();
    }
    void onUpdate(f32 dt) override {
        onTick(dt);
        m_scene.update(dt);
    }
    void onRender(rhi::CommandList& cmd) override {
        Extent2D size = app().device().backbufferExtent();
        f32 aspect = size.height ? static_cast<f32>(size.width) / static_cast<f32>(size.height) : 1.0f;
        if (m_scene.buildRenderScene(m_renderScene, aspect)) renderer().renderToBackbuffer(cmd, m_renderScene);
    }

private:
    Scene m_scene;
    RenderScene m_renderScene;
};

} // namespace nova
