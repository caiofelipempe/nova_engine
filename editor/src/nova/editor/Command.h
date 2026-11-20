#pragma once

/// @file
/// @brief Editor commands (menu items, keyboard shortcuts, command palette entries).

#include "nova/core/Module.h"
#include "nova/platform/KeyCodes.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace nova::editor {

class EditorContext;

/// @brief A key plus modifiers, e.g. `Ctrl+Shift+S`.
struct KeyChord {
    Key key = Key::Unknown; ///< Main key; Unknown = no shortcut.
    bool ctrl = false;      ///< Either Ctrl key.
    bool shift = false;     ///< Either Shift key.
    bool alt = false;       ///< Either Alt key.

    /// @brief Parses text such as `"Ctrl+S"`, `"Shift+F5"`, `"Delete"`, `"W"` (case-insensitive).
    ///
    /// Key names: `A`–`Z`, `0`–`9`, `F1`–`F12`, `Space`, `Enter`, `Escape`/`Esc`, `Tab`,
    /// `Backspace`, `Delete`/`Del`, `Insert`, `Home`, `End`, `PageUp`, `PageDown`, `Up`, `Down`,
    /// `Left`, `Right`, `Minus`, `Equal`, `Comma`, `Period`, `Slash`. Modifiers: `Ctrl`/`Control`,
    /// `Shift`, `Alt`.
    /// @return The chord, or `std::nullopt` for empty or invalid text.
    static std::optional<KeyChord> parse(std::string_view text);
    /// @brief Canonical text, e.g. `"Ctrl+Shift+S"` (empty when invalid).
    std::string toString() const;
    /// @brief True when a key is set.
    bool valid() const { return key != Key::Unknown; }
    bool operator==(const KeyChord&) const = default;
};

/// @brief An action the user can trigger from a menu, a shortcut or the command palette.
///
/// Usually registered through Registry::command:
/// @code
/// registry.command("Tools/Bake Navmesh", [](EditorContext& ctx) { bake(ctx.scene()); }, "Ctrl+B");
/// @endcode
struct Command {
    std::string id;       ///< Unique id. Registering the same id again replaces the command.
    std::string menuPath; ///< Menu location, `/`-separated (`"File/Import glTF..."`); empty = palette only.
    KeyChord shortcut;    ///< Optional keyboard shortcut (ignored while typing in a text field or flying the camera).
    std::function<void(EditorContext&)> execute;   ///< What the command does (required).
    std::function<bool(EditorContext&)> isEnabled; ///< Optional: greyed out / not triggerable when false.
    std::function<bool(EditorContext&)> isChecked; ///< Optional: shows a check mark in menus (toggles).
    int order = 0;                ///< Sort key inside its menu (lower first; ties keep registration order).
    bool separatorBefore = false; ///< Draw a separator above the menu item.
    bool showInPalette = true;    ///< List in the command palette.
    ModuleId module = 0;          ///< Code module that registered it (set by Registry::command).

    /// @brief True if it has no enable predicate or the predicate returns true.
    bool enabled(EditorContext& ctx) const { return !isEnabled || isEnabled(ctx); }
};

} // namespace nova::editor
