#include "nova/editor/Picking.h"

#include <cmath>

namespace nova::editor {

std::optional<PickHit> pick(Scene& scene, const MeshLookup& meshes, const GlobalPosition& origin,
                            const glm::vec3& direction, const PickOptions& options) {
    if (glm::dot(direction, direction) < 1e-20f) return std::nullopt;
    const glm::vec3 dir = glm::normalize(direction);
    std::optional<PickHit> best;
    f32 bestDistance = options.maxDistance;

    for (auto [handle, t] : scene.registry().view<TransformComponent>().each()) {
        Entity e{handle, &scene};
        if (options.ignore && options.ignore(e)) continue;

        const MeshComponent* mc = e.tryGet<MeshComponent>();
        const MeshData* mesh = (mc && mc->mesh && meshes) ? meshes(mc->mesh) : nullptr;
        if (mesh) {
            // Ray at the local origin, mesh placed relative to it.
            if (auto hit = raycastMesh(*mesh, t.matrixRelativeTo(origin), glm::vec3(0.0f), dir, bestDistance)) {
                bestDistance = hit->distance;
                best = PickHit{e, origin + hit->point, hit->normal, hit->distance};
            }
            continue;
        }
        if (options.markerRadius <= 0.0f) continue;
        // Ray vs marker sphere at the entity position.
        const glm::vec3 center = (t.position - origin).toVec3();
        const glm::vec3 oc = -center;
        const f32 b = glm::dot(oc, dir);
        const f32 c = glm::dot(oc, oc) - options.markerRadius * options.markerRadius;
        const f32 disc = b * b - c;
        if (disc < 0.0f) continue;
        f32 d = -b - std::sqrt(disc);
        if (d < 0.0f) d = -b + std::sqrt(disc);
        if (d < 0.0f || d >= bestDistance) continue;
        bestDistance = d;
        const glm::vec3 p = dir * d;
        best = PickHit{e, origin + p, glm::normalize(p - center), d};
    }
    return best;
}

} // namespace nova::editor
