#include "nova/editor/Widgets.h"

#include "nova/editor/EditorContext.h"
#include "nova/editor/Registry.h"
#include "nova/reflect/Reflect.h"
#include "nova/renderer/Renderer.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

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

// Fixed point: shown and dragged as doubles (a float has 24 bits, fewer than most formats), with
// as many decimals as the format has (at most 6), written back only when edited (unchanged
// components keep their exact bits).
const char* fixedFormat(const reflect::FieldInfo& f) {
    static const char* const formats[] = {"%.0f", "%.1f", "%.2f", "%.3f", "%.4f", "%.5f", "%.6f"};
    // The step in the shown unit: 2^-fracBits (degrees: a turn of 2^32 steps is 360°).
    f64 step = std::ldexp(1.0, -static_cast<int>(f.fracBits));
    if (f.kind == reflect::FieldKind::Angle || f.kind == reflect::FieldKind::AngleSpeed) step *= 360.0;
    const int decimals = static_cast<int>(std::ceil(-std::log10(step)));
    return formats[decimals < 0 ? 0 : decimals > 6 ? 6 : decimals];
}

// An angle shown in [0°, 360°) when its range is not negative (a hinge's upper limit of 180°
// reads 180, not −180), in [−180°, 180°) otherwise.
f64 shownFixedValue(const reflect::FieldInfo& f, const void* p, int i = 0) {
    f64 v = reflect::fixedValue(f, p, i);
    if (f.kind == reflect::FieldKind::Angle && f.attrs.min >= 0.0f && v < 0.0) v += 360.0;
    return v;
}

bool doubleField(const reflect::FieldInfo& f, f64& value) {
    const auto& a = f.attrs;
    const char* format = fixedFormat(f);
    if (bounded(a)) {
        const f64 lo = a.min, hi = a.max;
        return ImGui::SliderScalar(f.label.c_str(), ImGuiDataType_Double, &value, &lo, &hi, format);
    }
    return ImGui::DragScalar(f.label.c_str(), ImGuiDataType_Double, &value, dragSpeed(a, 0.05f), nullptr, nullptr, format);
}

bool fixedField(const reflect::FieldInfo& f, void* p) {
    f64 v = shownFixedValue(f, p);
    if (!doubleField(f, v)) return false;
    reflect::setFixedValue(f, p, v);
    return true;
}

bool fixedVecField(const reflect::FieldInfo& f, void* p) {
    f64 v[3], before[3];
    for (int i = 0; i < 3; ++i) before[i] = v[i] = shownFixedValue(f, p, i);
    const auto& a = f.attrs;
    const bool changed = ImGui::DragScalarN(f.label.c_str(), ImGuiDataType_Double, v, 3, dragSpeed(a, 0.05f), nullptr,
                                            nullptr, fixedFormat(f));
    if (!changed) return false;
    for (int i = 0; i < 3; ++i)
        if (v[i] != before[i]) reflect::setFixedValue(f, p, v[i], i);
    return true;
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
        case K::Position: changed = position(f.label.c_str(), *static_cast<GlobalPosition*>(p)); break;
        case K::Quat: changed = rotation(f.label.c_str(), *static_cast<glm::quat*>(p), dragSpeed(a, 0.5f)); break;
        case K::FixedQuat: changed = rotation(f.label.c_str(), *static_cast<Quat*>(p), dragSpeed(a, 0.5f)); break;
        case K::Fixed:
        case K::Angle:      // degrees
        case K::AngleSpeed: // degrees per second
            changed = fixedField(f, p);
            break;
        case K::FixedVec3: changed = fixedVecField(f, p); break;
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
        case K::Texture: {
            auto& t = *static_cast<rhi::TextureHandle*>(p);
            AssetDatabase& db = ctx.assets();
            std::string current = "<none>";
            if (t) {
                current = db.textureLabel(t);
                if (current.empty()) current = "<texture " + std::to_string(t.index) + ">";
            }
            if (ImGui::BeginCombo(f.label.c_str(), current.c_str())) {
                if (ImGui::Selectable("<none>", !t)) {
                    t = {};
                    changed = true;
                }
                for (const AssetRecord& r : db.assets()) {
                    if (r.type != "Texture") continue;
                    if (ImGui::Selectable(r.path.generic_string().c_str(), t && db.textureRef(t) == AssetDatabase::makeRef(r.guid))) {
                        if (const AssetData* d = db.load(r.guid); d && !d->textures.empty()) {
                            t = d->textures[0];
                            changed = true;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (!a.readOnly && ImGui::BeginDragDropTarget()) { // drop a texture from the Assets panel
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
                    AssetGuid guid = 0;
                    std::memcpy(&guid, payload->Data, sizeof(guid));
                    if (const AssetData* d = db.load(guid); d && !d->textures.empty()) {
                        t = d->textures[0];
                        changed = true;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            break;
        }
        case K::Shader: {
            auto& sh = *static_cast<ShaderHandle*>(p);
            AssetDatabase& db = ctx.assets();
            ShaderLibrary& lib = ctx.renderer().shaders();
            std::string current = "<default>";
            if (sh) {
                current = db.shaderLabel(sh);
                if (current.empty()) current = lib.path(sh).filename().string();
                if (current.empty()) current = "<shader " + std::to_string(sh.id) + ">";
            }
            // Material::vertexShader takes vertex shaders; the other shader fields fragment ones.
            const ShaderStage wanted = f.name.find("vertex") != std::string::npos ? ShaderStage::Vertex : ShaderStage::Fragment;
            auto stageOf = [](const std::filesystem::path& path) {
                ShaderStage stage = ShaderStage::Fragment;
                ShaderLibrary::stageFromPath(path, stage);
                return stage;
            };
            auto pick = [&](AssetGuid guid) {
                const AssetRecord* rec = db.find(guid);
                if (rec && stageOf(rec->path) != wanted) return;
                if (const AssetData* d = db.load(guid); d && !d->shaders.empty()) {
                    sh = d->shaders[0];
                    changed = true;
                }
            };
            if (ImGui::BeginCombo(f.label.c_str(), current.c_str())) {
                if (ImGui::Selectable("<default>", !sh)) {
                    sh = {};
                    changed = true;
                }
                for (const AssetRecord& r : db.assets()) {
                    if (r.type != "Shader") continue;
                    if (stageOf(r.path) != wanted) continue;
                    if (ImGui::Selectable(r.path.generic_string().c_str(), sh && db.shaderRef(sh) == AssetDatabase::makeRef(r.guid)))
                        pick(r.guid);
                }
                ImGui::EndCombo();
            }
            if (!a.readOnly && ImGui::BeginDragDropTarget()) { // drop a shader from the Assets panel
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayload)) {
                    AssetGuid guid = 0;
                    std::memcpy(&guid, payload->Data, sizeof(guid));
                    pick(guid);
                }
                ImGui::EndDragDropTarget();
            }
            if (sh && !lib.error(sh).empty()) {
                ImGui::TextColored({1.0f, 0.45f, 0.4f, 1.0f}, lib.ready(sh) ? "Compile error (previous version in use)"
                                                                           : "Compile error (default shading in use)");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lib.error(sh).c_str());
            }
            break;
        }
        case K::Entity: {
            auto& ref = *static_cast<EntityRef*>(p);
            Scene& scene = ctx.scene();
            const Entity target = scene.resolve(ref);
            std::string current = !ref ? "<none>" : target.valid() ? target.name() : "<missing>";
            if (ImGui::BeginCombo(f.label.c_str(), current.c_str())) {
                if (ImGui::Selectable("<none>", !ref)) {
                    ref = {};
                    changed = true;
                }
                for (auto [h, id, name] : scene.view<IdComponent, NameComponent>().each()) {
                    ImGui::PushID(static_cast<int>(entt::to_integral(h)));
                    if (ImGui::Selectable(name.name.c_str(), id.uuid == ref.uuid)) {
                        ref.uuid = id.uuid;
                        changed = true;
                    }
                    ImGui::PopID();
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

EulerOrder rotationDisplayOrder = kDefaultEulerOrder;

namespace {
bool rotationAt(const char* label, glm::quat& value, const void* key, float speed);
} // namespace

bool rotation(const char* label, glm::quat& value, float speed) { return rotationAt(label, value, &value, speed); }

bool rotation(const char* label, Quat& value, float speed) {
    glm::quat q = value.toGlm();
    if (!rotationAt(label, q, &value, speed)) return false;
    value = q;
    return true;
}

namespace {
bool rotationAt(const char* label, glm::quat& value, const void* key, float speed) {
    // Last angles shown per field: kept while the quaternion is unchanged (and the order too).
    struct Shown {
        glm::quat q;
        glm::vec3 degrees;
        EulerOrder order;
    };
    static std::unordered_map<const void*, Shown> shownByField;
    if (shownByField.size() > 4096) shownByField.clear();
    Shown& shown = shownByField[key];
    if (shown.order != rotationDisplayOrder || !(shown.q == value)) {
        // "+ 0" turns −0 into 0, so an identity rotation does not read "-0.00".
        shown = {value, glm::vec3(Quat(value).toEulerDegrees(rotationDisplayOrder)) + glm::vec3(0.0f), rotationDisplayOrder};
    }
    glm::vec3 degrees = shown.degrees;
    const bool changed = vec3(label, degrees, speed);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Euler %s, degrees (View/Rotation Order)\nquaternion x %.4f  y %.4f  z %.4f  w %.4f",
                          eulerOrderName(rotationDisplayOrder), value.x, value.y, value.z, value.w);
    if (!changed) return false;
    value = Quat::fromEulerDegrees(glm::dvec3(degrees), rotationDisplayOrder).toGlm();
    shown = {value, degrees, rotationDisplayOrder};
    return true;
}
} // namespace

bool position(const char* label, GlobalPosition& value, float speed) {
    // Edited in meters (double): exact to well below a nanometer up to millions of km. Unchanged
    // axes keep their exact integer value.
    glm::dvec3 meters = value.toMeters();
    bool changed = false;
    ImGui::PushID(label);
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 90.0f);
    ImGui::TextUnformatted(label);
    ImGui::NextColumn();
    const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    const char* axes[3] = {"X", "Y", "Z"};
    const ImVec4 colors[3] = {{0.75f, 0.2f, 0.2f, 1}, {0.25f, 0.65f, 0.25f, 1}, {0.2f, 0.35f, 0.8f, 1}};
    const double step = speed;
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Border, colors[i]);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::SetNextItemWidth(w);
        char id[8];
        std::snprintf(id, sizeof(id), "##%s", axes[i]);
        double v = meters[i];
        bool axisChanged = ImGui::DragScalar(id, ImGuiDataType_Double, &v, static_cast<float>(step), nullptr, nullptr, "%.3f");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            v = 0.0;
            axisChanged = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            const LengthUnit u = readableUnit(meters[i]);
            ImGui::SetTooltip("%.9g %s\n%lld steps of %.3g m", fromMeters(meters[i], u), unitSymbol(u),
                              static_cast<long long>(value.raw(i)), WorldSpace::kUnit);
        }
        if (axisChanged) {
            // Move by the edited amount: the other axes stay bit-exact.
            glm::dvec3 delta(0.0);
            delta[i] = v - meters[i];
            value += GlobalOffset::fromMeters(delta);
            changed = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::Columns(1);
    ImGui::PopID();
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
