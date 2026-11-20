#pragma once

/// @file
/// @brief GLFW implementation of platform::Window.

#include "nova/platform/Window.h"

struct GLFWwindow;

namespace nova::platform {

/// @brief Window backend built on GLFW 3.
///
/// Created through `Window::create(WindowBackend::Glfw, ...)`. GLFW is initialized with the first
/// window and terminated with the last one. The window is created without a client API
/// (`GLFW_NO_API`); the RHI owns the swapchain. See Window for the member documentation.
class GlfwWindow final : public Window {
public:
    /// @param desc Creation parameters.
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

    bool gamepadState(u32 joystick, GamepadState& out) const override;
    bool joystickState(u32 joystick, JoystickState& out) const override;
    std::string joystickName(u32 joystick) const override;
    bool addGamepadMappings(const char* mappings) override;

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
