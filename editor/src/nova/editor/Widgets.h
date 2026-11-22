#pragma once

/// @file
/// @brief Reusable ImGui widgets with the editor's look, for built-in and game panels.

#include "nova/math/Fixed.h"
#include "nova/math/GlobalPosition.h"
#include "nova/math/Rotation.h"
#include "nova/renderer/Mesh.h"

#include <glm/glm.hpp>

namespace nova::reflect {
struct TypeInfo;
struct FieldInfo;
} // namespace nova::reflect

namespace nova::editor {
class EditorContext;
}

namespace nova::editor::widgets {

/// @brief Draws widgets for every visible reflected field of `object` (the generated inspector).
///
/// Uses the Registry's field drawers when one matches (field-specific first, then by C++ type),
/// otherwise a widget chosen by reflect::FieldKind and FieldAttrs (sliders for bounded numbers,
/// color pickers, degrees, enum combos, mesh picker, nested structs as tree nodes).
/// @param type Reflected type of `object`.
/// @param object Pointer to the instance (component, script or struct).
/// @param ctx Editor context (field drawers, renderer for mesh lists).
/// @return True if any field changed.
bool reflected(const reflect::TypeInfo& type, void* object, EditorContext& ctx);

/// @brief Draws one reflected field (same rules as reflected()). @return True if it changed.
bool reflectedField(const reflect::TypeInfo& owner, const reflect::FieldInfo& field, void* value, EditorContext& ctx);

/// @brief Three colored X/Y/Z drag fields on one row with a label column.
/// @param label Row label (also the ImGui id).
/// @param value Edited vector.
/// @param speed Drag speed.
/// @param resetValue Value applied when a field is double-clicked.
/// @return True if the value changed.
bool vec3(const char* label, glm::vec3& value, float speed = 0.05f, float resetValue = 0.0f);

/// @brief Euler order the inspector shows rotations in (*View/Rotation Order*). Default YXZ.
extern EulerOrder rotationDisplayOrder;

/// @brief Rotation editor: Euler angles in degrees (rotationDisplayOrder) over a quaternion.
///
/// The angles typed are remembered while the quaternion does not change elsewhere, so a value
/// like 180° yaw does not flip to an equivalent triple after the round trip.
/// @return True if the rotation changed.
bool rotation(const char* label, glm::quat& value, float speed = 0.5f);
/// @brief Same, for a fixed-point rotation (quantized back when edited).
bool rotation(const char* label, FixedQuat& value, float speed = 0.5f);

/// @brief X/Y/Z editor for a world position, in meters (hover an axis for its readable unit and raw steps).
/// @return True if the value changed.
bool position(const char* label, GlobalPosition& value, float speed = 0.05f);

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
