#pragma once

/// @file
/// @brief Keyboard, mouse and gamepad identifiers, and joystick state structs.

#include "nova/core/Base.h"

#include <array>
#include <vector>

namespace nova {

/// @brief Keyboard keys (US layout positions).
///
/// Values intentionally match GLFW's so the GLFW backend is a plain cast; other window backends
/// translate their native codes into these.
enum class Key : i32 {
    Unknown = -1,
    Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
    D0 = 48, D1, D2, D3, D4, D5, D6, D7, D8, D9,
    Semicolon = 59, Equal = 61,
    A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    LeftBracket = 91, Backslash = 92, RightBracket = 93, GraveAccent = 96,
    Escape = 256, Enter, Tab, Backspace, Insert, Delete, Right, Left, Down, Up,
    PageUp, PageDown, Home, End,
    CapsLock = 280, ScrollLock, NumLock, PrintScreen, Pause,
    F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    LeftShift = 340, LeftControl, LeftAlt, LeftSuper, RightShift, RightControl, RightAlt, RightSuper,
    Count = 349 ///< Number of key slots (not a key).
};

/// @brief Mouse buttons.
enum class MouseButton : i32 {
    Left = 0,
    Right = 1,
    Middle = 2,
    Count = 8 ///< Number of button slots (not a button).
};

/// @brief Buttons of a standard gamepad (Xbox layout names; values match GLFW).
///
/// PlayStation equivalents: A = Cross, B = Circle, X = Square, Y = Triangle, Back = Share/Select,
/// Start = Options, Guide = PS button. Nintendo controllers are mapped by position (A = bottom).
enum class GamepadButton : i32 {
    A = 0,       ///< Bottom face button.
    B,           ///< Right face button.
    X,           ///< Left face button.
    Y,           ///< Top face button.
    LeftBumper,  ///< LB / L1.
    RightBumper, ///< RB / R1.
    Back,        ///< View / Share / Select.
    Start,       ///< Menu / Options / Start.
    Guide,       ///< Xbox / PS / Home button.
    LeftThumb,   ///< Left stick click (L3).
    RightThumb,  ///< Right stick click (R3).
    DpadUp,
    DpadRight,
    DpadDown,
    DpadLeft,
    Count ///< Number of buttons (not a button).
};

/// @brief Analog inputs of a standard gamepad (values match GLFW).
enum class GamepadAxis : i32 {
    LeftX = 0,    ///< Left stick horizontal.
    LeftY,        ///< Left stick vertical.
    RightX,       ///< Right stick horizontal.
    RightY,       ///< Right stick vertical.
    LeftTrigger,  ///< LT / L2.
    RightTrigger, ///< RT / R2.
    Count ///< Number of axes (not an axis).
};

/// @brief Maximum number of joysticks/gamepads tracked at once.
constexpr u32 kMaxJoysticks = 16;

/// @brief Raw state of a joystick recognized as a standard gamepad.
///
/// Values are as reported by the backend: axes in [-1, 1], stick Y **positive = down**, triggers
/// -1 (released) .. 1 (fully pressed), no dead zone. nova::Input exposes processed values.
struct GamepadState {
    std::array<bool, static_cast<size_t>(GamepadButton::Count)> buttons{}; ///< Pressed state per GamepadButton.
    std::array<f32, static_cast<size_t>(GamepadAxis::Count)> axes{};       ///< Value per GamepadAxis.

    /// @brief Pressed state of `b`.
    bool button(GamepadButton b) const { return buttons[static_cast<size_t>(b)]; }
    /// @brief Raw value of `a`.
    f32 axis(GamepadAxis a) const { return axes[static_cast<size_t>(a)]; }
};

/// @brief Bits of a joystick hat (POV switch) in JoystickState::hats.
enum JoystickHat : u8 {
    HatCentered = 0,
    HatUp = 1 << 0,
    HatRight = 1 << 1,
    HatDown = 1 << 2,
    HatLeft = 1 << 3,
};

/// @brief Raw state of any joystick, including devices without a gamepad mapping
///        (flight sticks, wheels, pedals, arcade panels).
struct JoystickState {
    std::vector<f32> axes;   ///< Axes in [-1, 1], in device order.
    std::vector<bool> buttons; ///< Buttons, in device order.
    std::vector<u8> hats;    ///< Hats as JoystickHat bit sets.
};

} // namespace nova
