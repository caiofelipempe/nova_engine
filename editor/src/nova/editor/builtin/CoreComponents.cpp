#include "nova/editor/builtin/CoreComponents.h"

#include "nova/core/Application.h"
#include "nova/editor/EditorContext.h"
#include "nova/editor/Widgets.h"
#include "nova/renderer/Renderer.h"

#include <imgui.h>

namespace nova::editor::builtin {

namespace {
bool hasCollider(Entity e) {
    return e.has<BoxColliderComponent>() || e.has<SphereColliderComponent>() || e.has<CapsuleColliderComponent>() ||
           e.has<CylinderColliderComponent>() || e.has<ConvexHullColliderComponent>() || e.has<MeshColliderComponent>();
}

bool hasUnreflectedScript(Entity e) {
    auto* sc = e.tryGet<ScriptComponent>();
    if (!sc) return false;
    for (auto& s : sc->scripts) {
        const reflect::TypeInfo* t = reflect::types().find(std::type_index(typeid(*s)));
        if (!t || !t->isScript) return true;
    }
    return false;
}
} // namespace

void CoreComponentsPlugin::onRegister(Registry& r) {
    // Everything is generated from reflection (see reflect/EngineTypes.cpp); this plugin only
    // fixes the inspector order and adds behaviour reflection cannot express.

    // Transform: always present; edits while simulating teleport the physics body.
    ComponentEditor& transform = r.component<TransformComponent>();
    transform.add = nullptr;
    transform.remove = nullptr;
    transform.draw = [](Entity e, EditorContext& ctx) {
        const reflect::TypeInfo& t = *reflect::types().find<TransformComponent>();
        if (widgets::reflected(t, &e.get<TransformComponent>(), ctx) && ctx.scene().isSimulating())
            ctx.scene().syncTransformToPhysics(e);
    };

    r.component<MeshComponent>().add = [](Entity e) { e.add<MeshComponent>(Application::get().renderer().cube()); };
    r.component<CameraComponent>().add = [](Entity e) { e.add<CameraComponent>().primary = false; };
    r.component<DirectionalLightComponent>();
    r.component<PointLightComponent>();

    r.component<RigidBodyComponent>();
    auto oneCollider = [](Entity e) { return !hasCollider(e); };
    r.component<BoxColliderComponent>().canAdd = oneCollider;
    r.component<SphereColliderComponent>().canAdd = oneCollider;
    r.component<CapsuleColliderComponent>().canAdd = oneCollider;
    r.component<CylinderColliderComponent>().canAdd = oneCollider;
    r.component<ConvexHullColliderComponent>().canAdd = oneCollider;
    r.component<MeshColliderComponent>().canAdd = oneCollider;

    r.component<RotatorComponent>();

    // Scripts without reflection can't be edited or saved: show that they exist.
    ComponentEditor scripts;
    scripts.name = "Scripts";
    scripts.has = hasUnreflectedScript;
    scripts.defaultOpen = false;
    scripts.draw = [](Entity e, EditorContext&) {
        for (auto& s : e.get<ScriptComponent>().scripts) {
            const reflect::TypeInfo* t = reflect::types().find(std::type_index(typeid(*s)));
            if (!t || !t->isScript) ImGui::BulletText("%s", typeid(*s).name());
        }
        ImGui::TextDisabled("Not reflected: not editable, not saved.");
    };
    r.addComponentEditor(std::move(scripts));
}

} // namespace nova::editor::builtin
