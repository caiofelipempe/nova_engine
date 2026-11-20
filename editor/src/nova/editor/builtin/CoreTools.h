#pragma once

/// @file
/// @brief Built-in viewport tools: Move / Rotate / Scale gizmos.

#include "nova/editor/Registry.h"
#include "nova/editor/Tool.h"

namespace nova::editor::builtin {

/// @brief Transform gizmo (ImGuizmo) for the selection.
///
/// Manipulates the primary selected entity; the same world-space delta is applied to every
/// other selected entity. Hold Ctrl to snap (0.5 units / 15°). Edits while simulating teleport
/// physics bodies. Undo is automatic (one step per drag).
class TransformTool final : public Tool {
public:
    enum class Mode { Translate, Rotate, Scale };
    TransformTool(Mode mode, std::string name, std::string shortcut);

    void onViewport(EditorContext& ctx, const ViewportState& viewport) override;
    bool isInteracting() const override;

    Mode mode() const { return m_mode; }
    static NOVA_EDITOR_API bool localSpace; ///< Gizmo axes follow the entity (true) or the world.

private:
    Mode m_mode;
};

/// @brief Registers Move (W), Rotate (E), Scale (R) and the *View/Tools/Local Space* toggle.
class CoreToolsPlugin final : public Plugin {
public:
    void onRegister(Registry& registry) override;
};

} // namespace nova::editor::builtin
