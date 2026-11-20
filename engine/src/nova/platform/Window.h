#pragma once

#include "nova/core/Base.h"
#include "nova/platform/KeyCodes.h"

#include <glm/vec2.hpp>

#include <string>
#include <vector>

namespace nova::platform {

enum class WindowBackend { Glfw };

struct WindowDesc {
    std::string title = "Nova";
    u32 width = 1600;
    u32 height = 900;
    bool resizable = true;
    bool maximized = false;
};

// Abstract OS window + input source.
//
// The window layer must not depend on any graphics API, so the Vulkan WSI bridge is
// type-erased (void*). A future D3D12 backend would use nativeHandle() (HWND) instead.
class Window : NonCopyable {
public:
    static Scope<Window> create(WindowBackend backend, const WindowDesc& desc);
    virtual ~Window() = default;

    virtual void pollEvents() = 0;
    virtual void waitEvents() = 0;
    virtual bool shouldClose() const = 0;
    virtual void requestClose() = 0;

    virtual Extent2D framebufferSize() const = 0;
    virtual bool isMinimized() const = 0;
    virtual void setTitle(const std::string& title) = 0;
    virtual f64 time() const = 0;

    // Input
    virtual bool isKeyDown(Key key) const = 0;
    virtual bool isMouseButtonDown(MouseButton button) const = 0;
    virtual glm::vec2 mousePosition() const = 0;
    virtual f32 consumeScrollDelta() = 0;
    virtual void setCursorCaptured(bool captured) = 0;

    // Native handle (HWND on Windows) for APIs that create surfaces from it.
    virtual void* nativeHandle() const = 0;

    // Vulkan WSI bridge.
    virtual std::vector<const char*> vulkanInstanceExtensions() const = 0;
    // `vkInstance` is a VkInstance, `outSurface` a VkSurfaceKHR*.
    virtual bool createVulkanSurface(void* vkInstance, void* outSurface) const = 0;

    // Dear ImGui platform backend (input + display size).
    virtual void imguiInit() = 0;
    virtual void imguiNewFrame() = 0;
    virtual void imguiShutdown() = 0;
};

} // namespace nova::platform
