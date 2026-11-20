#pragma once

#include "nova/scene/Scene.h"

#include <filesystem>
#include <optional>

namespace nova {

class Renderer;

// Loads a glTF 2.0 file (.gltf / .glb) into the scene: one entity per mesh primitive with
// its baked world transform and PBR factors. Returns an (empty) marker entity named after the
// file, or nullopt on failure. Textures and a transform hierarchy are roadmap items.
std::optional<Entity> loadGltf(Scene& scene, Renderer& renderer, const std::filesystem::path& path);

} // namespace nova
