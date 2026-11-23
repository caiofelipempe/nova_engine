#include "nova/editor/builtin/CoreComponents.h"

#include "nova/assets/AssetDatabase.h"
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
    r.component<AnimatorComponent>();
    // The animator's clip by name: the clips of the entity's own model first, then any other
    // model's (loaded when its menu opens); the reference text in the tooltip.
    r.fieldDrawer("Animator", "clip", [](void* field, const reflect::FieldInfo& info, EditorContext& ctx) {
        std::string& clip = *static_cast<std::string*>(field);
        AssetDatabase& assets = ctx.assets();
        Entity e = ctx.selected();
        const AnimatorComponent* anim = e.valid() ? e.tryGet<AnimatorComponent>() : nullptr;
        std::string label = "<none>";
        AssetGuid guid = 0;
        u32 index = 0;
        if (AssetDatabase::parseRef(clip, guid, index)) {
            const AssetRecord* r = assets.find(guid);
            const auto names = assets.animationNames(guid);
            label = (r ? r->fileName() : std::string("?")) + " / " + (index < names.size() ? names[index] : "#" + std::to_string(index));
        } else if (!clip.empty()) {
            label = clip; // another kind of reference
        } else if (anim && anim->data) {
            label = anim->data->name + " (set by code)";
        }
        bool changed = false;
        auto choose = [&](const std::string& ref) {
            if (ref == clip) return;
            clip = ref;
            changed = true;
            if (e.valid() && e.has<AnimatorComponent>()) { // the new clip from its start
                auto& a = e.get<AnimatorComponent>();
                a.time = 0.0;
                a.started = true;
                if (ref.empty()) a.data.reset();
            }
        };
        auto listClips = [&](AssetGuid model) {
            const auto names = assets.animationNames(model);
            if (names.empty()) ImGui::TextDisabled("(no animations)");
            for (u32 i = 0; i < names.size(); ++i) {
                const std::string ref = AssetDatabase::makeRef(model, i);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(names[i].empty() ? ("#" + std::to_string(i)).c_str() : names[i].c_str(), ref == clip)) choose(ref);
                ImGui::PopID();
            }
        };
        if (ImGui::BeginCombo(info.label.c_str(), label.c_str())) {
            if (ImGui::Selectable("<none>", clip.empty())) choose("");
            const AssetGuid own = e.valid() && e.has<PrefabInstanceComponent>() ? e.get<PrefabInstanceComponent>().asset : 0;
            const AssetRecord* ownRecord = own ? assets.find(own) : nullptr;
            if (ownRecord && ownRecord->type == "Model") {
                ImGui::SeparatorText(ownRecord->fileName().c_str());
                ImGui::PushID("own");
                listClips(own);
                ImGui::PopID();
            }
            if (ImGui::BeginMenu("Other models")) {
                bool any = false;
                for (const AssetRecord& r : assets.assets()) {
                    if (r.type != "Model" || r.guid == own) continue;
                    any = true;
                    ImGui::PushID(static_cast<int>(r.guid & 0x7FFFFFFF));
                    if (ImGui::BeginMenu(r.path.generic_string().c_str())) { // loads it now
                        listClips(r.guid);
                        ImGui::EndMenu();
                    }
                    ImGui::PopID();
                }
                if (!any) ImGui::TextDisabled("(no other models)");
                ImGui::EndMenu();
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered() && !clip.empty()) ImGui::SetTooltip("%s", clip.c_str());
        return changed;
    });
    r.component<SkinComponent>();
    r.component<AudioSourceComponent>();
    // The source's sound by name: every sound asset of the project; the reference in the tooltip.
    r.fieldDrawer("AudioSource", "clip", [](void* field, const reflect::FieldInfo& info, EditorContext& ctx) {
        std::string& clip = *static_cast<std::string*>(field);
        AssetDatabase& assets = ctx.assets();
        Entity e = ctx.selected();
        const AudioSourceComponent* src = e.valid() ? e.tryGet<AudioSourceComponent>() : nullptr;
        std::string label = "<none>";
        AssetGuid guid = 0;
        u32 index = 0;
        if (AssetDatabase::parseRef(clip, guid, index)) {
            const AssetRecord* r = assets.find(guid);
            label = r ? r->path.generic_string() : clip;
        } else if (!clip.empty()) {
            label = clip;
        } else if (src && src->data) {
            label = src->data->name + " (set by code)";
        }
        bool changed = false;
        if (ImGui::BeginCombo(info.label.c_str(), label.c_str())) {
            if (ImGui::Selectable("<none>", clip.empty()) && !clip.empty()) {
                clip.clear();
                changed = true;
            }
            for (const AssetRecord& r : assets.assets()) {
                if (r.type != "Sound") continue;
                const std::string ref = AssetDatabase::makeRef(r.guid, 0);
                if (ImGui::Selectable(r.path.generic_string().c_str(), ref == clip) && ref != clip) {
                    clip = ref;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered() && !clip.empty()) ImGui::SetTooltip("%s", clip.c_str());
        if (changed && e.valid() && e.has<AudioSourceComponent>()) { // the new sound from its start
            auto& s = e.get<AudioSourceComponent>();
            if (clip.empty()) s.data.reset();
            s.started = false;
        }
        return changed;
    });
    r.component<AudioListenerComponent>();

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
