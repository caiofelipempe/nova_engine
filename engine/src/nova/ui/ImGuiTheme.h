#pragma once

/// @file
/// @brief Dear ImGui look and feel.

/// @brief Engine UI helpers.
namespace nova::ui {

/// @brief Applies the engine's dark theme (rounding, spacing, colors) to the current ImGui context.
void applyTheme();

/// @brief Loads the UI font: Segoe UI 17 px on Windows when available, ImGui's default font otherwise.
void loadFonts();

} // namespace nova::ui
