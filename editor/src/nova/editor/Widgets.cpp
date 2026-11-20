#include "nova/editor/Widgets.h"

#include "nova/editor/EditorContext.h"
#include "nova/editor/Registry.h"
#include "nova/reflect/Reflect.h"
#include "nova/renderer/Renderer.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace nova::editor::widgets {

namespace {

float dragSpeed(const reflect::FieldAttrs& a, float fallback) { return a.speed > 0.0f ? a.speed : fallback; }
bool bounded(const reflect::FieldAttrs& a) { return a.max > a.min; }

bool floatField(const reflect::FieldInfo& f, float& value) {
    const auto& a = f.attrs;
    float shown = a.degrees ? glm::degrees(value) : value;
    bool changed = bounded(a) ? ImGui::SliderFloat(f.label.c_str(), &shown, a.min, a.max)
                              : ImGui::DragFloat(f.label.c_str(), &shown, dragSpeed(a, a.degrees ? 0.5f : 0.05f));
    if (changed) value = a.degrees ? glm::radians(shown) : shown;
    return changed;
}

bool vecField(const reflect::FieldInfo& f, float* v, int n) {
    const auto& a = f.attrs;
    if (a.color && n == 3) return ImGui::ColorEdit3(f.label.c_str(), v);
    if (a.color && n == 4) return ImGui::ColorEdit4(f.label.c_str(), v);
    if (n == 3) {
        glm::vec3 shown{v[0], v[1], v[2]};
        if (a.degrees) shown = glm::degrees(shown);
        if (!vec3(f.label.c_str(), shown, dragSpeed(a, 0.05f))) return false;
        if (a.degrees) shown = glm::radians(shown);
        v[0] = shown.x, v[1] = shown.y, v[2] = shown.z;
        return true;
    }
    const float speed = dragSpeed(a, 0.05f);
    const float lo = bounded(a) ? a.min : 0.0f, hi = bounded(a) ? a.max : 0.0f;
    return n == 2 ? ImGui::DragFloat2(f.label.c_str(), v, speed, lo, hi) : ImGui::DragFloat4(f.label.c_str(), v, speed, lo, hi);
}

} // namespace

bool reflectedField(const reflect::TypeInfo& owner, const reflect::FieldInfo& f, void* p, EditorContext& ctx) {
    if (const FieldDrawer* custom = ctx.registry().findFieldDrawer(owner, f)) return (*custom)(p, f, ctx);

    using K = reflect::FieldKind;
    const auto& a = f.attrs;
    bool changed = false;
    ImGui::BeginDisabled(a.readOnly);
    switch (f.kind) {
        case K::Bool: changed = ImGui::Checkbox(f.label.c_str(), static_cast<bool*>(p)); break;
        case K::Int: {
            auto* v = static_cast<i32*>(p);
            changed = bounded(a) ? ImGui::SliderInt(f.label.c_str(), v, static_cast<int>(a.min), static_cast<int>(a.max))
                                 : ImGui::DragInt(f.label.c_str(), v, dragSpeed(a, 0.2f));
            break;
        }
        case K::UInt: changed = ImGui::DragScalar(f.label.c_str(), ImGuiDataType_U32, p, dragSpeed(a, 0.2f)); break;
        case K::UInt64:
            if (a.readOnly) ImGui::Text("%s: %llu", f.label.c_str(), static_cast<unsigned long long>(*static_cast<u64*>(p)));
            else changed = ImGui::DragScalar(f.label.c_str(), ImGuiDataType_U64, p, dragSpeed(a, 1.0f));
            break;
        case K::Float: changed = floatField(f, *static_cast<float*>(p)); break;
        case K::Vec2: changed = vecField(f, &static_cast<glm::vec2*>(p)->x, 2); break;
        case K::Vec3: changed = vecField(f, &static_cast<glm::vec3*>(p)->x, 3); break;
        case K::Vec4: changed = vecField(f, &static_cast<glm::vec4*>(p)->x, 4); break;
        case K::String: {
            auto& s = *static_cast<std::string*>(p);
            char buf[512];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (ImGui::InputText(f.label.c_str(), buf, sizeof(buf))) {
                s = buf;
                changed = true;
            }
            break;
        }
        case K::Enum: {
            const reflect::TypeInfo* e = f.nestedType();
            const i64 current = f.getEnum(p);
            const char* currentName = e ? e->enumName(current) : nullptr;
            if (ImGui::BeginCombo(f.label.c_str(), currentName ? currentName : "?")) {
                if (e)
                    for (const auto& v : e->enumValues)
                        if (ImGui::Selectable(v.name.c_str(), v.value == current)) {
                            f.setEnum(p, v.value);
                            changed = true;
                        }
                ImGui::EndCombo();
            }
            break;
        }
        case K::Mesh: {
            auto& m = *static_cast<MeshHandle*>(p);
            Renderer& renderer = ctx.renderer();
            if (ImGui::BeginCombo(f.label.c_str(), m ? renderer.meshName(m).c_str() : "<none>")) {
                if (ImGui::Selectable("<none>", !m)) {
                    m = {};
                    changed = true;
                }
                for (MeshHandle h : renderer.meshes())
                    if (ImGui::Selectable(renderer.meshName(h).c_str(), h == m)) {
                        m = h;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
            break;
        }
        case K::Struct: {
            const reflect::TypeInfo* t = f.nestedType();
            if (!t) {
                ImGui::TextDisabled("%s: <unreflected type>", f.label.c_str());
                break;
            }
            if (ImGui::TreeNodeEx(f.label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
                changed = reflected(*t, p, ctx);
                ImGui::TreePop();
            }
            break;
        }
    }
    ImGui::EndDisabled();
    if (a.tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", a.tooltip);
    return changed;
}

bool reflected(const reflect::TypeInfo& type, void* object, EditorContext& ctx) {
    bool changed = false;
    for (const auto& f : type.fields) {
        if (f.attrs.hidden || f.attrs.transient) continue;
        ImGui::PushID(f.name.c_str());
        changed |= reflectedField(type, f, f.in(object), ctx);
        ImGui::PopID();
    }
    return changed;
}

bool vec3(const char* label, glm::vec3& value, float speed, float resetValue) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 90.0f);
    ImGui::TextUnformatted(label);
    ImGui::NextColumn();
    const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    const char* axes[3] = {"X", "Y", "Z"};
    const ImVec4 colors[3] = {{0.75f, 0.2f, 0.2f, 1}, {0.25f, 0.65f, 0.25f, 1}, {0.2f, 0.35f, 0.8f, 1}};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Border, colors[i]);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::SetNextItemWidth(w);
        char id[8];
        std::snprintf(id, sizeof(id), "##%s", axes[i]);
        changed |= ImGui::DragFloat(id, &value[i], speed, 0.0f, 0.0f, "%.2f");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            value[i] = resetValue;
            changed = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::Columns(1);
    ImGui::PopID();
    return changed;
}

bool material(Material& m) {
    bool changed = false;
    changed |= ImGui::ColorEdit4("Base color", glm::value_ptr(m.baseColor));
    changed |= ImGui::SliderFloat("Metallic", &m.metallic, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Roughness", &m.roughness, 0.0f, 1.0f);
    changed |= ImGui::ColorEdit3("Emissive", glm::value_ptr(m.emissive));
    changed |= ImGui::DragFloat("Emissive strength", &m.emissiveStrength, 0.1f, 0.0f, 100.0f);
    changed |= ImGui::Checkbox("Checker pattern", &m.checker);
    return changed;
}

bool componentHeader(const char* title, bool removable, bool& removeRequested, bool defaultOpen) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    const bool open = ImGui::CollapsingHeader(title, flags);
    if (removable) {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 10.0f);
        ImGui::PushID(title);
        if (ImGui::SmallButton("x")) removeRequested = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove component");
        ImGui::PopID();
    }
    return open;
}

} // namespace nova::editor::widgets
