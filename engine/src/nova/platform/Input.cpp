#include "nova/platform/Input.h"

#include "nova/platform/Window.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <unordered_map>
#include <vector>

namespace nova {

platform::Window* Input::s_window = nullptr;

namespace {

// ------------------------------------------------------------------ state --
glm::vec2 g_lastMouse{0.0f};
glm::vec2 g_mouseDelta{0.0f};
f32 g_scroll = 0.0f;
bool g_firstFrame = true;
bool g_cursorCaptured = false;
std::array<bool, static_cast<size_t>(Key::Count)> g_prevKeys{};
std::array<bool, static_cast<size_t>(Key::Count)> g_currKeys{};
std::array<bool, static_cast<size_t>(MouseButton::Count)> g_prevMouse{};
std::array<bool, static_cast<size_t>(MouseButton::Count)> g_currMouse{};

// A connected gamepad; its position in g_pads is the gamepad index (player = index + 1).
struct PadSlot {
    u32 joystick = 0;  // backend slot
    GamepadState curr; // this frame (raw)
    GamepadState prev; // previous frame (all released if it just connected)
};
std::vector<PadSlot> g_pads;
f32 g_stickDeadZone = 0.15f;
f32 g_triggerDeadZone = 0.05f;

std::unordered_map<std::string, ActionBinding> g_actions;
std::unordered_map<std::string, AxisBinding> g_axes;

const PadSlot* pad(u32 gamepad) { return gamepad < g_pads.size() ? &g_pads[gamepad] : nullptr; }
const PadSlot* padOfPlayer(u32 player) { return player == 0 ? nullptr : pad(player - 1); }

bool keyIn(const std::array<bool, static_cast<size_t>(Key::Count)>& keys, Key k) {
    auto i = static_cast<size_t>(k);
    return i < keys.size() && keys[i];
}

bool mouseIn(const std::array<bool, static_cast<size_t>(MouseButton::Count)>& buttons, MouseButton b) {
    auto i = static_cast<size_t>(b);
    return i < buttons.size() && buttons[i];
}

// Raw stick (Y down) -> processed (Y up, dead zone).
glm::vec2 processedStick(const GamepadState& s, GamepadAxis x, GamepadAxis y) {
    return Input::applyRadialDeadZone({s.axis(x), -s.axis(y)}, g_stickDeadZone);
}

glm::vec2 dpad(const GamepadState& s) {
    glm::vec2 v{0.0f};
    if (s.button(GamepadButton::DpadRight)) v.x += 1.0f;
    if (s.button(GamepadButton::DpadLeft)) v.x -= 1.0f;
    if (s.button(GamepadButton::DpadUp)) v.y += 1.0f;
    if (s.button(GamepadButton::DpadDown)) v.y -= 1.0f;
    return glm::length(v) > 1.0f ? glm::normalize(v) : v;
}

// Whether `player` holds `action` in the current (`current = true`) or previous frame.
bool actionHeld(const ActionBinding& b, u32 player, bool current) {
    if (player == Input::kKeyboardMousePlayer) {
        const auto& keys = current ? g_currKeys : g_prevKeys;
        const auto& mouse = current ? g_currMouse : g_prevMouse;
        for (Key k : b.keys)
            if (keyIn(keys, k)) return true;
        for (MouseButton m : b.mouseButtons)
            if (mouseIn(mouse, m)) return true;
        return false;
    }
    const PadSlot* p = padOfPlayer(player);
    if (!p) return false;
    const GamepadState& s = current ? p->curr : p->prev;
    for (GamepadButton gb : b.gamepadButtons)
        if (s.button(gb)) return true;
    for (GamepadAxis t : b.gamepadTriggers)
        if (Input::applyTriggerDeadZone(s.axis(t), g_triggerDeadZone) >= b.triggerThreshold) return true;
    return false;
}

const ActionBinding* findAction(const std::string& name) {
    auto it = g_actions.find(name);
    return it != g_actions.end() ? &it->second : nullptr;
}

} // namespace

// ----------------------------------------------------------------- update --
void Input::setWindow(platform::Window* window) {
    s_window = window;
    g_firstFrame = true;
    g_cursorCaptured = false;
    g_pads.clear();
    g_prevKeys = g_currKeys = {};
    g_prevMouse = g_currMouse = {};
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
    g_prevMouse = g_currMouse;
    for (i32 b = 0; b < static_cast<i32>(MouseButton::Count); ++b)
        g_currMouse[b] = s_window->isMouseButtonDown(static_cast<MouseButton>(b));

    // Gamepads: rebuild the dense list, keeping each device's previous state for edge detection.
    std::vector<PadSlot> pads;
    for (u32 j = 0; j < kMaxJoysticks; ++j) {
        PadSlot slot;
        slot.joystick = j;
        if (!s_window->gamepadState(j, slot.curr)) continue;
        auto old = std::find_if(g_pads.begin(), g_pads.end(), [&](const PadSlot& p) { return p.joystick == j; });
        if (old != g_pads.end()) slot.prev = old->curr;
        pads.push_back(slot);
    }
    g_pads = std::move(pads);
}

// ---------------------------------------------------------------- players --
u32 Input::playerCount() { return 1u + gamepadCount(); }

bool Input::isPlayerConnected(u32 player) { return player == kKeyboardMousePlayer || padOfPlayer(player) != nullptr; }

InputDevice Input::playerDevice(u32 player) {
    return player == kKeyboardMousePlayer ? InputDevice::KeyboardMouse : InputDevice::Gamepad;
}

std::string Input::playerDeviceName(u32 player) {
    if (player == kKeyboardMousePlayer) return "Keyboard & Mouse";
    return gamepadName(gamepadOfPlayer(player));
}

// -------------------------------------------------------- actions & axes --
void Input::bindAction(const std::string& action, ActionBinding binding) { g_actions[action] = std::move(binding); }
void Input::bindAxis(const std::string& axis, AxisBinding binding) { g_axes[axis] = binding; }

void Input::clearBindings() {
    g_actions.clear();
    g_axes.clear();
}

bool Input::isActionDown(const std::string& action, u32 player) {
    const ActionBinding* b = findAction(action);
    return b && actionHeld(*b, player, true);
}

bool Input::isActionPressed(const std::string& action, u32 player) {
    const ActionBinding* b = findAction(action);
    return b && actionHeld(*b, player, true) && !actionHeld(*b, player, false);
}

bool Input::isActionReleased(const std::string& action, u32 player) {
    const ActionBinding* b = findAction(action);
    return b && !actionHeld(*b, player, true) && actionHeld(*b, player, false);
}

glm::vec2 Input::axis(const std::string& name, u32 player) {
    auto it = g_axes.find(name);
    if (it == g_axes.end()) return glm::vec2(0.0f);
    const AxisBinding& b = it->second;

    if (player == kKeyboardMousePlayer) {
        glm::vec2 v{0.0f};
        if (b.right != Key::Unknown && keyIn(g_currKeys, b.right)) v.x += 1.0f;
        if (b.left != Key::Unknown && keyIn(g_currKeys, b.left)) v.x -= 1.0f;
        if (b.up != Key::Unknown && keyIn(g_currKeys, b.up)) v.y += 1.0f;
        if (b.down != Key::Unknown && keyIn(g_currKeys, b.down)) v.y -= 1.0f;
        if (glm::length(v) > 1.0f) v = glm::normalize(v);
        if (b.mouseDelta) v += glm::vec2(g_mouseDelta.x, -g_mouseDelta.y) * b.mouseScale;
        return v;
    }

    const PadSlot* p = padOfPlayer(player);
    if (!p) return glm::vec2(0.0f);
    switch (b.stick) {
        case GamepadStick::Left: return processedStick(p->curr, GamepadAxis::LeftX, GamepadAxis::LeftY) * b.stickScale;
        case GamepadStick::Right: return processedStick(p->curr, GamepadAxis::RightX, GamepadAxis::RightY) * b.stickScale;
        case GamepadStick::Dpad: return dpad(p->curr) * b.stickScale;
        case GamepadStick::None: break;
    }
    return glm::vec2(0.0f);
}

// ------------------------------------------------------- keyboard & mouse --
bool Input::isKeyDown(Key key) { return keyIn(g_currKeys, key); }
bool Input::isKeyPressed(Key key) { return keyIn(g_currKeys, key) && !keyIn(g_prevKeys, key); }
bool Input::isKeyReleased(Key key) { return !keyIn(g_currKeys, key) && keyIn(g_prevKeys, key); }
bool Input::isMouseButtonDown(MouseButton button) { return mouseIn(g_currMouse, button); }
bool Input::isMouseButtonPressed(MouseButton button) {
    return mouseIn(g_currMouse, button) && !mouseIn(g_prevMouse, button);
}
glm::vec2 Input::mousePosition() { return s_window ? s_window->mousePosition() : glm::vec2(0.0f); }
glm::vec2 Input::mouseDelta() { return g_mouseDelta; }

void Input::setCursorCaptured(bool captured) {
    if (!s_window || captured == g_cursorCaptured) return;
    g_cursorCaptured = captured;
    s_window->setCursorCaptured(captured);
    g_lastMouse = s_window->mousePosition(); // the platform moved the cursor: no motion from that
}

bool Input::cursorCaptured() { return g_cursorCaptured; }
f32 Input::scrollDelta() { return g_scroll; }

// ---------------------------------------------------------------- gamepads --
u32 Input::gamepadCount() { return static_cast<u32>(g_pads.size()); }

bool Input::isGamepadConnected(u32 gamepad) { return pad(gamepad) != nullptr; }

std::string Input::gamepadName(u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    return (p && s_window) ? s_window->joystickName(p->joystick) : std::string();
}

bool Input::isGamepadButtonDown(GamepadButton button, u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    return p && p->curr.button(button);
}

bool Input::isGamepadButtonPressed(GamepadButton button, u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    return p && p->curr.button(button) && !p->prev.button(button);
}

bool Input::isGamepadButtonReleased(GamepadButton button, u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    return p && !p->curr.button(button) && p->prev.button(button);
}

f32 Input::gamepadAxis(GamepadAxis axis, u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    if (!p) return 0.0f;
    switch (axis) {
        case GamepadAxis::LeftX: return processedStick(p->curr, GamepadAxis::LeftX, GamepadAxis::LeftY).x;
        case GamepadAxis::LeftY: return processedStick(p->curr, GamepadAxis::LeftX, GamepadAxis::LeftY).y;
        case GamepadAxis::RightX: return processedStick(p->curr, GamepadAxis::RightX, GamepadAxis::RightY).x;
        case GamepadAxis::RightY: return processedStick(p->curr, GamepadAxis::RightX, GamepadAxis::RightY).y;
        case GamepadAxis::LeftTrigger:
        case GamepadAxis::RightTrigger: return applyTriggerDeadZone(p->curr.axis(axis), g_triggerDeadZone);
        default: return 0.0f;
    }
}

glm::vec2 Input::gamepadLeftStick(u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    return p ? processedStick(p->curr, GamepadAxis::LeftX, GamepadAxis::LeftY) : glm::vec2(0.0f);
}

glm::vec2 Input::gamepadRightStick(u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    return p ? processedStick(p->curr, GamepadAxis::RightX, GamepadAxis::RightY) : glm::vec2(0.0f);
}

glm::vec2 Input::gamepadDpad(u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    return p ? dpad(p->curr) : glm::vec2(0.0f);
}

bool Input::gamepadRawState(GamepadState& out, u32 gamepad) {
    const PadSlot* p = pad(gamepad);
    if (!p) return false;
    out = p->curr;
    return true;
}

void Input::setStickDeadZone(f32 deadZone) { g_stickDeadZone = std::clamp(deadZone, 0.0f, 0.99f); }
void Input::setTriggerDeadZone(f32 deadZone) { g_triggerDeadZone = std::clamp(deadZone, 0.0f, 0.99f); }

bool Input::addGamepadMappings(const char* mappings) { return s_window && s_window->addGamepadMappings(mappings); }

bool Input::joystickState(u32 joystick, JoystickState& out) {
    return s_window && s_window->joystickState(joystick, out);
}

std::string Input::joystickName(u32 joystick) { return s_window ? s_window->joystickName(joystick) : std::string(); }

// ---------------------------------------------------------- dead zone math --
glm::vec2 Input::applyRadialDeadZone(glm::vec2 stick, f32 deadZone) {
    const f32 len = glm::length(stick);
    if (len <= deadZone || len <= 0.0f) return glm::vec2(0.0f);
    const f32 scaled = std::min((len - deadZone) / (1.0f - deadZone), 1.0f);
    return stick / len * scaled;
}

f32 Input::applyTriggerDeadZone(f32 rawTrigger, f32 deadZone) {
    const f32 v = std::clamp((rawTrigger + 1.0f) * 0.5f, 0.0f, 1.0f);
    if (v <= deadZone) return 0.0f;
    return std::min((v - deadZone) / (1.0f - deadZone), 1.0f);
}

} // namespace nova
