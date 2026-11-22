#pragma once

/// @file
/// @brief Built-in viewport visualizations drawn with DebugDraw (selection, colliders, lights, grid).

#include "nova/editor/Registry.h"

namespace nova::editor::builtin {

/// @brief Toggles of the built-in visualizations (also in the *View* menu).
struct VisualSettings {
    bool selectionBounds = true; ///< Box around selected meshes (marker for entities without mesh).
    bool allColliders = false;   ///< Colliders of every rigid body (selected ones are always shown).
    bool lights = true;          ///< Sun direction, point light radius, camera frustum of selected entities.
    bool grid = false;           ///< Ground grid at y = 0.
    bool worldBounds = true;     ///< World edges near the camera: barriers (red), wrapped edges and torus seams (cyan).
};

/// @brief Registers the *View/Show ...* toggles and draws the visualizations every frame.
class CoreVisualsPlugin final : public Plugin {
public:
    void onRegister(Registry& registry) override;
    static NOVA_EDITOR_API VisualSettings settings; ///< Current toggles.
};

/// @brief Draws an entity's collider (Box/Sphere/Capsule, or the default box) as the physics sees it.
void drawCollider(DebugDraw& draw, Entity entity, const glm::vec4& color);

} // namespace nova::editor::builtin
