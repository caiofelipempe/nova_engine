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
    const bool turn = circular && options.turn;
    const bool simple = !circular || (options.mode == WrapShift::Object && repeat == 0 && !turn);
    // Turned: a sphere around the camera inside its turned lap (the turned distance of a lattice
    // step is at least L/sqrt(3)), and one around the turned lap (its points are within sqrt(3)
    // times half the lap's diagonal).
    f32 smallest = 1e30f, diagonal = 0.0f;
    for (int a = 0; a < 3; ++a)
        if (size[a] > 0.0f) smallest = std::min(smallest, size[a]), diagonal += size[a] * size[a];
    const f32 inner = smallest / (2.0f * std::sqrt(3.0f)), outer = std::sqrt(3.0f) * 0.5f * std::sqrt(diagonal);

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
            if (turn && options.crop == WrapCrop::Camera) // the copy whose origin is in the turned lap
                offset = turnedLapShift(glm::vec3(d.transform[3]), size, options.view);
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
        bool crosses = glm::any(across);
        // Turned: per object, the copy whose origin is nearest in the camera's axes; per triangle
        // and split, what may reach past the sphere inside the turned lap.
        glm::vec3 own(0.0f);
        if (turn) {
            crosses = glm::length(center) + radius > inner;
            if (options.mode == WrapShift::Object) own = turnedLapShift(glm::vec3(d.transform[3]), size, options.view);
        }
        // A sphere around a copy and its split parts, for culling the further laps.
        const f32 reach = radius + (crosses ? glm::length(size) : 0.0f) + glm::length(own);
        for (int kx = -laps.x; kx <= laps.x; ++kx)
            for (int ky = -laps.y; ky <= laps.y; ++ky)
                for (int kz = -laps.z; kz <= laps.z; ++kz) {
                    const glm::vec3 cell = glm::vec3(kx, ky, kz) * size;
                    const bool central = kx == 0 && ky == 0 && kz == 0;
                    if (!central && !visible(center + cell, reach)) continue;
                    if (out.size() >= options.maxInstances) return;
                    if (turn && options.mode == WrapShift::Split && crosses) {
                        // Every copy one lap over that may reach the turned lap: the fragment
                        // shader keeps what is in it (FRAME_FLAG_TURN_CUT).
                        for (int sx = -1; sx <= 1; ++sx)
                            for (int sy = -1; sy <= 1; ++sy)
                                for (int sz = -1; sz <= 1; ++sz) {
                                    if ((sx && size.x <= 0.0f) || (sy && size.y <= 0.0f) || (sz && size.z <= 0.0f)) continue;
                                    const glm::vec3 lap = glm::vec3(sx, sy, sz) * size;
                                    if (glm::length(center + lap) - radius >= outer) continue;
                                    if (out.size() >= options.maxInstances) return;
                                    out.push_back({i, cell + lap, cell, false, lap != glm::vec3(0.0f)});
                                }
                        continue;
                    }
                    out.push_back({i, cell + own, cell, options.mode == WrapShift::Triangle && crosses});
                    if (options.mode != WrapShift::Split || !crosses || turn) continue;
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
f32 viewCropHalf(const glm::mat3& view, const glm::vec3& size) {
    f32 half = 1e30f;
    for (int i = 0; i < 3; ++i) {
        if (size[i] <= 0.0f) continue;
        const f32 reach = std::abs(view[i][0]) + std::abs(view[i][1]) + std::abs(view[i][2]);
        half = std::min(half, size[i] * 0.5f / std::max(reach, 1e-6f));
    }
    return half;
}

glm::vec3 turnedLapShift(const glm::vec3& d, const glm::vec3& size, const glm::mat3& view) {
    auto norm = [&](const glm::vec3& v) {
        const glm::vec3 a = glm::abs(view * v);
        return std::max(a.x, std::max(a.y, a.z));
    };
    glm::vec3 base(0.0f);
    for (int a = 0; a < 3; ++a)
        if (size[a] > 0.0f) base[a] = -std::floor(d[a] / size[a] + 0.5f) * size[a];
    glm::vec3 best = base;
    f32 bestNorm = norm(d + base);
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j)
            for (int k = -1; k <= 1; ++k) {
                if ((i && size.x <= 0.0f) || (j && size.y <= 0.0f) || (k && size.z <= 0.0f)) continue;
                const glm::vec3 shift = base + glm::vec3(i, j, k) * size;
                const f32 n = norm(d + shift);
                if (n < bestNorm) best = shift, bestNorm = n;
            }
    return best;
}

} // namespace nova
