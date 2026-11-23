#pragma once

/// @file
/// @brief glTF 2.0 model loading (meshes, materials, base color textures) and instancing.

#include "nova/scene/Scene.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nova {

class Renderer;

/// @brief One node of a model: a glTF node (a transform, maybe with a mesh), or one more primitive
///        of a node's mesh (a child of that node, at the same pose).
struct ModelNode {
    std::string name;                ///< Node name (or "node<N>"; "<name>#<primitive>" for extra primitives).
    glm::vec3 position{0.0f};        ///< Position inside the model (the node's whole transform baked).
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; ///< Rotation inside the model (glTF stores quaternions).
    glm::vec3 scale{1.0f};           ///< Scale inside the model.
    u32 mesh = ~0u;                  ///< Index into ModelData::meshes; ~0u: none (a transform only).
    Material material;               ///< Material (textures already resolved).
    u32 parent = ~0u;                ///< Index of the parent in ModelData::nodes (always earlier); ~0u: top level.
};

/// @brief A loaded model: GPU meshes and textures plus the nodes that place them.
///
/// Meshes shared by several nodes are uploaded once. The node hierarchy is kept (parents before
/// children); each node also carries its pose inside the whole model.
struct ModelData {
    std::string name;                         ///< The file's stem (the root entity's name).
    std::vector<MeshHandle> meshes;           ///< One per (glTF mesh, primitive) actually used.
    std::vector<rhi::TextureHandle> textures; ///< One per glTF image (invalid if it failed to load).
    std::vector<ModelNode> nodes;             ///< Nodes to create, parents first.
    /// @brief The file's animations; channel targets are paths of node names from the model's root.
    std::vector<std::shared_ptr<const AnimationClip>> animations;
};

/// @brief Loads a glTF 2.0 file (`.gltf` or `.glb`) into GPU meshes and textures.
///
/// Reads positions, normals (generated if missing), UVs, metallic-roughness factors, emission
/// and the base color texture (embedded in a `.glb` or an external image file).
/// @param renderer Renderer that receives meshes and textures.
/// @param path File to load (external buffers/images are resolved relative to it).
/// @param meshPrefix Prefix for mesh names (default: the file stem + "/").
/// @return The model, or `std::nullopt` on failure (logged).
std::optional<ModelData> loadModel(Renderer& renderer, const std::filesystem::path& path,
                                   const std::string& meshPrefix = {});
/// @brief Same; without a renderer (null), only the nodes and animations are read (no meshes nor
///        textures: headless tools and tests).
std::optional<ModelData> loadModel(Renderer* renderer, const std::filesystem::path& path,
                                   const std::string& meshPrefix = {});

/// @brief Creates the model: a root entity at `position` (named after the model) and one entity per
///        node under it, children under their parents (ParentComponent): moving the root moves the
///        whole model, each part keeping its place in it. A model with animations gets an
///        AnimatorComponent on its root, playing the first one.
/// @return The created entities: the root first, then the nodes in order.
std::vector<Entity> instantiateModel(Scene& scene, const ModelData& model, const GlobalPosition& position = GlobalPosition());

/// @brief Files a glTF depends on (external `.bin` buffers and images), resolved next to `path`.
std::vector<std::filesystem::path> gltfDependencies(const std::filesystem::path& path);

/// @brief Loads a model and instantiates it at the origin (loadModel + instantiateModel).
/// @return The model's root entity, or `std::nullopt` on failure.
/// @note Meshes created this way are not tracked by an AssetDatabase: prefer
///       AssetDatabase::instantiate() when scenes will be saved.
std::optional<Entity> loadGltf(Scene& scene, Renderer& renderer, const std::filesystem::path& path);

/// @brief Decodes an image file (PNG, JPEG, TGA, BMP...) to RGBA8.
/// @param[out] width Width in pixels.
/// @param[out] height Height in pixels.
/// @return Pixels (`width * height * 4` bytes), empty on failure.
std::vector<u8> decodeImageFile(const std::filesystem::path& path, u32& width, u32& height);

/// @brief Decodes an encoded image held in memory to RGBA8 (see decodeImageFile).
std::vector<u8> decodeImageMemory(const void* data, size_t size, u32& width, u32& height);

} // namespace nova
