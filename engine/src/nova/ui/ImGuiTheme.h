#pragma once

namespace nova::ui {

void applyTheme();
// Loads a system UI font when available (Segoe UI on Windows), falling back to ImGui's default.
void loadFonts();

} // namespace nova::ui
