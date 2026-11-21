#pragma once

/// @file
/// @brief glTF 2.0 model loading (meshes, materials, base color textures) and instancing.

#include "nova/scene/Scene.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace nova {

class Renderer;

/// @brief One mesh instance of a model: a glTF node × primitive, with its baked world transform.
struct ModelNode {
    std::string name;                ///< Node name (or "node<N>").
    glm::vec3 position{0.0f};        ///< World position inside the model.
    glm::vec3 rotation{0.0f};        ///< Euler angles (radians, XYZ).
    glm::vec3 scale{1.0f};           ///< Scale.
    u32 mesh = 0;                    ///< Index into ModelData::meshes.
    Material material;               ///< Material (textures already resolved).
};

/// @brief A loaded model: GPU meshes and textures plus the instances that place them.
///
/// Meshes shared by several nodes are uploaded once. The transform hierarchy is flattened.
struct ModelData {
    std::vector<MeshHandle> meshes;           ///< One per (glTF mesh, primitive) actually used.
    std::vector<rhi::TextureHandle> textures; ///< One per glTF image (invalid if it failed to load).
    std::vector<ModelNode> nodes;             ///< Instances to create.
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

/// @brief Creates one entity per model node, offset by `position`.
/// @return The created entities (first = first node).
std::vector<Entity> instantiateModel(Scene& scene, const ModelData& model, const GlobalPosition& position = GlobalPosition());

/// @brief Files a glTF depends on (external `.bin` buffers and images), resolved next to `path`.
std::vector<std::filesystem::path> gltfDependencies(const std::filesystem::path& path);

/// @brief Loads a model and instantiates it at the origin (loadModel + instantiateModel).
/// @return The first created entity, or `std::nullopt` on failure.
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
