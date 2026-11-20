#pragma once

#include "nova/core/Layer.h"
#include "nova/platform/Window.h"
#include "nova/rhi/Device.h"

#include <string>
#include <vector>

namespace nova {

class Renderer;

struct ApplicationDesc {
    std::string name = "Nova Application";
    u32 width = 1600;
    u32 height = 900;
    bool maximized = false;
    bool vsync = true;
#ifdef NOVA_DEBUG
    bool validation = true; // Vulkan validation layers
#else
    bool validation = false;
#endif
    bool rayTracing = true;    // enable RT extensions when the GPU supports them
    bool imgui = true;         // Dear ImGui (docking) context + renderer
    platform::WindowBackend windowBackend = platform::WindowBackend::Glfw;
    rhi::Backend rhiBackend = rhi::Backend::Vulkan;
};

// Owns the engine subsystems and drives the main loop.
//
//   int main() {
//       nova::Application app({.name = "My Game"});
//       app.pushLayer<MyGame>();
//       app.run();
//   }
class Application : NonCopyable {
public:
    explicit Application(const ApplicationDesc& desc = {});
    virtual ~Application();

    template <typename T, typename... Args>
    T& pushLayer(Args&&... args) {
        auto layer = makeScope<T>(std::forward<Args>(args)...);
        T& ref = *layer;
        attachLayer(std::move(layer));
        return ref;
    }

    void run();
    void quit();

    static Application& get() { return *s_instance; }

    platform::Window& window() { return *m_window; }
    rhi::Device& device() { return *m_device; }
    Renderer& renderer() { return *m_renderer; }
    rhi::ImGuiRenderer* imgui() { return m_imguiRenderer.get(); }
    const ApplicationDesc& desc() const { return m_desc; }

    f32 deltaTime() const { return m_deltaTime; }
    f32 fps() const { return m_fps; }

private:
    void attachLayer(Scope<Layer> layer);
    void initImGui();
    void shutdownImGui();

    static Application* s_instance;

    ApplicationDesc m_desc;
    Scope<platform::Window> m_window;
    Scope<rhi::Device> m_device;
    Scope<rhi::ImGuiRenderer> m_imguiRenderer;
    Scope<Renderer> m_renderer;
    std::vector<Scope<Layer>> m_layers;

    bool m_running = true;
    f32 m_deltaTime = 0.0f;
    f32 m_fps = 0.0f;
};

// One-liner entry point: `int main() { return nova::run<MyGame>({.name = "My Game"}); }`
template <typename LayerT, typename... Args>
int run(const ApplicationDesc& desc = {}, Args&&... args) {
    Application app(desc);
    app.pushLayer<LayerT>(std::forward<Args>(args)...);
    app.run();
    return 0;
}

} // namespace nova
