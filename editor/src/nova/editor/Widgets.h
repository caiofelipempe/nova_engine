#pragma once

/// @file
/// @brief Reusable ImGui widgets with the editor's look, for built-in and game panels.

#include "nova/renderer/Mesh.h"

#include <glm/glm.hpp>

namespace nova::editor::widgets {

/// @brief Three colored X/Y/Z drag fields on one row with a label column.
/// @param label Row label (also the ImGui id).
/// @param value Edited vector.
/// @param speed Drag speed.
/// @param resetValue Value applied when a field is double-clicked.
/// @return True if the value changed.
bool vec3(const char* label, glm::vec3& value, float speed = 0.05f, float resetValue = 0.0f);

/// @brief Editor for a Material (base color, metallic, roughness, emissive, checker).
/// @return True if anything changed.
bool material(Material& material);

/// @brief Collapsing section with an optional "×" remove button on the right.
/// @param title Header text (also the ImGui id).
/// @param removable Show the remove button.
/// @param[out] removeRequested Set to true when the remove button is clicked.
/// @param defaultOpen Start expanded.
/// @return True if the section is open (draw its contents, then nothing else needs closing).
bool componentHeader(const char* title, bool removable, bool& removeRequested, bool defaultOpen = true);

} // namespace nova::editor::widgets
