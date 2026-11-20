#include "nova/core/Application.h"

#include "nova/core/Log.h"
#include "nova/platform/Input.h"
#include "nova/renderer/Renderer.h"
#include "nova/ui/ImGuiTheme.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>

namespace nova {

Application* Application::s_instance = nullptr;

Application::Application(const ApplicationDesc& desc) : m_desc(desc) {
    NOVA_VERIFY(s_instance == nullptr, "Only one Application may exist");
    s_instance = this;
    Log::init();
    NOVA_CORE_INFO("Nova Engine starting: '{}'", desc.name);

    m_window = platform::Window::create(desc.windowBackend,
                                        {desc.name, desc.width, desc.height, true, desc.maximized});
    Input::setWindow(m_window.get());

    rhi::DeviceDesc dd;
    dd.enableValidation = desc.validation;
    dd.vsync = desc.vsync;
    dd.enableRayTracing = desc.rayTracing;
    m_device = rhi::Device::create(desc.rhiBackend, dd, *m_window);

    if (desc.imgui) initImGui();
    m_renderer = makeScope<Renderer>(*m_device);
}

Application::~Application() {
    m_device->waitIdle();
    for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) (*it)->onDetach();
    m_layers.clear();
    m_renderer.reset();
    shutdownImGui();
    m_device.reset();
    Input::setWindow(nullptr);
    m_window.reset();
    NOVA_CORE_INFO("Nova Engine shut down");
    Log::shutdown();
    s_instance = nullptr;
}

void Application::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    ui::applyTheme();
    ui::loadFonts();
    m_window->imguiInit();
    m_imguiRenderer = m_device->createImGuiRenderer();
}

void Application::shutdownImGui() {
    if (!ImGui::GetCurrentContext()) return;
    m_imguiRenderer.reset();
    m_window->imguiShutdown();
    ImGui::DestroyContext();
}

void Application::attachLayer(Scope<Layer> layer) {
    m_layers.push_back(std::move(layer));
    m_layers.back()->onAttach();
}

void Application::quit() { m_running = false; }

void Application::run() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    f32 fpsAccum = 0.0f;
    u32 fpsFrames = 0;

    while (m_running && !m_window->shouldClose()) {
        m_window->pollEvents();
        if (m_window->isMinimized()) {
            m_window->waitEvents();
            last = clock::now();
            continue;
        }

        auto now = clock::now();
        m_deltaTime = std::min(std::chrono::duration<f32>(now - last).count(), 0.25f);
        last = now;
        fpsAccum += m_deltaTime;
        if (++fpsFrames >= 30) {
            m_fps = fpsFrames / fpsAccum;
            fpsAccum = 0.0f;
            fpsFrames = 0;
        }

        Input::update();
        for (auto& layer : m_layers) layer->onUpdate(m_deltaTime);

        if (!m_device->beginFrame()) continue;
        m_renderer->beginFrame();

        if (m_imguiRenderer) {
            m_imguiRenderer->newFrame();
            m_window->imguiNewFrame();
            ImGui::NewFrame();
            for (auto& layer : m_layers) layer->onImGui();
            ImGui::Render();
        }

        rhi::CommandList& cmd = m_device->commandList();
        for (auto& layer : m_layers) layer->onRender(cmd);

        // UI overlay on top of whatever the layers drew into the swapchain image.
        rhi::TextureHandle bb = m_device->backbuffer();
        const bool untouched = m_device->textureState(bb) == rhi::ResourceState::Undefined;
        if (m_imguiRenderer || untouched) {
            rhi::RenderingDesc pass;
            pass.colors.push_back({bb, untouched ? rhi::LoadOp::Clear : rhi::LoadOp::Load, rhi::StoreOp::Store,
                                   {0.05f, 0.05f, 0.06f, 1.0f}});
            cmd.beginDebugLabel("UI");
            cmd.beginRendering(pass);
            if (m_imguiRenderer) m_imguiRenderer->render(cmd);
            cmd.endRendering();
            cmd.endDebugLabel();
        }

        m_device->endFrame();
    }
    m_device->waitIdle();
}

} // namespace nova
