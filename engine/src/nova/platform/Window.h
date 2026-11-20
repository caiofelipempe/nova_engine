#pragma once

/// @file
/// @brief Backend-agnostic OS window and input source.

#include "nova/core/Base.h"
#include "nova/platform/KeyCodes.h"

#include <glm/vec2.hpp>

#include <string>
#include <vector>

/// @brief OS abstraction layer (windows, input, files).
namespace nova::platform {

/// @brief Available window/input backends.
enum class WindowBackend {
    Glfw ///< GLFW 3 (Windows, Linux, macOS).
};

/// @brief Window creation parameters.
struct WindowDesc {
    std::string title = "Nova"; ///< Title bar text.
    u32 width = 1600;           ///< Initial width in screen coordinates.
    u32 height = 900;           ///< Initial height in screen coordinates.
    bool resizable = true;      ///< Allow the user to resize the window.
    bool maximized = false;     ///< Start maximized.
};

/// @brief Abstract OS window and input source.
///
/// The window layer must not depend on any graphics API, so the Vulkan WSI bridge is
/// type-erased (`void*`). A future D3D12 backend would use nativeHandle() (an `HWND`) instead.
///
/// Game code normally reads input through the nova::Input facade rather than this class.
class Window : NonCopyable {
public:
    /// @brief Creates a window with the given backend.
    /// @param backend Which implementation to use; must be compiled in (see the `NOVA_PLATFORM_*` CMake options).
    /// @param desc Creation parameters.
    static Scope<Window> create(WindowBackend backend, const WindowDesc& desc);
    virtual ~Window() = default;

    /// @brief Processes pending OS events without blocking.
    virtual void pollEvents() = 0;
    /// @brief Blocks until at least one OS event arrives (used while minimized).
    virtual void waitEvents() = 0;
    /// @brief True once the user (or requestClose()) asked the window to close.
    virtual bool shouldClose() const = 0;
    /// @brief Flags the window to close; the main loop exits at the end of the frame.
    virtual void requestClose() = 0;

    /// @brief Size of the drawable area in pixels (may differ from window size on HiDPI).
    virtual Extent2D framebufferSize() const = 0;
    /// @brief True when iconified or when the framebuffer has zero area.
    virtual bool isMinimized() const = 0;
    /// @brief Changes the title bar text.
    virtual void setTitle(const std::string& title) = 0;
    /// @brief Seconds since the window system was initialized.
    virtual f64 time() const = 0;

    /// @name Input
    /// @{
    /// @brief True while `key` is held down.
    virtual bool isKeyDown(Key key) const = 0;
    /// @brief True while `button` is held down.
    virtual bool isMouseButtonDown(MouseButton button) const = 0;
    /// @brief Cursor position in screen coordinates, relative to the window's top-left corner.
    virtual glm::vec2 mousePosition() const = 0;
    /// @brief Vertical wheel movement accumulated since the last call (positive = away from the user).
    virtual f32 consumeScrollDelta() = 0;
    /// @brief Hides and locks the cursor (relative/raw mouse motion) when `captured` is true.
    virtual void setCursorCaptured(bool captured) = 0;
    /// @}

    /// @name Joysticks and gamepads
    /// Joysticks are identified by a backend slot in `[0, kMaxJoysticks)`. Slots may have gaps;
    /// nova::Input maps connected gamepads to dense player indices.
    /// @{
    /// @brief Raw state of a joystick that has a standard gamepad mapping.
    /// @param joystick Backend slot.
    /// @param[out] out Filled on success.
    /// @return False if the slot is empty or the device has no gamepad mapping.
    virtual bool gamepadState(u32 joystick, GamepadState& out) const = 0;
    /// @brief Raw axes/buttons/hats of any connected joystick (mapped or not).
    /// @return False if the slot is empty.
    virtual bool joystickState(u32 joystick, JoystickState& out) const = 0;
    /// @brief Device name (the mapping's name for gamepads), or empty if the slot is empty.
    virtual std::string joystickName(u32 joystick) const = 0;
    /// @brief Adds/updates gamepad mappings in SDL_GameControllerDB format (one mapping per line).
    /// @return False if the string could not be parsed.
    virtual bool addGamepadMappings(const char* mappings) = 0;
    /// @}

    /// @brief Native window handle (`HWND` on Windows) for APIs that create surfaces from it.
    virtual void* nativeHandle() const = 0;

    /// @name Vulkan WSI bridge
    /// @{
    /// @brief Instance extensions the window system needs (e.g. `VK_KHR_win32_surface`).
    virtual std::vector<const char*> vulkanInstanceExtensions() const = 0;
    /// @brief Creates a presentation surface.
    /// @param vkInstance A `VkInstance`.
    /// @param outSurface Pointer to a `VkSurfaceKHR` that receives the surface.
    /// @return True on success.
    virtual bool createVulkanSurface(void* vkInstance, void* outSurface) const = 0;
    /// @}

    /// @name Dear ImGui platform backend
    /// @{
    /// @brief Installs ImGui input/display hooks. @pre An ImGui context exists.
    virtual void imguiInit() = 0;
    /// @brief Feeds display size and input to ImGui; call before `ImGui::NewFrame()`.
    virtual void imguiNewFrame() = 0;
    /// @brief Removes the ImGui hooks.
    virtual void imguiShutdown() = 0;
    /// @}
};

} // namespace nova::platform
