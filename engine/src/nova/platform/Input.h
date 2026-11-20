#pragma once

#include "nova/platform/KeyCodes.h"

#include <glm/vec2.hpp>

namespace nova {

namespace platform { class Window; }

// Polling-style input facade over the active window.
class Input {
public:
    static void setWindow(platform::Window* window);
    static void update(); // called once per frame by the Application

    static bool isKeyDown(Key key);
    static bool isKeyPressed(Key key); // went down this frame
    static bool isMouseButtonDown(MouseButton button);
    static glm::vec2 mousePosition();
    static glm::vec2 mouseDelta();
    static f32 scrollDelta();

private:
    static platform::Window* s_window;
};

} // namespace nova
