#include "nova/platform/Input.h"

#include "nova/platform/Window.h"

#include <array>

namespace nova {

platform::Window* Input::s_window = nullptr;

namespace {
glm::vec2 g_lastMouse{0.0f};
glm::vec2 g_mouseDelta{0.0f};
f32 g_scroll = 0.0f;
bool g_firstFrame = true;
std::array<bool, static_cast<size_t>(Key::Count)> g_prevKeys{};
std::array<bool, static_cast<size_t>(Key::Count)> g_currKeys{};
} // namespace

void Input::setWindow(platform::Window* window) {
    s_window = window;
    g_firstFrame = true;
}

void Input::update() {
    if (!s_window) return;
    glm::vec2 mouse = s_window->mousePosition();
    g_mouseDelta = g_firstFrame ? glm::vec2(0.0f) : mouse - g_lastMouse;
    g_lastMouse = mouse;
    g_firstFrame = false;
    g_scroll = s_window->consumeScrollDelta();

    g_prevKeys = g_currKeys;
    for (i32 k = static_cast<i32>(Key::Space); k < static_cast<i32>(Key::Count); ++k)
        g_currKeys[k] = s_window->isKeyDown(static_cast<Key>(k));
}

bool Input::isKeyDown(Key key) { return s_window && s_window->isKeyDown(key); }

bool Input::isKeyPressed(Key key) {
    auto k = static_cast<size_t>(key);
    return k < g_currKeys.size() && g_currKeys[k] && !g_prevKeys[k];
}

bool Input::isMouseButtonDown(MouseButton button) {
    return s_window && s_window->isMouseButtonDown(button);
}

glm::vec2 Input::mousePosition() { return s_window ? s_window->mousePosition() : glm::vec2(0.0f); }
glm::vec2 Input::mouseDelta() { return g_mouseDelta; }
f32 Input::scrollDelta() { return g_scroll; }

} // namespace nova
