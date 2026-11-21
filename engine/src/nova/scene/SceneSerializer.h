#pragma once

/// @file
/// @brief Scene ↔ JSON, driven by reflection.

#include "nova/scene/Scene.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace nova {

class Renderer;

/// @brief How mesh references are written/read.
///
/// By default meshes and textures are stored by numeric id, which is exact within one session
/// (editor Play/Stop). For files use AssetDatabase::serializeOptions() (asset references, e.g.
/// "asset:3f2a...#0") or fromRenderer() (mesh **names**, e.g. "Cube"). A callback returning an
/// empty string falls back to the id.
struct SerializeOptions {
    std::function<std::string(MeshHandle)> meshName;            ///< Mesh → name (null = write ids).
    std::function<MeshHandle(const std::string&)> findMesh;     ///< Name → mesh when reading names.
    std::function<std::string(rhi::TextureHandle)> textureName; ///< Texture → reference string (null = write ids).
    std::function<rhi::TextureHandle(const std::string&)> findTexture; ///< Reference string → texture.
    std::function<std::string(ShaderHandle)> shaderName;                ///< Shader → reference string (null = write ids).
    std::function<ShaderHandle(const std::string&)> findShader;         ///< Reference string → shader.

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

/// @brief One entity (reflected components and scripts) as a JSON object string.
std::string serializeEntity(Entity entity, const SerializeOptions& options = {});

/// @brief Applies an entity produced by serializeEntity().
///
/// If an entity with the same UUID exists, its reflected components and scripts are replaced
/// **in place** (its handle and position in the scene stay the same); otherwise it is created.
/// @return The restored entity, or an invalid Entity if the JSON is malformed.
Entity restoreEntity(Scene& scene, std::string_view json, const SerializeOptions& options = {});

/// @brief Writes entities as a prefab (`"format": "nova.prefab"`): same entity format as scenes,
///        without UUIDs, positions relative to the first entity.
std::string serializePrefab(const std::vector<Entity>& entities, const SerializeOptions& options = {});

/// @brief Creates the entities of a prefab (new UUIDs), with the first one at `position`.
/// @return The created entities (empty, with `error` set, if the text is not a prefab).
std::vector<Entity> instantiatePrefab(Scene& scene, std::string_view json, const glm::vec3& position = glm::vec3(0.0f),
                                      const SerializeOptions& options = {}, std::string* error = nullptr);

/// @brief Scene name for a file: `levels/forest.scene.json` → `forest`.
std::string sceneNameFromPath(const std::filesystem::path& path);

/// @brief Copies `entity` (reflected components and scripts) into a new entity with a new UUID.
/// @return The copy (same scene).
Entity duplicateEntity(Entity entity);

} // namespace nova
