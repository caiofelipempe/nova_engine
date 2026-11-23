#include "nova/renderer/WrapInstances.h"

#include <algorithm>
#include <cmath>

namespace nova {

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
            laps[a] = repeat;
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
                        out.push_back({i, offset, cell, false});
                    }
                }
    }
}

} // namespace nova
