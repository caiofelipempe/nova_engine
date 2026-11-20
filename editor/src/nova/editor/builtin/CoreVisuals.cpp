#include "nova/editor/builtin/CoreVisuals.h"

#include "nova/editor/EditorContext.h"
#include "nova/renderer/Renderer.h"

#include <algorithm>

namespace nova::editor::builtin {

namespace {
const glm::vec4 kSelection{1.0f, 0.6f, 0.1f, 1.0f};
const glm::vec4 kCollider{0.3f, 1.0f, 0.4f, 0.9f};
const glm::vec4 kColliderFaint{0.3f, 1.0f, 0.4f, 0.35f};
const glm::vec4 kLight{1.0f, 0.9f, 0.3f, 0.9f};
const glm::vec4 kCamera{0.7f, 0.8f, 1.0f, 0.9f};

void drawFrustum(DebugDraw& dd, const TransformComponent& t, const CameraComponent& c) {
    const f32 farDist = std::min(c.farClip, 4.0f), aspect = 16.0f / 9.0f;
    const f32 h = std::tan(c.fovY * 0.5f), w = h * aspect;
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), t.position) * glm::toMat4(t.orientation());
    auto corner = [&](f32 d, f32 sx, f32 sy) { return glm::vec3(m * glm::vec4(sx * w * d, sy * h * d, -d, 1.0f)); };
    const f32 n = c.nearClip;
    glm::vec3 nc[4] = {corner(n, -1, -1), corner(n, 1, -1), corner(n, 1, 1), corner(n, -1, 1)};
    glm::vec3 fc[4] = {corner(farDist, -1, -1), corner(farDist, 1, -1), corner(farDist, 1, 1), corner(farDist, -1, 1)};
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

    r.uiHook([](EditorContext& ctx) {
        DebugDraw& dd = ctx.renderer().debugDraw();
        Scene& scene = ctx.scene();
        if (settings.grid) dd.grid({0.0f, 0.002f, 0.0f}, 40.0f, 40, {0.45f, 0.45f, 0.5f, 0.6f});

        if (settings.allColliders)
            for (auto [h, rb] : scene.registry().view<RigidBodyComponent>().each())
                if (!ctx.isSelected({h, &scene})) drawCollider(dd, {h, &scene}, kColliderFaint);

        Renderer& renderer = ctx.renderer();
        for (Entity e : ctx.selection()) {
            if (!e.valid()) continue;
            if (settings.selectionBounds) {
                if (auto* mc = e.tryGet<MeshComponent>(); mc && mc->mesh) {
                    const Bounds b = renderer.meshBounds(mc->mesh);
                    dd.box(e.transform().matrix(), b.center(), b.extents() * 1.02f, kSelection);
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
