#include "nova/editor/builtin/CoreVisuals.h"

#include "nova/core/Application.h"
#include "nova/editor/EditorContext.h"
#include "nova/editor/Widgets.h"
#include "nova/renderer/Renderer.h"

#include <algorithm>

namespace nova::editor::builtin {

VisualSettings CoreVisualsPlugin::settings;

namespace {
const glm::vec4 kSelection{1.0f, 0.6f, 0.1f, 1.0f};
const glm::vec4 kCollider{0.3f, 1.0f, 0.4f, 0.9f};
const glm::vec4 kColliderFaint{0.3f, 1.0f, 0.4f, 0.35f};
const glm::vec4 kLight{1.0f, 0.9f, 0.3f, 0.9f};
const glm::vec4 kCamera{0.7f, 0.8f, 1.0f, 0.9f};

void drawFrustum(DebugDraw& dd, const TransformComponent& t, const CameraComponent& c) {
    const f32 farDist = std::min(c.farClip, 4.0f), aspect = 16.0f / 9.0f;
    const f32 h = std::tan(c.fovY * 0.5f), w = h * aspect;
    const glm::quat q = t.orientation();
    auto corner = [&](f32 d, f32 sx, f32 sy) { return t.position + q * glm::vec3(sx * w * d, sy * h * d, -d); };
    const f32 n = c.nearClip;
    GlobalPosition nc[4] = {corner(n, -1, -1), corner(n, 1, -1), corner(n, 1, 1), corner(n, -1, 1)};
    GlobalPosition fc[4] = {corner(farDist, -1, -1), corner(farDist, 1, -1), corner(farDist, 1, 1), corner(farDist, -1, 1)};
    for (int i = 0; i < 4; ++i) {
        dd.line(nc[i], nc[(i + 1) % 4], kCamera);
        dd.line(fc[i], fc[(i + 1) % 4], kCamera);
        dd.line(nc[i], fc[i], kCamera);
    }
}

void drawLight(DebugDraw& dd, Entity e) {
    const auto& t = e.transform();
    if (auto* l = e.tryGet<PointLightComponent>()) dd.sphere(t.position, l->radius, kLight);
    if (e.has<DirectionalLightComponent>()) {
        const glm::vec3 dir = t.forward();
        for (const glm::vec3 offset : {glm::vec3(0.0f), t.right() * 0.3f, -t.right() * 0.3f})
            dd.arrow(t.position + offset, t.position + offset + dir * 2.0f, kLight, true);
    }
    if (auto* c = e.tryGet<CameraComponent>()) drawFrustum(dd, t, *c);
}
// The seams of the circular world near the camera (cyan, where +size/2 meets −size/2), as a
// world-anchored grid patch on each seam plane. In small worlds a seam can be in reach on both
// sides of the camera.
void drawWorldSeams(DebugDraw& dd, const GlobalPosition& camera, const WorldSize& size) {
    constexpr f64 kReach = 300.0; // draw seams closer than this to the camera
    constexpr f64 kHalf = 120.0;  // half size of the drawn patch
    constexpr f64 kStep = 10.0;   // grid spacing
    const glm::vec4 color(0.2f, 0.85f, 1.0f, 0.8f);
    const glm::dvec3 cam = camera.toMeters();
    for (int axis = 0; axis < 3; ++axis) {
        glm::i64vec3 raw = camera.raw();
        raw[axis] = -size.halfSteps(axis);
        const f64 seam = (GlobalPosition::fromRaw(raw) - camera).toMeters()[axis]; // the nearest, shortest way
        const f64 lap = size.size(axis);
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        const f64 uh = std::min(kHalf, size.size(u) * 0.5), vh = std::min(kHalf, size.size(v) * 0.5);
        for (f64 d = seam - std::floor((seam + kReach) / lap) * lap; d <= kReach; d += lap) {
            if (d < -kReach) continue;
            auto at = [&](f64 a, f64 b) {
                glm::vec3 o(0.0f);
                o[axis] = static_cast<f32>(d), o[u] = static_cast<f32>(a), o[v] = static_cast<f32>(b);
                return camera + o;
            };
            // Grid lines on world multiples of kStep (they do not slide with the camera).
            for (f64 x = std::ceil((cam[u] - uh) / kStep) * kStep - cam[u]; x <= uh; x += kStep)
                dd.line(at(x, -vh), at(x, vh), color);
            for (f64 y = std::ceil((cam[v] - vh) / kStep) * kStep - cam[v]; y <= vh; y += kStep)
                dd.line(at(-uh, y), at(uh, y), color);
        }
    }
}
} // namespace

void drawCollider(DebugDraw& dd, Entity e, const glm::vec4& color) {
    // Same sizing rules as Scene::createBody: sizes scaled by the transform, offset in local space.
    const auto& t = e.transform();
    const glm::vec3 s = glm::abs(t.scaleF());
    const glm::quat q = t.orientation();
    if (auto* box = e.tryGet<BoxColliderComponent>()) {
        dd.box(t.position + q * (box->offset.toGlm() * s), box->halfExtents.toGlm() * s, q, color);
    } else if (auto* sphere = e.tryGet<SphereColliderComponent>()) {
        dd.sphere(t.position + q * (sphere->offset.toGlm() * s), toFloat(sphere->radius) * std::max({s.x, s.y, s.z}), color);
    } else if (auto* cap = e.tryGet<CapsuleColliderComponent>()) {
        dd.capsule(t.position + q * (cap->offset.toGlm() * s), q, toFloat(cap->radius) * std::max(s.x, s.z),
                   toFloat(cap->halfHeight) * s.y, color);
    } else if (auto* cyl = e.tryGet<CylinderColliderComponent>()) {
        const GlobalPosition c = t.position + q * (cyl->offset.toGlm() * s);
        const f32 r = toFloat(cyl->radius) * std::max(s.x, s.z), h = toFloat(cyl->halfHeight) * s.y;
        const glm::vec3 up = q * glm::vec3(0, h, 0);
        dd.circle(c + up, q * glm::vec3(0, 1, 0), r, color);
        dd.circle(c - up, q * glm::vec3(0, 1, 0), r, color);
        for (const glm::vec3& side : {glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1)})
            dd.line(c + up + q * (side * r), c - up + q * (side * r), color);
    } else if (e.has<ConvexHullColliderComponent>() || e.has<MeshColliderComponent>()) {
        // The source mesh's edges (for a hull, an approximation from inside: the hull wraps them).
        const auto* hull = e.tryGet<ConvexHullColliderComponent>();
        MeshHandle mesh = hull ? hull->mesh : e.get<MeshColliderComponent>().mesh;
        if (!mesh)
            if (const auto* m = e.tryGet<MeshComponent>()) mesh = m->mesh;
        const MeshData* data = mesh ? Application::get().renderer().meshData(mesh) : nullptr;
        if (!data) return;
        const glm::vec3 shift = hull ? q * (hull->offset.toGlm() * s) : glm::vec3(0.0f);
        auto corner = [&](u32 i) { return t.position + (shift + q * (data->vertices[i].position * s)); };
        const size_t maxTriangles = 20000; // keep huge meshes affordable
        for (size_t i = 0; i + 2 < data->indices.size() && i / 3 < maxTriangles; i += 3) {
            const GlobalPosition a = corner(data->indices[i]), b = corner(data->indices[i + 1]), c = corner(data->indices[i + 2]);
            dd.line(a, b, color);
            dd.line(b, c, color);
            dd.line(c, a, color);
        }
    } else {
        dd.box(t.position, 0.5f * s, q, color);
    }
}

void CoreVisualsPlugin::onRegister(Registry& r) {
    auto toggle = [&r](const char* id, const char* menu, bool VisualSettings::*flag, int order) {
        Command c;
        c.id = id;
        c.menuPath = menu;
        c.order = order;
        c.execute = [flag](EditorContext&) { settings.*flag = !(settings.*flag); };
        c.isChecked = [flag](EditorContext&) { return settings.*flag; };
        return &r.command(std::move(c));
    };
    toggle("view.selectionBounds", "View/Show Selection Bounds", &VisualSettings::selectionBounds, 200)->separatorBefore = true;
    toggle("view.colliders", "View/Show All Colliders", &VisualSettings::allColliders, 201);
    toggle("view.lights", "View/Show Light and Camera Gizmos", &VisualSettings::lights, 202);
    toggle("view.grid", "View/Show Grid", &VisualSettings::grid, 203);
    toggle("view.worldBounds", "View/Show World Seams", &VisualSettings::worldBounds, 204);

    // How the inspector shows rotations (they are stored as quaternions).
    for (int i = 0; i < 6; ++i) {
        const auto order = static_cast<EulerOrder>(i);
        Command c;
        c.id = std::string("view.rotationOrder.") + eulerOrderName(order);
        c.menuPath = std::string("View/Rotation Order/") + eulerOrderName(order) +
                     (order == EulerOrder::YXZ ? " (yaw, pitch, roll)" : "");
        c.order = 210 + i;
        c.separatorBefore = i == 0;
        c.execute = [order](EditorContext&) { widgets::rotationDisplayOrder = order; };
        c.isChecked = [order](EditorContext&) { return widgets::rotationDisplayOrder == order; };
        r.command(std::move(c));
    }

    r.uiHook([](EditorContext& ctx) {
        DebugDraw& dd = ctx.renderer().debugDraw();
        Scene& scene = ctx.scene();
        if (settings.grid) dd.grid({0.0f, 0.002f, 0.0f}, 40.0f, 40, {0.45f, 0.45f, 0.5f, 0.6f});
        if (settings.worldBounds) drawWorldSeams(dd, ctx.camera().position, scene.worldSize());

        if (settings.allColliders)
            for (auto [h, rb] : scene.registry().view<RigidBodyComponent>().each())
                if (!ctx.isSelected({h, &scene})) drawCollider(dd, {h, &scene}, kColliderFaint);

        Renderer& renderer = ctx.renderer();
        for (Entity e : ctx.selection()) {
            if (!e.valid()) continue;
            if (settings.selectionBounds) {
                if (auto* mc = e.tryGet<MeshComponent>(); mc && mc->mesh) {
                    const Bounds b = renderer.meshBounds(mc->mesh);
                    const auto& t = e.transform();
                    dd.box(t.position, t.matrixRelativeTo(t.position), b.center(), b.extents() * 1.02f, kSelection);
                } else {
                    dd.sphere(e.transform().position, 0.3f, kSelection, true);
                }
            }
            if (e.has<RigidBodyComponent>()) drawCollider(dd, e, kCollider);
            if (settings.lights) drawLight(dd, e);
        }
    });
}

} // namespace nova::editor::builtin
