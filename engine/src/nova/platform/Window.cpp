#include "nova/platform/Window.h"

#include "nova/core/Log.h"

#if NOVA_PLATFORM_GLFW
    #include "nova/platform/glfw/GlfwWindow.h"
#endif

namespace nova::platform {

Scope<Window> Window::create(WindowBackend backend, const WindowDesc& desc) {
    switch (backend) {
#if NOVA_PLATFORM_GLFW
        case WindowBackend::Glfw: return makeScope<GlfwWindow>(desc);
#endif
        default: break;
    }
    NOVA_VERIFY(false, "Requested window backend is not compiled in");
    return nullptr;
}

} // namespace nova::platform
