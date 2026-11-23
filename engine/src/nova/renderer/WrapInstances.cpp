#include "nova/renderer/WrapInstances.h"

#include <algorithm>
#include <cmath>

namespace nova {

namespace {

// Split turning with the camera: the copies of draw `i` (bounding sphere `center`, `radius`, from
// the camera) that reach the lap of `cell`, a slab along options.slab.axis. Across the slab, the
// copies one lap over where the object crosses the lap's faces; along it, every lap where the copy
// reaches the slab (|normal · q| within its half thickness). The copy holding the object's center
// casts the ray-traced shadow.
void splitSlab(u32 i, const glm::vec3& center, f32 radius, const glm::vec3& cell, const glm::bvec3& across,
               const WrapOptions& options, std::vector<WrapInstance>& out) {
    const WrapSlab& slab = options.slab;
    const int k = slab.axis;
    const glm::vec3 size = options.size;
    const f32 along = slab.normal[k] * size[k]; // normal · (one lap along the slab's axis)
    glm::ivec3 sideLaps(0);
    for (int a = 0; a < 3; ++a)
        if (a != k && size[a] > 0.0f && across[a]) sideLaps[a] = 1;
    for (int sx = -sideLaps.x; sx <= sideLaps.x; ++sx)
        for (int sy = -sideLaps.y; sy <= sideLaps.y; ++sy)
            for (int sz = -sideLaps.z; sz <= sideLaps.z; ++sz) {
                const glm::vec3 side = glm::vec3(sx, sy, sz) * size;
                const f32 s = glm::dot(slab.normal, center + side);
                // Laps t along the slab's axis with |s + along·t| within half the slab plus the radius.
                const f32 reach = slab.half + radius;
                f32 lo = (-reach - s) / along, hi = (reach - s) / along;
                if (lo > hi) std::swap(lo, hi);
                const int first = std::max(static_cast<int>(std::ceil(lo)), -4), last = std::min(static_cast<int>(std::floor(hi)), 4);
                const int home = static_cast<int>(std::lround(-s / along)); // the lap holding the center
                for (int t = first; t <= last; ++t) {
                    if (out.size() >= options.maxInstances) return;
                    glm::vec3 offset = cell + side;
                    offset[k] += static_cast<f32>(t) * size[k];
                    out.push_back({i, offset, cell, false, !(t == home && side == glm::vec3(0.0f))});
                }
            }
}

} // namespace

void expandWrapInstances(const std::vector<DrawItem>& draws, const MeshBoundsLookup& bounds,
                         const glm::mat4& viewProj, const WrapOptions& options, std::vector<WrapInstance>& out) {
    out.clear();
    const u32 count = static_cast<u32>(draws.size());
    const glm::vec3 size = options.size;
    const bool circular = size.x > 0.0f || size.y > 0.0f || size.z > 0.0f;
    const int repeat = circular ? static_cast<int>(std::min<u32>(options.repeat, 16)) : 0;
    const bool simple = !circular || (options.mode == WrapShift::Object && repeat == 0);

    // Frustum planes (Gribb–Hartmann, depth 0..1): a sphere is outside when it is behind one.
    glm::vec4 planes[6];
    const glm::mat4 t = glm::transpose(viewProj);
    planes[0] = t[3] + t[0], planes[1] = t[3] - t[0], planes[2] = t[3] + t[1];
    planes[3] = t[3] - t[1], planes[4] = t[2], planes[5] = t[3] - t[2];
    for (glm::vec4& p : planes) {
        const f32 l = glm::length(glm::vec3(p));
        if (l > 0.0f) p /= l;
    }
    auto visible = [&](const glm::vec3& c, f32 r) {
        for (const glm::vec4& p : planes)
            if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
        return true;
    };

    for (u32 i = 0; i < count && out.size() < options.maxInstances; ++i) {
        const DrawItem& d = draws[i];
        const Bounds* b = bounds(d.mesh);
        if (!b) continue;
        if (simple) { // each object once, at its copy nearest the camera
            out.push_back({i, glm::vec3(0.0f), glm::vec3(0.0f), false});
            continue;
        }
        if (options.mode == WrapShift::Crop) {
            // Once, clipped at the edges of one lap by the vertex shader. Around the camera: as it
            // is. The world's lap: each object at its copy whose origin is in that lap (where its
            // transform says), cut where it sticks out past a seam.
            glm::vec3 offset(0.0f), cell(0.0f);
            if (options.crop == WrapCrop::World)
                for (int a = 0; a < 3; ++a) {
                    if (size[a] <= 0.0f) continue;
                    cell[a] = options.worldCenter[a];
                    offset[a] = -std::floor((d.transform[3][a] - cell[a]) / size[a] + 0.5f) * size[a];
                }
            out.push_back({i, offset, cell, false});
            continue;
        }
        const glm::vec3 center(d.transform * glm::vec4(b->center(), 1.0f));
        const f32 scale = std::max({glm::length(glm::vec3(d.transform[0])), glm::length(glm::vec3(d.transform[1])),
                                    glm::length(glm::vec3(d.transform[2]))});
        const f32 radius = glm::length(b->extents()) * scale;
        // Axes on which the bounding sphere crosses an edge of the camera's lap.
        glm::bvec3 across(false);
        glm::ivec3 laps(0);
        for (int a = 0; a < 3; ++a) {
            if (size[a] <= 0.0f) continue;
            across[a] = std::abs(center[a]) + radius > size[a] * 0.5f;
            laps[a] = options.repeatAxes[a] ? repeat : 0;
        }
        const bool crosses = glm::any(across);
        // A sphere around a copy and its split parts, for culling the further laps.
        const f32 reach = radius + (crosses ? glm::length(size) : 0.0f);
        for (int kx = -laps.x; kx <= laps.x; ++kx)
            for (int ky = -laps.y; ky <= laps.y; ++ky)
                for (int kz = -laps.z; kz <= laps.z; ++kz) {
                    const glm::vec3 cell = glm::vec3(kx, ky, kz) * size;
                    const bool central = kx == 0 && ky == 0 && kz == 0;
                    if (!central && !visible(center + cell, reach)) continue;
                    if (out.size() >= options.maxInstances) return;
                    if (options.mode == WrapShift::Split && options.slab.axis >= 0) {
                        splitSlab(i, center, radius, cell, across, options, out);
                        continue;
                    }
                    out.push_back({i, cell, cell, options.mode == WrapShift::Triangle && crosses});
                    if (options.mode != WrapShift::Split || !crosses) continue;
                    // The copies one lap over on the crossed axes (all of them at a corner), each
                    // clipped at the edges of `cell` by the vertex shader.
                    for (u32 subset = 1; subset < 8; ++subset) {
                        glm::vec3 offset = cell;
                        bool ok = true;
                        for (int a = 0; a < 3; ++a) {
                            if (!(subset & (1u << a))) continue;
                            ok = ok && across[a];
                            offset[a] -= (center[a] > 0.0f ? 1.0f : -1.0f) * size[a];
                        }
                        if (!ok) continue;
                        if (out.size() >= options.maxInstances) return;
                        out.push_back({i, offset, cell, false, true});
                    }
                }
    }
}

// Half a side of the biggest box in the camera's axes that fits in the lap around the camera: on
// world axis i the box reaches h · Σ_j |view[i][j]| (column i of world → view), which must stay
// within half that lap. Axes that are not circular do not limit it.
f32 viewCropHalf(const glm::mat3& view, const glm::vec3& size, f32 grow) {
    f32 half = 1e30f, largest = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (size[i] <= 0.0f) continue;
        const f32 reach = std::abs(view[i][0]) + std::abs(view[i][1]) + std::abs(view[i][2]);
        half = std::min(half, size[i] * 0.5f / std::max(reach, 1e-6f));
        largest = std::max(largest, size[i] * 0.5f);
    }
    if (largest <= 0.0f) return half; // not circular: nothing to fit in
    return half + std::clamp(grow, 0.0f, 1.0f) * std::max(largest - half, 0.0f);
}

WrapSlab wrapSlab(const glm::mat3& view, const glm::vec3& size, int previousAxis) {
    const glm::vec3 forward = glm::normalize(glm::transpose(view) * glm::vec3(0.0f, 0.0f, -1.0f));
    int best = -1;
    for (int a = 0; a < 3; ++a)
        if (size[a] > 0.0f && (best < 0 || std::abs(forward[a]) > std::abs(forward[best]))) best = a;
    if (best < 0) return {};
    int axis = best;
    if (previousAxis >= 0 && previousAxis < 3 && size[previousAxis] > 0.0f &&
        std::abs(forward[previousAxis]) >= 0.9f * std::abs(forward[best]))
        axis = previousAxis; // a little slack: no flicker near 45°
    if (std::abs(forward[axis]) < 1e-4f) return {}; // looking square across every circular axis
    WrapSlab slab;
    slab.axis = axis;
    slab.normal = forward;
    slab.half = std::abs(forward[axis]) * size[axis] * 0.5f;
    return slab;
}

} // namespace nova
