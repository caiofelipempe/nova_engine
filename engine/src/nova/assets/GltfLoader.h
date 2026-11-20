#pragma once

/// @file
/// @brief glTF 2.0 scene import.

#include "nova/scene/Scene.h"

#include <filesystem>
#include <optional>

namespace nova {

class Renderer;

/// @brief Imports a glTF 2.0 file (`.gltf` or `.glb`) into a scene.
///
/// Creates one entity per mesh primitive, with its node's world transform baked into the
/// TransformComponent and its metallic-roughness factors in the MeshComponent's Material.
/// Meshes shared by several nodes are uploaded once. Primitives without positions or that are
/// not triangle lists are skipped; missing normals are generated.
///
/// @param scene Scene that receives the entities.
/// @param renderer Renderer that receives the meshes.
/// @param path File to load (external buffers are resolved relative to it).
/// @return An empty marker entity named after the file, or `std::nullopt` on failure (logged).
/// @note Not imported yet: textures, transform hierarchy, cameras, lights, animation.
std::optional<Entity> loadGltf(Scene& scene, Renderer& renderer, const std::filesystem::path& path);

} // namespace nova
