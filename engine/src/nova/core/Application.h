#pragma once

/// @file
/// @brief Application object, its configuration and the `nova::run` entry point.

#include "nova/core/Layer.h"
#include "nova/math/Time.h"
#include "nova/platform/Window.h"
#include "nova/rhi/Device.h"

#include <string>
#include <vector>

namespace nova {

class Renderer;
class Audio;

/// @brief Startup configuration of an Application.
///
/// Designed for C++20 designated initializers:
/// @code
/// nova::Application app({.name = "My Game", .width = 1280, .height = 720, .vsync = false});
/// @endcode
struct ApplicationDesc {
    std::string name = "Nova Application"; ///< Window title and log name.
    u32 width = 1600;                       ///< Initial window width in screen coordinates.
    u32 height = 900;                       ///< Initial window height in screen coordinates.
    bool maximized = false;                 ///< Start with the window maximized.
    bool vsync = true;                      ///< Present synchronized to the display (FIFO). Changeable at runtime.
#ifdef NOVA_DEBUG
    bool validation = true; ///< Enable the graphics API validation layers (default: on in Debug builds).
#else
    bool validation = false; ///< Enable the graphics API validation layers (default: on in Debug builds).
#endif
    bool rayTracing = true; ///< Request ray tracing extensions when the GPU supports them.
    bool imgui = true;      ///< Create a Dear ImGui context (with docking) and its renderer.
    platform::WindowBackend windowBackend = platform::WindowBackend::Glfw; ///< Window/input backend.
    rhi::Backend rhiBackend = rhi::Backend::Vulkan;                        ///< Graphics API backend.
};

/// @brief Owns the engine subsystems and drives the main loop.
///
/// Creates, in order: logging, the window, the graphics device, the Dear ImGui context (optional)
/// and the Renderer. Exactly one instance may exist; it is reachable through Application::get().
///
/// Each frame runs, for every Layer in push order: Layer::onUpdate, Layer::onImGui and
/// Layer::onRender, then draws the UI on top of the swapchain image and presents it.
///
/// @code
/// int main() {
///     nova::Application app({.name = "My Game"});
///     app.pushLayer<MyGame>();
///     app.run();
/// }
/// @endcode
/// @see nova::run for a one-line equivalent.
class Application : NonCopyable {
public:
    /// @brief Initializes every subsystem.
    /// @param desc Startup configuration.
    /// @note Aborts with a log message if no compatible GPU/backend is available.
    explicit Application(const ApplicationDesc& desc = {});

    /// @brief Detaches all layers (in reverse order) and shuts the subsystems down.
    virtual ~Application();

    /// @brief Creates a layer, attaches it (calls Layer::onAttach) and appends it to the frame loop.
    /// @tparam T Layer type, derived from Layer.
    /// @param args Arguments forwarded to `T`'s constructor.
    /// @return Reference to the new layer, owned by the application.
    template <typename T, typename... Args>
    T& pushLayer(Args&&... args) {
        auto layer = makeScope<T>(std::forward<Args>(args)...);
        T& ref = *layer;
        attachLayer(std::move(layer));
        return ref;
    }

    /// @brief Runs the main loop until the window is closed or quit() is called.
    void run();

    /// @brief Requests the main loop to stop after the current frame.
    void quit();

    /// @brief The single running application.
    static Application& get() { return *s_instance; }

    /// @brief The OS window (also the input source).
    platform::Window& window() { return *m_window; }
    /// @brief The graphics device (resources, frame, swapchain).
    rhi::Device& device() { return *m_device; }
    /// @brief The engine renderer (meshes, render views, settings).
    Renderer& renderer() { return *m_renderer; }
    /// @brief The sound output (silent without a device); every scene plays through it by default.
    Audio& audio() { return *m_audio; }
    /// @brief The Dear ImGui renderer, or `nullptr` when ApplicationDesc::imgui is false.
    rhi::ImGuiRenderer* imgui() { return m_imguiRenderer.get(); }
    /// @brief The configuration the application was created with.
    const ApplicationDesc& desc() const { return m_desc; }

    /// @brief Duration of the last frame in seconds (clamped to 0.25 s to survive hitches).
    f32 deltaTime() const { return m_deltaTime; }
    /// @brief Exact duration of the last frame, from the steady clock (clamped to 0.25 s, like deltaTime()).
    Duration frameDuration() const { return m_frameDuration; }
    /// @brief Frames per second, averaged over the last 30 frames.
    f32 fps() const { return m_fps; }

private:
    void attachLayer(Scope<Layer> layer);
    void initImGui();
    void shutdownImGui();

    static NOVA_API Application* s_instance;

    ApplicationDesc m_desc;
    Scope<platform::Window> m_window;
    Scope<rhi::Device> m_device;
    Scope<rhi::ImGuiRenderer> m_imguiRenderer;
    Scope<Renderer> m_renderer;
    Scope<Audio> m_audio;
    std::vector<Scope<Layer>> m_layers;

    bool m_running = true;
    f32 m_deltaTime = 0.0f;
    Duration m_frameDuration;
    f32 m_fps = 0.0f;
};

/// @brief One-line entry point: creates an Application, pushes one layer and runs it.
///
/// @code
/// int main() { return nova::run<MyGame>({.name = "My Game"}); }
/// @endcode
/// @tparam LayerT Layer to run (typically derived from GameLayer).
/// @param desc Application configuration.
/// @param args Arguments forwarded to `LayerT`'s constructor.
/// @return Process exit code (0).
template <typename LayerT, typename... Args>
int run(const ApplicationDesc& desc = {}, Args&&... args) {
    Application app(desc);
    app.pushLayer<LayerT>(std::forward<Args>(args)...);
    app.run();
    return 0;
}

} // namespace nova
