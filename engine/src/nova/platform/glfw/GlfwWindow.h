#pragma once

#include "nova/platform/Window.h"

struct GLFWwindow;

namespace nova::platform {

class GlfwWindow final : public Window {
public:
    explicit GlfwWindow(const WindowDesc& desc);
    ~GlfwWindow() override;

    void pollEvents() override;
    void waitEvents() override;
    bool shouldClose() const override;
    void requestClose() override;

    Extent2D framebufferSize() const override;
    bool isMinimized() const override;
    void setTitle(const std::string& title) override;
    f64 time() const override;

    bool isKeyDown(Key key) const override;
    bool isMouseButtonDown(MouseButton button) const override;
    glm::vec2 mousePosition() const override;
    f32 consumeScrollDelta() override;
    void setCursorCaptured(bool captured) override;

    void* nativeHandle() const override;

    std::vector<const char*> vulkanInstanceExtensions() const override;
    bool createVulkanSurface(void* vkInstance, void* outSurface) const override;

    void imguiInit() override;
    void imguiNewFrame() override;
    void imguiShutdown() override;

private:
    GLFWwindow* m_window = nullptr;
    f32 m_scroll = 0.0f;
    bool m_imgui = false;
};

} // namespace nova::platform
