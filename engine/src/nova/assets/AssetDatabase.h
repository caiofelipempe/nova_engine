#pragma once

/// @file
/// @brief Project assets: stable GUIDs, registrable importers, cached loading and scene references.

#include "nova/core/Module.h"
#include "nova/scene/SceneSerializer.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nova {

class Renderer;
class AssetDatabase;

/// @brief Stable identity of an asset, stored in its `.meta` sidecar file.
using AssetGuid = u64;

/// @brief What loading an asset produced.
///
/// Meshes and textures are addressable sub-resources: scenes reference them as
/// `"asset:<guid>#<index>"`, so files survive renames and moves of the asset.
struct AssetData {
    std::vector<MeshHandle> meshes;           ///< Meshes, by sub-resource index.
    std::vector<rhi::TextureHandle> textures; ///< Textures, by sub-resource index.
    std::vector<ShaderHandle> shaders;        ///< Shaders, by sub-resource index.
    std::shared_ptr<void> payload;            ///< Importer-specific data (e.g. ModelData, prefab text).

    /// @brief The payload as `T` (the importer decides the type).
    template <typename T>
    T* as() const { return static_cast<T*>(payload.get()); }
};

/// @brief Teaches the database a kind of file.
///
/// @code
/// db.addImporter({
///     .type = "Dialogue",
///     .extensions = {".dlg.json"},
///     .load = [](nova::AssetDatabase&, const std::filesystem::path& file, nova::AssetData& out) {
///         out.payload = std::make_shared<Dialogue>(parseDialogue(file));
///         return true;
///     },
/// });
/// @endcode
struct AssetImporter {
    std::string type;                    ///< Asset type shown in the browser ("Texture", "Model"...).
    std::vector<std::string> extensions; ///< Lowercase file suffixes, compound ones allowed (".prefab.json").
    /// @brief Loads the file (null = the asset is listed but has nothing to load, e.g. scenes).
    std::function<bool(AssetDatabase& db, const std::filesystem::path& file, AssetData& out)> load;
    /// @brief Creates entities from the loaded asset at `position` (null = cannot be placed in a scene).
    std::function<std::vector<Entity>(AssetDatabase& db, const AssetData& data, Scene& scene, const GlobalPosition& position)>
        instantiate;
    /// @brief Other files the asset needs, copied along by AssetDatabase::import (e.g. glTF buffers).
    std::function<std::vector<std::filesystem::path>(const std::filesystem::path& file)> dependencies;
    ModuleId module = 0; ///< Set by addImporter() to currentModule().
};

/// @brief One known asset.
struct AssetRecord {
    AssetGuid guid = 0;                     ///< Stable identity.
    std::filesystem::path path;             ///< Path relative to the root (generic separators).
    std::string type;                       ///< Importer type.
    const AssetImporter* importer = nullptr; ///< Importer handling it.

    /// @brief File name without directories.
    std::string fileName() const { return path.filename().string(); }
};

/// @brief The project's asset folder.
///
/// refresh() scans the root: every file an importer recognizes becomes an asset and gets a
/// `<file>.meta` sidecar holding its GUID (commit those files with the assets). load() caches
/// results; serializeOptions() makes scenes reference assets by GUID.
///
/// Built-in importers: Texture (png, jpg, jpeg, tga, bmp), Model (gltf, glb), Shader (vert, frag,
/// comp and their `.glsl`/`.spv` forms; loaded through Renderer::shaders(), so they hot reload),
/// Prefab (`.prefab.json`) and Scene (`.scene.json`, listed only).
class AssetDatabase {
public:
    /// @param renderer Receives meshes/textures; may be null (headless: GPU assets fail to load).
    explicit AssetDatabase(Renderer* renderer = nullptr);
    ~AssetDatabase();
    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;

    /// @brief Sets the renderer used to create GPU resources.
    void setRenderer(Renderer* renderer) { m_renderer = renderer; }
    /// @brief The renderer (may be null).
    Renderer* renderer() const { return m_renderer; }

    /// @name Importers
    /// @{
    /// @brief Registers an importer; for a suffix claimed twice the newest wins.
    void addImporter(AssetImporter importer);
    /// @brief Removes the importers registered by `module` and forgets assets they loaded
    ///        (before unloading that module's code).
    void removeModule(ModuleId module);
    /// @brief The importer for a file (longest matching suffix), or `nullptr`.
    const AssetImporter* importerFor(const std::filesystem::path& file) const;
    /// @}

    /// @name Files
    /// @{
    /// @brief Sets the root folder (created if missing) and scans it.
    void setRoot(const std::filesystem::path& root);
    /// @brief The root folder (absolute).
    const std::filesystem::path& root() const { return m_root; }
    /// @brief Rescans the root: new files get a `.meta` (new GUID), removed files are dropped.
    void refresh();
    /// @brief Every asset, sorted by path.
    /// @warning refresh() (also called by import, createPrefab and addImporter) rebuilds the list:
    ///          keep GUIDs, not AssetRecord pointers, across those calls.
    const std::vector<AssetRecord>& assets() const { return m_records; }
    /// @brief Asset by GUID, or `nullptr`.
    const AssetRecord* find(AssetGuid guid) const;
    /// @brief Asset by path (absolute or relative to the root), or `nullptr`.
    const AssetRecord* findByPath(const std::filesystem::path& path) const;
    /// @brief Absolute path of an asset.
    std::filesystem::path absolutePath(const AssetRecord& record) const { return m_root / record.path; }
    /// @brief Copies an external file (and its dependencies) into `folder` (relative to the root) and registers it.
    ///        Files already inside the root are just registered.
    /// @return The new asset's GUID, or `std::nullopt` (unknown type, copy failure).
    std::optional<AssetGuid> import(const std::filesystem::path& file, const std::filesystem::path& folder = {},
                                    std::string* error = nullptr);
    /// @}

    /// @name Loading
    /// @{
    /// @brief Loads (once) and returns an asset's data, or `nullptr` if it cannot be loaded.
    const AssetData* load(AssetGuid guid);
    /// @brief True if load() already succeeded.
    bool isLoaded(AssetGuid guid) const { return m_loaded.contains(guid); }
    /// @brief Forgets the cached data (GPU resources stay alive; scenes may still use them).
    void unload(AssetGuid guid);
    /// @brief True if the asset's importer can place it in a scene.
    bool canInstantiate(const AssetRecord& record) const { return record.importer && record.importer->instantiate; }
    /// @brief Loads the asset and creates its entities at `position`; each gets a PrefabInstanceComponent.
    std::vector<Entity> instantiate(AssetGuid guid, Scene& scene, const GlobalPosition& position = GlobalPosition());
    /// @brief Finds the clips of model assets by reference ("asset:<guid>#<index>", what
    ///        instantiate() puts in a model's AnimatorComponent::clip), loading the model if needed.
    ///        Give it to Scene::setAnimationSource (the editor does) so saved scenes play them again.
    Scene::AnimationSource animationSource();
    /// @}

    /// @name References
    /// @{
    /// @brief `"asset:<16 hex digits>#<index>"`.
    static std::string makeRef(AssetGuid guid, u32 index = 0);
    /// @brief Parses makeRef() text. @return False if `ref` is not an asset reference.
    static bool parseRef(std::string_view ref, AssetGuid& guid, u32& index);
    /// @brief Reference of a mesh that came from an asset, or "".
    std::string meshRef(MeshHandle mesh) const;
    /// @brief Reference of a texture that came from an asset, or "".
    std::string textureRef(rhi::TextureHandle texture) const;
    /// @brief Mesh for a reference (loads the asset if needed); falls back to renderer mesh names.
    MeshHandle findMesh(std::string_view ref);
    /// @brief Texture for a reference (loads the asset if needed).
    rhi::TextureHandle findTexture(std::string_view ref);
    /// @brief Readable label for a texture: "textures/wood.png", "" if not from an asset.
    std::string textureLabel(rhi::TextureHandle texture) const;
    /// @brief Reference of a shader that came from an asset, or "".
    std::string shaderRef(ShaderHandle shader) const;
    /// @brief Shader for a reference (loads the asset if needed).
    ShaderHandle findShader(std::string_view ref);
    /// @brief Readable label for a shader: "shaders/water.frag", "" if not from an asset.
    std::string shaderLabel(ShaderHandle shader) const;
    /// @brief Serialization options for files: asset references, then renderer mesh names, then ids.
    SerializeOptions serializeOptions();
    /// @}

    /// @name Prefabs
    /// @{
    /// @brief Saves `entities` as a prefab file (relative to the root, `.prefab.json` appended if missing).
    /// @return The prefab's GUID, or `std::nullopt` on error.
    std::optional<AssetGuid> createPrefab(const std::vector<Entity>& entities, const std::filesystem::path& file,
                                          std::string* error = nullptr);
    /// @}

    /// @brief Creates a random GUID (never 0).
    static AssetGuid newGuid();

private:
    void registerBuiltinImporters();
    AssetRecord* findMutable(AssetGuid guid);
    std::filesystem::path relative(const std::filesystem::path& path) const;
    AssetGuid readOrCreateMeta(const std::filesystem::path& file, const AssetImporter& importer);

    Renderer* m_renderer = nullptr;
    std::filesystem::path m_root;
    std::deque<AssetImporter> m_importers; // deque: AssetRecord::importer pointers stay valid
    std::vector<AssetRecord> m_records;
    std::unordered_map<AssetGuid, AssetData> m_loaded;
    std::unordered_set<AssetGuid> m_failed; // load errors are reported once
    std::unordered_map<u32, std::pair<AssetGuid, u32>> m_meshOwners;    // mesh id → (asset, index)
    std::unordered_map<u64, std::pair<AssetGuid, u32>> m_textureOwners; // texture key → (asset, index)
    std::unordered_map<u32, std::pair<AssetGuid, u32>> m_shaderOwners;  // shader id → (asset, index)
};

} // namespace nova
