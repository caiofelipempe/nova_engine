#pragma once

/// @file
/// @brief Ready-made layer that owns, simulates and renders a Scene.

#include "nova/core/Application.h"
#include "nova/scene/Scene.h"

namespace nova {

/// @brief Ready-made layer for games.
///
/// Owns a Scene, starts its physics simulation, updates it every frame and renders it to the
/// screen with the scene's primary camera. Subclass it and build the world in onStart():
/// @code
/// struct MyGame : nova::GameLayer {
///     void onStart() override {
///         auto cam = scene().createEntity("Camera");
///         cam.add<nova::CameraComponent>();
///         cam.transform().position = {0, 2, 6};
///
///         auto box = scene().createEntity("Box");
///         box.add<nova::MeshComponent>(renderer().cube());
///         box.add<nova::RigidBodyComponent>();
///     }
/// };
/// int main() { return nova::run<MyGame>({.name = "My Game"}); }
/// @endcode
/// Layer::onImGui can still be overridden for debug UI.
class GameLayer : public Layer {
public:
    /// @param name Debug name of the layer.
    explicit GameLayer(std::string name = "Game") : Layer(std::move(name)) {}

    /// @brief Build the initial world here. Called once, before the simulation starts.
    virtual void onStart() {}
    /// @brief Per-frame game logic, called before the scene (scripts, physics) updates.
    /// @param dt Seconds since the previous frame.
    virtual void onTick(f32 /*dt*/) {}
    /// @brief Called on shutdown, before the scene is cleared.
    virtual void onStop() {}

    /// @brief The game's scene.
    Scene& scene() { return m_scene; }
    /// @brief The engine renderer (meshes, settings, statistics).
    Renderer& renderer() { return Application::get().renderer(); }
    /// @brief The running application (window, device, quit()).
    Application& app() { return Application::get(); }

    /// @brief Calls onStart(), then starts the physics simulation.
    void onAttach() override {
        onStart();
        m_scene.startSimulation(); // physics runs for every entity with a RigidBodyComponent
    }
    /// @brief Calls onStop(), clears the scene and stops the simulation.
    void onDetach() override {
        onStop();
        m_scene.clear();
        m_scene.stopSimulation();
    }
    /// @brief Calls onTick(), then updates the scene.
    void onUpdate(f32 dt) override {
        onTick(dt);
        m_scene.update(app().frameDuration()); // exact: the scene clock never drifts
    }
    /// @brief Renders the scene to the swapchain image from its primary camera (nothing without a camera).
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
