#include "nova/editor/builtin/CoreComponents.h"

#include "nova/core/Application.h"
#include "nova/editor/EditorContext.h"
#include "nova/editor/Widgets.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

namespace nova::editor::builtin {

namespace {
bool hasCollider(Entity e) {
    return e.has<BoxColliderComponent>() || e.has<SphereColliderComponent>() || e.has<CapsuleColliderComponent>();
}
} // namespace

void CoreComponentsPlugin::onRegister(Registry& r) {
    // Transform: always present, not removable.
    ComponentEditor& transform = r.component<TransformComponent>("Transform", [](TransformComponent& t, Entity e, EditorContext& ctx) {
        bool changed = widgets::vec3("Position", t.position);
        glm::vec3 deg = glm::degrees(t.rotation);
        if (widgets::vec3("Rotation", deg, 0.5f)) {
            t.rotation = glm::radians(deg);
            changed = true;
        }
        changed |= widgets::vec3("Scale", t.scale, 0.02f, 1.0f);
        if (changed && ctx.scene().isSimulating()) ctx.scene().syncTransformToPhysics(e);
    });
    transform.add = nullptr;
    transform.remove = nullptr;

    ComponentEditor& mesh = r.component<MeshComponent>("Mesh", [](MeshComponent& m, Entity, EditorContext& ctx) {
        Renderer& renderer = ctx.renderer();
        if (ImGui::BeginCombo("Mesh", renderer.meshName(m.mesh).c_str())) {
            for (MeshHandle h : renderer.meshes())
                if (ImGui::Selectable(renderer.meshName(h).c_str(), h == m.mesh)) m.mesh = h;
            ImGui::EndCombo();
        }
        ImGui::Checkbox("Cast shadows", &m.castShadows);
        widgets::material(m.material);
    });
    mesh.add = [](Entity e) { e.add<MeshComponent>(Application::get().renderer().cube()); };

    ComponentEditor& camera = r.component<CameraComponent>("Camera", [](CameraComponent& c) {
        float fov = glm::degrees(c.fovY);
        if (ImGui::SliderFloat("FOV", &fov, 10.0f, 120.0f)) c.fovY = glm::radians(fov);
        ImGui::DragFloat("Near", &c.nearClip, 0.01f, 0.001f, 10.0f);
        ImGui::DragFloat("Far", &c.farClip, 1.0f, 1.0f, 10000.0f);
        ImGui::Checkbox("Primary", &c.primary);
    });
    camera.add = [](Entity e) { e.add<CameraComponent>().primary = false; };

    r.component<DirectionalLightComponent>("Directional Light", [](DirectionalLightComponent& l) {
        ImGui::ColorEdit3("Color", glm::value_ptr(l.color));
        ImGui::DragFloat("Intensity", &l.intensity, 0.05f, 0.0f, 100.0f);
    });
    r.component<PointLightComponent>("Point Light", [](PointLightComponent& l) {
        ImGui::ColorEdit3("Color", glm::value_ptr(l.color));
        ImGui::DragFloat("Intensity", &l.intensity, 0.1f, 0.0f, 1000.0f);
        ImGui::DragFloat("Radius", &l.radius, 0.05f, 0.1f, 100.0f);
    });

    r.component<RigidBodyComponent>("Rigid Body", [](RigidBodyComponent& rb, Entity e, EditorContext& ctx) {
        const char* types[] = {"Static", "Kinematic", "Dynamic"};
        int type = static_cast<int>(rb.type);
        ImGui::BeginDisabled(ctx.scene().isSimulating());
        if (ImGui::Combo("Type", &type, types, 3)) rb.type = static_cast<physics::BodyType>(type);
        ImGui::EndDisabled();
        ImGui::DragFloat("Mass", &rb.mass, 0.05f, 0.001f, 10000.0f);
        ImGui::SliderFloat("Friction", &rb.friction, 0.0f, 1.0f);
        ImGui::SliderFloat("Restitution", &rb.restitution, 0.0f, 1.0f);
        ImGui::SliderFloat("Linear damping", &rb.linearDamping, 0.0f, 1.0f);
        ImGui::SliderFloat("Angular damping", &rb.angularDamping, 0.0f, 1.0f);
        ImGui::DragFloat("Gravity factor", &rb.gravityFactor, 0.01f, -10.0f, 10.0f);
        ImGui::Checkbox("Sensor", &rb.isSensor);
        if (ctx.scene().isSimulating()) {
            ImGui::TextDisabled("Body properties apply on next Play.");
            glm::vec3 v = ctx.scene().linearVelocity(e);
            ImGui::Text("Velocity %.2f %.2f %.2f", v.x, v.y, v.z);
        }
    });

    auto colliderCanAdd = [](Entity e) { return !hasCollider(e); };
    r.component<BoxColliderComponent>("Box Collider", [](BoxColliderComponent& c) {
        ImGui::DragFloat3("Half extents", glm::value_ptr(c.halfExtents), 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat3("Offset", glm::value_ptr(c.offset), 0.01f);
    }).canAdd = colliderCanAdd;
    r.component<SphereColliderComponent>("Sphere Collider", [](SphereColliderComponent& c) {
        ImGui::DragFloat("Radius", &c.radius, 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat3("Offset", glm::value_ptr(c.offset), 0.01f);
    }).canAdd = colliderCanAdd;
    r.component<CapsuleColliderComponent>("Capsule Collider", [](CapsuleColliderComponent& c) {
        ImGui::DragFloat("Radius", &c.radius, 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat("Half height", &c.halfHeight, 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat3("Offset", glm::value_ptr(c.offset), 0.01f);
    }).canAdd = colliderCanAdd;

    r.component<RotatorComponent>("Rotator", [](RotatorComponent& c) {
        ImGui::DragFloat3("Axis", glm::value_ptr(c.axis), 0.01f);
        ImGui::DragFloat("Speed", &c.speed, 0.01f);
    });

    // Native scripts: read-only summary (scripts are code, attached with Entity::addScript).
    ComponentEditor& scripts = r.component<ScriptComponent>("Scripts", [](ScriptComponent& sc) {
        ImGui::TextDisabled("%zu native script(s)", sc.scripts.size());
    });
    scripts.add = nullptr;
    scripts.remove = nullptr;
    scripts.defaultOpen = false;
}

} // namespace nova::editor::builtin
