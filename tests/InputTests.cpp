// Headless input tests: a fake Window drives nova::Input (keyboard, mouse, gamepads, players).
#include <nova/Nova.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace nova;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAILED: %s (%s:%d)\n", expr, file, line);
    }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) check(std::abs((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)
#define CHECK_VEC(a, b, eps) check(glm::length((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)

constexpr f32 kEps = 1e-4f;

// Scriptable window: tests set keys/mouse/gamepads, then call Input::update() as a frame would.
class FakeWindow final : public platform::Window {
public:
    std::set<Key> keys;
    std::set<MouseButton> mouseButtons;
    glm::vec2 mouse{0.0f};
    std::map<u32, GamepadState> pads; // backend slot -> state
    std::map<u32, std::string> names;

    void pollEvents() override {}
    void waitEvents() override {}
    bool shouldClose() const override { return false; }
    void requestClose() override {}
    Extent2D framebufferSize() const override { return {800, 600}; }
    bool isMinimized() const override { return false; }
    void setTitle(const std::string&) override {}
    f64 time() const override { return 0.0; }
    bool isKeyDown(Key key) const override { return keys.count(key) != 0; }
    bool isMouseButtonDown(MouseButton b) const override { return mouseButtons.count(b) != 0; }
    glm::vec2 mousePosition() const override { return mouse; }
    f32 consumeScrollDelta() override { return 0.0f; }
    void setCursorCaptured(bool) override {}
    bool gamepadState(u32 joystick, GamepadState& out) const override {
        auto it = pads.find(joystick);
        if (it == pads.end()) return false;
        out = it->second;
        return true;
    }
    bool joystickState(u32 joystick, JoystickState& out) const override {
        if (!pads.count(joystick)) return false;
        out = {{0.5f}, {true}, {HatUp}};
        return true;
    }
    std::string joystickName(u32 joystick) const override {
        auto it = names.find(joystick);
        return it != names.end() ? it->second : std::string();
    }
    bool addGamepadMappings(const char*) override { return true; }
    void* nativeHandle() const override { return nullptr; }
    std::vector<const char*> vulkanInstanceExtensions() const override { return {}; }
    bool createVulkanSurface(void*, void*) const override { return false; }
    void imguiInit() override {}
    void imguiNewFrame() override {}
    void imguiShutdown() override {}

    // A gamepad at rest: sticks centered, triggers released (-1).
    static GamepadState idlePad() {
        GamepadState s;
        s.axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = -1.0f;
        s.axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = -1.0f;
        return s;
    }
    void setButton(u32 slot, GamepadButton b, bool down) { pads[slot].buttons[static_cast<size_t>(b)] = down; }
    void setAxis(u32 slot, GamepadAxis a, f32 v) { pads[slot].axes[static_cast<size_t>(a)] = v; }
};

struct Fixture {
    FakeWindow window;
    Fixture() {
        Input::setWindow(&window);
        Input::clearBindings();
        Input::setStickDeadZone(0.15f);
        Input::setTriggerDeadZone(0.05f);
    }
    ~Fixture() { Input::setWindow(nullptr); }
    void frame() { Input::update(); }
};

void playersWithoutGamepads() {
    Fixture f;
    f.frame();
    CHECK(Input::playerCount() == 1);
    CHECK(Input::isPlayerConnected(0));
    CHECK(Input::playerDevice(0) == InputDevice::KeyboardMouse);
    CHECK(Input::playerDeviceName(0) == "Keyboard & Mouse");
    CHECK(!Input::isPlayerConnected(1));
    CHECK(Input::gamepadCount() == 0);
}

void gamepadsBecomeExtraPlayers() {
    Fixture f;
    // Backend slots with gaps: 3 and 7.
    f.window.pads[3] = FakeWindow::idlePad();
    f.window.pads[7] = FakeWindow::idlePad();
    f.window.names[3] = "Xbox Controller";
    f.window.names[7] = "PS5 Controller";
    f.frame();
    CHECK(Input::playerCount() == 3);
    CHECK(Input::playerDevice(1) == InputDevice::Gamepad);
    CHECK(Input::playerDeviceName(1) == "Xbox Controller");
    CHECK(Input::playerDeviceName(2) == "PS5 Controller");
    CHECK(Input::gamepadOfPlayer(2) == 1);

    // Unplugging the first pad moves the other one to player 1.
    f.window.pads.erase(3);
    f.frame();
    CHECK(Input::playerCount() == 2);
    CHECK(Input::playerDeviceName(1) == "PS5 Controller");
    CHECK(!Input::isPlayerConnected(2));
}

void actionsArePerPlayer() {
    Fixture f;
    f.window.pads[0] = FakeWindow::idlePad();
    Input::bindAction("Jump", {.keys = {Key::Space}, .gamepadButtons = {GamepadButton::A}});
    f.frame();

    f.window.keys.insert(Key::Space);
    f.frame();
    CHECK(Input::isActionPressed("Jump", 0));
    CHECK(Input::isActionDown("Jump", 0));
    CHECK(!Input::isActionDown("Jump", 1)); // the keyboard belongs to player 0 only
    f.frame();
    CHECK(Input::isActionDown("Jump", 0));
    CHECK(!Input::isActionPressed("Jump", 0)); // only on the first frame
    f.window.keys.clear();
    f.frame();
    CHECK(Input::isActionReleased("Jump", 0));
    CHECK(!Input::isActionDown("Jump", 0));

    f.window.setButton(0, GamepadButton::A, true);
    f.frame();
    CHECK(Input::isActionPressed("Jump", 1));
    CHECK(!Input::isActionDown("Jump", 0)); // the gamepad belongs to player 1 only
    f.window.setButton(0, GamepadButton::A, false);
    f.frame();
    CHECK(Input::isActionReleased("Jump", 1));

    CHECK(!Input::isActionDown("DoesNotExist", 0));
    CHECK(!Input::isActionDown("Jump", 5)); // disconnected player
}

void mouseButtonsAndTriggersAsActions() {
    Fixture f;
    f.window.pads[0] = FakeWindow::idlePad();
    Input::bindAction("Fire", {.mouseButtons = {MouseButton::Left},
                               .gamepadTriggers = {GamepadAxis::RightTrigger},
                               .triggerThreshold = 0.5f});
    f.frame();
    f.window.mouseButtons.insert(MouseButton::Left);
    f.frame();
    CHECK(Input::isActionPressed("Fire", 0));

    f.window.setAxis(0, GamepadAxis::RightTrigger, -0.2f); // processed ~0.37: below threshold
    f.frame();
    CHECK(!Input::isActionDown("Fire", 1));
    f.window.setAxis(0, GamepadAxis::RightTrigger, 0.2f); // processed ~0.58: above threshold
    f.frame();
    CHECK(Input::isActionPressed("Fire", 1));
}

void axesPerPlayer() {
    Fixture f;
    f.window.pads[0] = FakeWindow::idlePad();
    Input::bindAxis("Move", {.up = Key::W, .down = Key::S, .left = Key::A, .right = Key::D, .stick = GamepadStick::Left});
    Input::bindAxis("Look", {.mouseDelta = true, .mouseScale = 0.1f, .stick = GamepadStick::Right, .stickScale = 2.0f});
    f.frame();

    f.window.keys = {Key::W, Key::D};
    f.window.mouse = {10.0f, 5.0f}; // moved right and down
    f.window.setAxis(0, GamepadAxis::LeftY, -1.0f); // raw Y down convention: -1 = pushed up
    f.window.setAxis(0, GamepadAxis::RightX, 1.0f);
    f.frame();

    CHECK_VEC(Input::axis("Move", 0), glm::normalize(glm::vec2(1, 1)), kEps); // diagonal normalized
    CHECK_VEC(Input::axis("Look", 0), glm::vec2(1.0f, -0.5f), kEps);         // +Y = up, so down is negative
    CHECK_VEC(Input::axis("Move", 1), glm::vec2(0.0f, 1.0f), kEps);           // stick up = +Y
    CHECK_VEC(Input::axis("Look", 1), glm::vec2(2.0f, 0.0f), kEps);           // stickScale
    CHECK_VEC(Input::axis("Move", 2), glm::vec2(0.0f), kEps);                 // no such player
    CHECK_VEC(Input::axis("Nope", 0), glm::vec2(0.0f), kEps);
}

void dpadAxis() {
    Fixture f;
    f.window.pads[0] = FakeWindow::idlePad();
    Input::bindAxis("Menu", {.stick = GamepadStick::Dpad});
    f.window.setButton(0, GamepadButton::DpadUp, true);
    f.window.setButton(0, GamepadButton::DpadLeft, true);
    f.frame();
    CHECK_VEC(Input::axis("Menu", 1), glm::normalize(glm::vec2(-1, 1)), kEps);
    CHECK_VEC(Input::gamepadDpad(0), glm::normalize(glm::vec2(-1, 1)), kEps);
}

void deadZones() {
    CHECK_VEC(Input::applyRadialDeadZone({0.1f, 0.0f}, 0.15f), glm::vec2(0.0f), kEps);
    CHECK_VEC(Input::applyRadialDeadZone({0.575f, 0.0f}, 0.15f), glm::vec2(0.5f, 0.0f), kEps); // rescaled
    CHECK_VEC(Input::applyRadialDeadZone({1.0f, 0.0f}, 0.15f), glm::vec2(1.0f, 0.0f), kEps);
    CHECK_NEAR(glm::length(Input::applyRadialDeadZone({1.0f, 1.0f}, 0.15f)), 1.0f, kEps); // clamped corner
    // Radial: a small diagonal below the radius is zero even though each axis alone might not be.
    CHECK_VEC(Input::applyRadialDeadZone({0.1f, 0.1f}, 0.15f), glm::vec2(0.0f), kEps);

    CHECK_NEAR(Input::applyTriggerDeadZone(-1.0f, 0.05f), 0.0f, kEps);
    CHECK_NEAR(Input::applyTriggerDeadZone(1.0f, 0.05f), 1.0f, kEps);
    CHECK_NEAR(Input::applyTriggerDeadZone(-0.95f, 0.05f), 0.0f, kEps); // 0.025 < dead zone

    Fixture f;
    f.window.pads[0] = FakeWindow::idlePad();
    f.window.setAxis(0, GamepadAxis::LeftX, 0.1f); // stick drift
    f.window.setAxis(0, GamepadAxis::LeftTrigger, 1.0f);
    f.frame();
    CHECK_VEC(Input::gamepadLeftStick(0), glm::vec2(0.0f), kEps);
    CHECK_NEAR(Input::gamepadAxis(GamepadAxis::LeftTrigger, 0), 1.0f, kEps);
    Input::setStickDeadZone(0.0f);
    CHECK_NEAR(Input::gamepadAxis(GamepadAxis::LeftX, 0), 0.1f, kEps);
}

void gamepadButtonEdges() {
    Fixture f;
    // A pad that connects with A already held reports a press on its first frame.
    f.window.pads[2] = FakeWindow::idlePad();
    f.window.setButton(2, GamepadButton::A, true);
    f.frame();
    CHECK(Input::isGamepadButtonPressed(GamepadButton::A, 0));
    f.frame();
    CHECK(Input::isGamepadButtonDown(GamepadButton::A, 0));
    CHECK(!Input::isGamepadButtonPressed(GamepadButton::A, 0));
    f.window.setButton(2, GamepadButton::A, false);
    f.frame();
    CHECK(Input::isGamepadButtonReleased(GamepadButton::A, 0));
    CHECK(!Input::isGamepadButtonDown(GamepadButton::B, 3)); // missing gamepad -> neutral

    JoystickState raw;
    CHECK(Input::joystickState(2, raw) && raw.axes.size() == 1 && raw.hats[0] == HatUp);
    CHECK(!Input::joystickState(9, raw));
}

void keyboardEdges() {
    Fixture f;
    f.window.keys.insert(Key::E);
    f.window.mouseButtons.insert(MouseButton::Right);
    f.frame();
    CHECK(Input::isKeyPressed(Key::E) && Input::isKeyDown(Key::E));
    CHECK(Input::isMouseButtonPressed(MouseButton::Right));
    f.window.keys.clear();
    f.frame();
    CHECK(Input::isKeyReleased(Key::E) && !Input::isKeyDown(Key::E));
    CHECK(Input::isMouseButtonDown(MouseButton::Right) && !Input::isMouseButtonPressed(MouseButton::Right));
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"players without gamepads", playersWithoutGamepads},
        {"gamepads become extra players", gamepadsBecomeExtraPlayers},
        {"actions are per player", actionsArePerPlayer},
        {"mouse buttons and triggers as actions", mouseButtonsAndTriggersAsActions},
        {"axes per player", axesPerPlayer},
        {"d-pad axis", dpadAxis},
        {"dead zones", deadZones},
        {"gamepad button edges", gamepadButtonEdges},
        {"keyboard and mouse edges", keyboardEdges},
    };
    for (auto& [name, fn] : tests) {
        int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
