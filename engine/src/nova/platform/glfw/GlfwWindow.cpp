#include "nova/platform/glfw/GlfwWindow.h"

#include "nova/core/Log.h"

// Vulkan must be declared before GLFW so glfwCreateWindowSurface is available.
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(_WIN32)
    #define GLFW_EXPOSE_NATIVE_WIN32
    #include <GLFW/glfw3native.h>
#endif

#include <imgui_impl_glfw.h>

namespace nova::platform {

namespace {
int g_glfwRefCount = 0;

void glfwErrorCallback(int code, const char* description) {
    NOVA_CORE_ERROR("GLFW error {}: {}", code, description);
}
} // namespace

GlfwWindow::GlfwWindow(const WindowDesc& desc) {
    if (g_glfwRefCount++ == 0) {
        glfwSetErrorCallback(glfwErrorCallback);
        NOVA_VERIFY(glfwInit() == GLFW_TRUE, "glfwInit failed");
        NOVA_VERIFY(glfwVulkanSupported() == GLFW_TRUE, "GLFW reports no Vulkan loader/ICD");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // the RHI owns the swapchain
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, desc.maximized ? GLFW_TRUE : GLFW_FALSE);

    m_window = glfwCreateWindow(static_cast<int>(desc.width), static_cast<int>(desc.height),
                                desc.title.c_str(), nullptr, nullptr);
    NOVA_VERIFY(m_window != nullptr, "glfwCreateWindow failed");

    glfwSetWindowUserPointer(m_window, this);
    // Installed before ImGui so ImGui's backend chains to it.
    glfwSetScrollCallback(m_window, [](GLFWwindow* w, double, double y) {
        static_cast<GlfwWindow*>(glfwGetWindowUserPointer(w))->m_scroll += static_cast<f32>(y);
    });

    NOVA_CORE_INFO("Window created ({}x{}, GLFW {})", desc.width, desc.height, glfwGetVersionString());
}

GlfwWindow::~GlfwWindow() {
    if (m_imgui) imguiShutdown();
    glfwDestroyWindow(m_window);
    if (--g_glfwRefCount == 0) glfwTerminate();
}

void GlfwWindow::pollEvents() { glfwPollEvents(); }
void GlfwWindow::waitEvents() { glfwWaitEvents(); }
bool GlfwWindow::shouldClose() const { return glfwWindowShouldClose(m_window) == GLFW_TRUE; }
void GlfwWindow::requestClose() { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }

Extent2D GlfwWindow::framebufferSize() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return {static_cast<u32>(w), static_cast<u32>(h)};
}

bool GlfwWindow::isMinimized() const {
    return glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE || framebufferSize().isZero();
}

void GlfwWindow::setTitle(const std::string& title) { glfwSetWindowTitle(m_window, title.c_str()); }
f64 GlfwWindow::time() const { return glfwGetTime(); }

bool GlfwWindow::isKeyDown(Key key) const {
    return glfwGetKey(m_window, static_cast<int>(key)) == GLFW_PRESS;
}

bool GlfwWindow::isMouseButtonDown(MouseButton button) const {
    return glfwGetMouseButton(m_window, static_cast<int>(button)) == GLFW_PRESS;
}

glm::vec2 GlfwWindow::mousePosition() const {
    double x = 0, y = 0;
    glfwGetCursorPos(m_window, &x, &y);
    return {static_cast<f32>(x), static_cast<f32>(y)};
}

f32 GlfwWindow::consumeScrollDelta() {
    f32 s = m_scroll;
    m_scroll = 0.0f;
    return s;
}

void GlfwWindow::setCursorCaptured(bool captured) {
    glfwSetInputMode(m_window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
}

void* GlfwWindow::nativeHandle() const {
#if defined(_WIN32)
    return glfwGetWin32Window(m_window);
#else
    return m_window;
#endif
}

std::vector<const char*> GlfwWindow::vulkanInstanceExtensions() const {
    uint32_t count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    return {exts, exts + count};
}

bool GlfwWindow::createVulkanSurface(void* vkInstance, void* outSurface) const {
    VkResult r = glfwCreateWindowSurface(static_cast<VkInstance>(vkInstance), m_window, nullptr,
                                         static_cast<VkSurfaceKHR*>(outSurface));
    return r == VK_SUCCESS;
}

void GlfwWindow::imguiInit() {
    // The renderer backend is chosen by the RHI; GLFW only provides input/platform hooks.
    ImGui_ImplGlfw_InitForVulkan(m_window, true);
    m_imgui = true;
}

void GlfwWindow::imguiNewFrame() { ImGui_ImplGlfw_NewFrame(); }

void GlfwWindow::imguiShutdown() {
    if (!m_imgui) return;
    ImGui_ImplGlfw_Shutdown();
    m_imgui = false;
}

} // namespace nova::platform
