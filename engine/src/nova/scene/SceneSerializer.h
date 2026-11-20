#pragma once

/// @file
/// @brief Scene ↔ JSON, driven by reflection.

#include "nova/scene/Scene.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace nova {

class Renderer;

/// @brief How mesh references are written/read.
///
/// By default meshes are stored by numeric id, which is exact within one session (editor
/// Play/Stop). For files use fromRenderer(), which stores mesh **names** ("Cube").
struct SerializeOptions {
    std::function<std::string(MeshHandle)> meshName;            ///< Mesh → name (null = write ids).
    std::function<MeshHandle(const std::string&)> findMesh;     ///< Name → mesh when reading names.

    /// @brief Options that store meshes by name using `renderer`.
    static SerializeOptions fromRenderer(Renderer& renderer);
};

/// @brief Writes every entity's reflected components and reflected scripts as JSON text.
///
/// Format (`"format": "nova.scene", "version": 1`):
/// @code{.json}
/// { "format": "nova.scene", "version": 1, "name": "Level 1",
///   "entities": [ { "components": { "Id": {"uuid": 123}, "Name": {"name": "Crate"},
///                                   "Transform": {"position": [0, 1, 0], ...} },
///                   "scripts": [ { "type": "Spinner", "fields": {"speed": 2.0} } ] } ] }
/// @endcode
/// Components/scripts that are not reflected, and `transient` fields, are skipped.
std::string serializeScene(Scene& scene, const SerializeOptions& options = {});

/// @brief Replaces the scene's entities with the ones in `json`.
///
/// Unknown components/scripts are skipped with a warning; missing fields keep their defaults
/// (files stay loadable as types evolve). The scene is cleared first.
/// @param[out] error Reason on failure (malformed JSON, wrong format).
/// @return False if the text is not a valid scene (the scene is left empty in that case).
bool deserializeScene(Scene& scene, std::string_view json, const SerializeOptions& options = {},
                      std::string* error = nullptr);

/// @brief serializeScene() to a file. @return False if the file cannot be written.
bool saveScene(Scene& scene, const std::filesystem::path& path, const SerializeOptions& options = {},
               std::string* error = nullptr);

/// @brief deserializeScene() from a file; the scene is renamed after the file (see sceneNameFromPath).
bool loadScene(Scene& scene, const std::filesystem::path& path, const SerializeOptions& options = {},
               std::string* error = nullptr);

/// @brief Scene name for a file: `levels/forest.scene.json` → `forest`.
std::string sceneNameFromPath(const std::filesystem::path& path);

/// @brief Copies `entity` (reflected components and scripts) into a new entity with a new UUID.
/// @return The copy (same scene).
Entity duplicateEntity(Entity entity);

} // namespace nova
