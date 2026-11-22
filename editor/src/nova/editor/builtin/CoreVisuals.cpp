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
// The torus seams (cyan, where +8 192 m meets −8 192 m) within reach of the camera, as a
// world-anchored grid patch on the seam plane.
void drawWorldBounds(DebugDraw& dd, const GlobalPosition& camera) {
    constexpr f64 kReach = 300.0;  // draw seams closer than this to the camera
    constexpr f32 kHalf = 120.0f;  // half size of the drawn patch
    constexpr f32 kStep = 10.0f;   // grid spacing
    const glm::vec4 color(0.2f, 0.85f, 1.0f, 0.8f);
    const glm::dvec3 camMeters = camera.toMeters();
    for (int axis = 0; axis < 3; ++axis) {
        // The seam is at −kHalfLap ≡ +kHalfLap: the shortest way there from the camera.
        GlobalPosition seam = camera;
        glm::i32vec3 raw = camera.raw();
        raw[axis] = std::numeric_limits<i32>::min();
        seam = GlobalPosition::fromRaw(raw);
        const f64 d = (seam - camera).toMeters()[axis];
        if (std::abs(d) > kReach) continue;
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        glm::vec3 toPlane(0.0f);
        toPlane[axis] = static_cast<f32>(d);
        // Snap the grid to world multiples of kStep so it does not slide with the camera.
        toPlane[u] = -static_cast<f32>(std::fmod(camMeters[u], static_cast<f64>(kStep)));
        toPlane[v] = -static_cast<f32>(std::fmod(camMeters[v], static_cast<f64>(kStep)));
        const GlobalPosition center = camera + toPlane;
        for (f32 o = -kHalf; o <= kHalf; o += kStep) {
            glm::vec3 a0(0.0f), a1(0.0f), b0(0.0f), b1(0.0f);
            a0[u] = -kHalf, a1[u] = kHalf, a0[v] = a1[v] = o;
            b0[v] = -kHalf, b1[v] = kHalf, b0[u] = b1[u] = o;
            dd.line(center + a0, center + a1, color);
            dd.line(center + b0, center + b1, color);
        }
    }
}
} // namespace

void drawCollider(DebugDraw& dd, Entity e, const glm::vec4& color) {
    // Same sizing rules as Scene::createBody: sizes scaled by the transform, offset in local space.
    const auto& t = e.transform();
    const glm::vec3 s = glm::abs(t.scale);
    const glm::quat q = t.orientation();
    if (auto* box = e.tryGet<BoxColliderComponent>()) {
        dd.box(t.position + q * (box->offset * s), box->halfExtents * s, q, color);
    } else if (auto* sphere = e.tryGet<SphereColliderComponent>()) {
        dd.sphere(t.position + q * (sphere->offset * s), sphere->radius * std::max({s.x, s.y, s.z}), color);
    } else if (auto* cap = e.tryGet<CapsuleColliderComponent>()) {
        dd.capsule(t.position + q * (cap->offset * s), q, cap->radius * std::max(s.x, s.z), cap->halfHeight * s.y, color);
    } else if (auto* cyl = e.tryGet<CylinderColliderComponent>()) {
        const GlobalPosition c = t.position + q * (cyl->offset * s);
        const f32 r = cyl->radius * std::max(s.x, s.z), h = cyl->halfHeight * s.y;
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
        const glm::vec3 shift = hull ? q * (hull->offset * s) : glm::vec3(0.0f);
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
    toggle("view.worldBounds", "View/Show World Bounds", &VisualSettings::worldBounds, 204);

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
        if (settings.worldBounds) drawWorldBounds(dd, ctx.camera().position);

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
