#pragma once

/// @file
/// @brief The mesh instances a frame draws in the circular world: copies across a seam and in
///        further laps (see WrapShift, RenderSettings::wrapRepeat).

#include "nova/renderer/Renderer.h"

#include <functional>
#include <vector>

namespace nova {

/// @brief One mesh instance as drawn: DrawItem `draw` moved by `offset` (whole laps of the world),
///        in the lap centered at `cell` (camera-relative meters; zero for the camera's own lap).
struct WrapInstance {
    u32 draw = 0;              ///< Index into RenderScene::draws.
    glm::vec3 offset{0.0f};    ///< Added to the draw's translation.
    glm::vec3 cell{0.0f};      ///< Center of the lap the copy belongs to (the shader wraps / clips around it).
    bool triangles = false;    ///< The per-triangle path (WrapShift::Triangle across a seam).
    bool splitCopy = false;    ///< A split copy moved off its lap (left out of the ray-traced shadows: the original is there whole).
};

/// @brief How expandWrapInstances() copies the draws.
struct WrapOptions {
    glm::vec3 size{0.0f};               ///< World size per axis (m); 0 = not circular.
    WrapShift mode = WrapShift::Object; ///< Across a seam: per triangle, the whole object, or split.
    u32 repeat = 0;                     ///< Further laps on each side (circular axes), frustum culled.
    glm::bvec3 repeatAxes{true};        ///< The axes `repeat` applies to.
    WrapCrop crop = WrapCrop::World;    ///< WrapShift::Crop: the world's lap or the camera's.
    glm::vec3 worldCenter{0.0f};        ///< The world's center from the camera (RenderScene::worldCenter).
    u32 maxInstances = 16384;           ///< Stop there.
};

/// @brief Bounds of a mesh (nullptr: not drawable, skipped).
using MeshBoundsLookup = std::function<const Bounds*(MeshHandle)>;

/// @brief The instances to draw for `draws` (each already at its copy nearest the camera, by its
///        origin). With WrapShift::Object and no repeat, exactly one instance per drawable draw.
///        Otherwise, for objects whose bounding sphere crosses an edge of the camera's lap:
///        Triangle marks the instance for the per-triangle path, Split adds the copies one lap over
///        on the crossed axes (each clipped at the seam by the shader); `repeat` adds every object
///        again in the laps around, the ones outside the view frustum culled.
/// @param viewProj Camera-relative view-projection (depth 0..1), for culling the further laps.
void expandWrapInstances(const std::vector<DrawItem>& draws, const MeshBoundsLookup& bounds,
                         const glm::mat4& viewProj, const WrapOptions& options, std::vector<WrapInstance>& out);

/// @brief WrapCrop::View: half a side of the biggest box in the camera's own axes that fits in the lap
///        around the camera (no point of the world inside it twice). From L/(2√3), looking along a
///        diagonal, to L/2, looking along an axis; axes that are not circular do not limit it.
/// @param view World → view rotation of the camera (camera-relative: no translation).
/// @param size World size per axis (m); 0 = not circular.
f32 viewCropHalf(const glm::mat3& view, const glm::vec3& size);

} // namespace nova
