#pragma once

/// @file
/// @brief Polling-style input: players, actions/axes, and raw keyboard, mouse, gamepad and joystick queries.

#include "nova/platform/KeyCodes.h"

#include <glm/vec2.hpp>

#include <string>
#include <vector>

namespace nova {

namespace platform { class Window; }

/// @brief Device that drives a player.
enum class InputDevice {
    KeyboardMouse, ///< Player 0.
    Gamepad,       ///< Players 1, 2, ... (one gamepad each).
};

/// @brief Physical inputs that trigger a named action (any of them).
///
/// Keyboard/mouse entries are used by player 0; gamepad entries by players ≥ 1.
struct ActionBinding {
    std::vector<Key> keys;                       ///< Keyboard keys (player 0).
    std::vector<MouseButton> mouseButtons;       ///< Mouse buttons (player 0).
    std::vector<GamepadButton> gamepadButtons;   ///< Gamepad buttons (players ≥ 1).
    std::vector<GamepadAxis> gamepadTriggers;    ///< Triggers treated as buttons (players ≥ 1).
    f32 triggerThreshold = 0.5f;                 ///< Processed trigger value (0..1) that counts as pressed.
};

/// @brief Stick (or d-pad) a 2D axis reads on gamepads.
enum class GamepadStick { None, Left, Right, Dpad };

/// @brief Physical inputs that produce a named 2D axis (+X = right, +Y = up/forward).
///
/// Player 0 combines the four keys (diagonals normalized) and, optionally, mouse movement;
/// players ≥ 1 read `stick`.
struct AxisBinding {
    Key up = Key::Unknown;               ///< +Y key (player 0).
    Key down = Key::Unknown;             ///< −Y key (player 0).
    Key left = Key::Unknown;             ///< −X key (player 0).
    Key right = Key::Unknown;            ///< +X key (player 0).
    bool mouseDelta = false;             ///< Add mouse movement (pixels × mouseScale, +Y = up) for player 0.
    f32 mouseScale = 1.0f;               ///< Multiplier for mouse movement.
    GamepadStick stick = GamepadStick::None; ///< Stick for players ≥ 1 (after dead zone).
    f32 stickScale = 1.0f;               ///< Multiplier for the stick value.
};

/// @brief Polling-style input over the active window.
///
/// State is sampled once per frame by the Application (before Layer::onUpdate), so "pressed this
/// frame" queries and deltas are consistent for the whole frame.
///
/// **Players.** Player 0 is the keyboard + mouse; players 1, 2, ... are the connected gamepads
/// in connection-slot order (player `p` ↔ gamepad `p - 1`). Bind named actions/axes once and query
/// them per player; the same game code then works for every player and device:
/// @code
/// nova::Input::bindAction("Jump", {.keys = {nova::Key::Space}, .gamepadButtons = {nova::GamepadButton::A}});
/// nova::Input::bindAxis("Move", {.up = nova::Key::W, .down = nova::Key::S, .left = nova::Key::A,
///                                .right = nova::Key::D, .stick = nova::GamepadStick::Left});
///
/// for (u32 p = 0; p < nova::Input::playerCount(); ++p) {
///     if (nova::Input::isActionPressed("Jump", p)) jump(p);
///     move(p, nova::Input::axis("Move", p));
/// }
/// @endcode
///
/// **Gamepad values** follow the engine convention: sticks get a radial dead zone
/// (setStickDeadZone) and are rescaled so they still reach 1, with **+Y = up**; triggers go from
/// 0 (released) to 1 (pressed) with their own dead zone (setTriggerDeadZone).
/// Devices without a gamepad mapping (wheels, flight sticks) are read with joystickState().
class Input {
public:
    /// @brief Sets the window input is read from (done by Application).
    static void setWindow(platform::Window* window);
    /// @brief Samples keyboard, mouse and gamepads. Called once per frame by Application.
    static void update();

    /// @name Players
    /// @{
    static constexpr u32 kKeyboardMousePlayer = 0; ///< The player that uses keyboard + mouse.
    /// @brief 1 (keyboard + mouse) + number of connected gamepads.
    static u32 playerCount();
    /// @brief True for player 0 and for players whose gamepad is connected.
    static bool isPlayerConnected(u32 player);
    /// @brief Device driving `player`.
    static InputDevice playerDevice(u32 player);
    /// @brief "Keyboard & Mouse", the gamepad's name, or empty if not connected.
    static std::string playerDeviceName(u32 player);
    /// @brief Gamepad index of `player` (`player - 1`). @pre `player >= 1`.
    static u32 gamepadOfPlayer(u32 player) { return player - 1; }
    /// @}

    /// @name Actions and axes
    /// @{
    /// @brief Defines (or replaces) a named action.
    static void bindAction(const std::string& action, ActionBinding binding);
    /// @brief Defines (or replaces) a named 2D axis.
    static void bindAxis(const std::string& axis, AxisBinding binding);
    /// @brief Removes every action and axis binding.
    static void clearBindings();
    /// @brief True while any input bound to `action` is held by `player`. Unknown actions return false.
    static bool isActionDown(const std::string& action, u32 player = kKeyboardMousePlayer);
    /// @brief True only on the frame `action` became held by `player`.
    static bool isActionPressed(const std::string& action, u32 player = kKeyboardMousePlayer);
    /// @brief True only on the frame `action` stopped being held by `player`.
    static bool isActionReleased(const std::string& action, u32 player = kKeyboardMousePlayer);
    /// @brief Value of a named 2D axis for `player` (zero for unknown axes or disconnected players).
    static glm::vec2 axis(const std::string& axis, u32 player = kKeyboardMousePlayer);
    /// @}

    /// @name Keyboard and mouse (player 0)
    /// @{
    /// @brief True while `key` is held down.
    static bool isKeyDown(Key key);
    /// @brief True only on the frame `key` went down.
    static bool isKeyPressed(Key key);
    /// @brief True only on the frame `key` was released.
    static bool isKeyReleased(Key key);
    /// @brief True while `button` is held down.
    static bool isMouseButtonDown(MouseButton button);
    /// @brief True only on the frame `button` went down.
    static bool isMouseButtonPressed(MouseButton button);
    /// @brief Cursor position in window coordinates.
    static glm::vec2 mousePosition();
    /// @brief Cursor movement since the previous frame, in pixels, screen convention (+Y = down).
    ///        With the cursor captured, raw mouse counts where the platform has them.
    static glm::vec2 mouseDelta();
    /// @brief Hides and locks the cursor for mouse look (unbounded movement, raw counts without
    ///        the OS pointer acceleration where available), or gives it back. The jump the platform
    ///        makes when it moves the cursor is not reported as motion.
    static void setCursorCaptured(bool captured);
    /// @brief True while setCursorCaptured(true) holds.
    static bool cursorCaptured();
    /// @brief Wheel movement during the last frame (positive = away from the user).
    static f32 scrollDelta();
    /// @}

    /// @name Gamepads (indexed among connected gamepads; gamepad `g` is player `g + 1`)
    /// @{
    /// @brief Number of connected gamepads (devices with a standard mapping).
    static u32 gamepadCount();
    /// @brief True if gamepad `gamepad` is connected.
    static bool isGamepadConnected(u32 gamepad = 0);
    /// @brief Device name, or empty if not connected.
    static std::string gamepadName(u32 gamepad = 0);
    /// @brief True while `button` is held.
    static bool isGamepadButtonDown(GamepadButton button, u32 gamepad = 0);
    /// @brief True only on the frame `button` went down.
    static bool isGamepadButtonPressed(GamepadButton button, u32 gamepad = 0);
    /// @brief True only on the frame `button` was released.
    static bool isGamepadButtonReleased(GamepadButton button, u32 gamepad = 0);
    /// @brief Processed axis: sticks in [-1, 1] after the radial dead zone (+Y = up), triggers in [0, 1].
    static f32 gamepadAxis(GamepadAxis axis, u32 gamepad = 0);
    /// @brief Left stick after the radial dead zone; length ≤ 1, +Y = up.
    static glm::vec2 gamepadLeftStick(u32 gamepad = 0);
    /// @brief Right stick after the radial dead zone; length ≤ 1, +Y = up.
    static glm::vec2 gamepadRightStick(u32 gamepad = 0);
    /// @brief D-pad as a vector (components −1, 0 or 1; diagonals normalized), +Y = up.
    static glm::vec2 gamepadDpad(u32 gamepad = 0);
    /// @brief Unprocessed backend state (see GamepadState for its conventions). @return False if not connected.
    static bool gamepadRawState(GamepadState& out, u32 gamepad = 0);
    /// @brief Stick dead zone radius in [0, 1) (default 0.15).
    static void setStickDeadZone(f32 deadZone);
    /// @brief Trigger dead zone in [0, 1) (default 0.05).
    static void setTriggerDeadZone(f32 deadZone);
    /// @brief Adds gamepad mappings in SDL_GameControllerDB format (for controllers the built-in database misses).
    static bool addGamepadMappings(const char* mappings);
    /// @}

    /// @name Raw joysticks
    /// @{
    /// @brief Raw state of the joystick in backend slot `joystick` (any device, mapped or not).
    /// @return False if the slot is empty.
    static bool joystickState(u32 joystick, JoystickState& out);
    /// @brief Name of the device in backend slot `joystick`, or empty.
    static std::string joystickName(u32 joystick);
    /// @}

    /// @brief Radial dead zone: zero inside `deadZone`, then rescaled from 0 at the edge to 1 at
    ///        full tilt (output length clamped to 1).
    static glm::vec2 applyRadialDeadZone(glm::vec2 stick, f32 deadZone);
    /// @brief Maps a raw trigger (−1..1) to 0..1 and applies a dead zone with rescaling.
    static f32 applyTriggerDeadZone(f32 rawTrigger, f32 deadZone);

private:
    static NOVA_API platform::Window* s_window;
};

} // namespace nova
