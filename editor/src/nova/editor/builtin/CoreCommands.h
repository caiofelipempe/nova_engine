#pragma once

/// @file
/// @brief The editor's built-in commands (File, Edit, Entity, Play menus) and menu bar status.

#include "nova/editor/Registry.h"

#include <string>

namespace nova::editor::builtin {

/// @brief Registers the built-in commands.
///
/// | Id                  | Menu                      | Shortcut   |
/// |---------------------|---------------------------|------------|
/// | file.importGltf     | File/Import glTF...       | Ctrl+I     |
/// | file.exit           | File/Exit                 |            |
/// | edit.delete         | Edit/Delete               | Delete     |
/// | edit.focus          | Edit/Focus Selection      | F          |
/// | edit.deselect       | Edit/Deselect             | Escape     |
/// | entity.empty ...    | Entity/Create Empty, Cube, Sphere, Plane, Point Light | |
/// | play.play           | Play/Play (resume)        | F5         |
/// | play.pause          | Play/Pause                | F6         |
/// | play.stop           | Play/Stop                 | Shift+F5   |
///
/// Override any of them by registering a command with the same id.
class CoreCommandsPlugin final : public Plugin {
public:
    void onRegister(Registry& registry) override;

private:
    bool m_openImport = false;
    char m_importPath[512] = "";
};

/// @brief Creates an entity with a mesh in front of the editor camera and selects it.
/// @return The new entity.
Entity createMeshEntity(EditorContext& ctx, const std::string& name, MeshHandle mesh);

} // namespace nova::editor::builtin
