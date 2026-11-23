#include "nova/editor/builtin/CorePanels.h"

#include "nova/core/Application.h"
#include "nova/core/Log.h"
#include "nova/editor/Picking.h"
#include "nova/editor/Tool.h"
#include "nova/editor/Widgets.h"
#include "nova/editor/builtin/CoreTools.h"

#include <imgui.h> // must precede ImGuizmo.h
#include <ImGuizmo.h>
#include <imgui_internal.h> // ImRect, BeginDragDropTargetCustom

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <cstdio>
#include <cstring>
#include <limits>

namespace nova::editor::builtin {

// ---------------------------------------------------------------- viewport --
namespace {
// Asset dropped on the viewport: textures and material shaders go onto the mesh under the mouse; models/prefabs are
// placed where the mouse ray hits the scene (or the ground plane), resting on upward surfaces.
void dropAsset(EditorContext& ctx, AssetGuid guid, const ViewportState& vp) {
    const AssetRecord* record = ctx.assets().find(guid);
    if (!record) return;
    Renderer& renderer = ctx.renderer();
    const auto hit = pick(ctx.scene(), [&](MeshHandle m) { return renderer.meshData(m); }, vp.rayOrigin, vp.rayDirection);

    if (record->type == "Texture") {
        const AssetData* data = ctx.assets().load(guid);
        if (!hit || !hit->entity.has<MeshComponent>() || !data || data->textures.empty()) return;
        Entity target = hit->entity;
        const rhi::TextureHandle texture = data->textures[0];
        ctx.edit("Apply texture", {target}, [&] { target.get<MeshComponent>().material.baseColorTexture = texture; });
        ctx.select(target);
        return;
    }
    if (record->type == "Shader") {
        ShaderStage stage{};
        if (ShaderLibrary::stageFromPath(record->path, stage) && stage != ShaderStage::Fragment) return;
        const AssetData* data = ctx.assets().load(guid);
        if (!hit || !hit->entity.has<MeshComponent>() || !data || data->shaders.empty()) return;
        Entity target = hit->entity;
        const ShaderHandle shader = data->shaders[0];
        ctx.edit("Apply material shader", {target}, [&] { target.get<MeshComponent>().material.shader = shader; });
        ctx.select(target);
        return;
    }
    if (!ctx.assets().canInstantiate(*record)) return;

    GlobalPosition position = vp.rayOrigin + vp.rayDirection * 6.0f;
    bool onSurface = false;
    if (hit) {
        position = hit->point;
        onSurface = hit->normal.y > 0.5f;
    } else if (vp.rayDirection.y < -1e-4f) { // the ground plane y = 0
        const f64 t = -vp.rayOrigin.toMeters().y / vp.rayDirection.y;
        position = vp.rayOrigin + GlobalOffset::fromMeters(glm::dvec3(vp.rayDirection) * t);
        onSurface = true;
    }
    ctx.instantiateAsset(guid, position, [&](std::vector<Entity>& created) {
        if (!onSurface) return;
        f32 minY = std::numeric_limits<f32>::max();
        for (Entity e : created) {
            const auto* mesh = e.tryGet<MeshComponent>();
            if (!mesh) continue;
            const Bounds b = renderer.meshBounds(mesh->mesh);
            const glm::mat4 m = e.transform().matrixRelativeTo(position); // heights measured from the drop point
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 corner{i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z};
                minY = std::min(minY, glm::vec3(m * glm::vec4(corner, 1.0f)).y);
            }
        }
        if (minY == std::numeric_limits<f32>::max()) return;
        for (Entity e : created) e.transform().position += glm::vec3(0.0f, -minY, 0.0f); // rest on the surface
    });
}
} // namespace
ViewportPanel::ViewportPanel() : Panel("Viewport", DockSlot::Center) { noPadding = true; }

void ViewportPanel::onAttach(EditorContext& ctx) { m_view = ctx.renderer().createView({1280, 720}); }

void ViewportPanel::onDetach(EditorContext& ctx) {
    if (ctx.inputCaptured()) ctx.app().window().setCursorCaptured(false);
    ctx.setInputCaptured(false);
    m_view.reset();
}

void ViewportPanel::onUpdate(EditorContext& ctx, f32 dt) {
    // Resize to last frame's panel size (never mid-frame).
    m_view->resize({static_cast<u32>(std::max(1.0f, m_size.x)), static_cast<u32>(std::max(1.0f, m_size.y))});

    const bool wasActive = ctx.inputCaptured();
    const bool active = ctx.camera().update(dt, m_hovered);
    if (active != wasActive) ctx.app().window().setCursorCaptured(active);
    ctx.setInputCaptured(active);
}

void ViewportPanel::onRender(EditorContext& ctx, rhi::CommandList& cmd) {
    const Extent2D size = m_view->size();
    const f32 aspect = static_cast<f32>(size.width) / static_cast<f32>(size.height);
    const CameraData editorCam = ctx.camera().data(aspect);
    Scene& scene = ctx.scene();
    if (!scene.buildRenderScene(m_renderScene, aspect, useSceneCamera ? nullptr : &editorCam))
        scene.buildRenderScene(m_renderScene, aspect, &editorCam); // no scene camera: fall back
    ctx.renderer().render(cmd, m_renderScene, *m_view);
}

void ViewportPanel::onImGui(EditorContext& ctx) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    m_size = {avail.x, avail.y};
    const ImVec2 imagePos = ImGui::GetCursorScreenPos();
    if (auto* imgui = ctx.app().imgui()) ImGui::Image(static_cast<ImTextureID>(imgui->textureId(m_view->output())), avail);
    m_hovered = ImGui::IsItemHovered();
    AssetGuid dropped = 0;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetDragPayload))
            std::memcpy(&dropped, payload->Data, sizeof(dropped));
        ImGui::EndDragDropTarget();
    }

    // Viewport state for tools: rect, camera and the mouse ray.
    ViewportState& vp = m_state;
    vp.min = {imagePos.x, imagePos.y};
    vp.size = {std::max(1.0f, avail.x), std::max(1.0f, avail.y)};
    const ImVec2 mouse = ImGui::GetMousePos();
    vp.mouse = {mouse.x - imagePos.x, mouse.y - imagePos.y};
    vp.hovered = m_hovered;
    vp.camera = ctx.camera().data(vp.size.x / vp.size.y);
    {
        const glm::vec2 ndc = vp.mouse / vp.size * 2.0f - 1.0f; // Vulkan clip space: y down
        const glm::mat4 inv = glm::inverse(vp.camera.projection * vp.camera.view);
        glm::vec4 nearP = inv * glm::vec4(ndc, 0.0f, 1.0f), farP = inv * glm::vec4(ndc, 1.0f, 1.0f);
        const glm::vec3 nearLocal = glm::vec3(nearP) / nearP.w; // relative to the camera origin
        vp.rayOrigin = vp.camera.origin + nearLocal;
        vp.rayDirection = glm::normalize(glm::vec3(farP) / farP.w - nearLocal);
    }

    if (dropped) dropAsset(ctx, dropped, vp);

    // Toolbar: one button per registered tool.
    ImGui::SetCursorScreenPos({imagePos.x + 8, imagePos.y + 8});
    Tool* active = ctx.activeTool();
    for (auto& tool : ctx.registry().tools()) {
        const bool isActive = tool.get() == active;
        if (isActive) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const std::string label = tool->shortcut().empty() ? tool->name() : tool->name() + " (" + tool->shortcut() + ")";
        if (ImGui::Button(label.c_str())) ctx.setActiveTool(tool.get());
        if (isActive) ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    if (dynamic_cast<TransformTool*>(active)) {
        ImGui::Checkbox("Local", &TransformTool::localSpace);
        ImGui::SameLine();
    }
    ImGui::Checkbox("Scene camera", &useSceneCamera);
    const bool overToolbar = ImGui::IsAnyItemHovered();

    // Active tool (gizmo, brush...) and its preview lines.
    if (active && !useSceneCamera) {
        active->onViewport(ctx, vp);
        active->onDebugDraw(ctx, ctx.renderer().debugDraw());
    }

    // Click to select (Ctrl toggles, Shift adds), unless the tool uses the left button.
    const bool clickable = m_hovered && !overToolbar && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() &&
                           (!active || active->clickSelects()) && !useSceneCamera;
    if (clickable && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Renderer& renderer = ctx.renderer();
        auto hit = pick(ctx.scene(), [&](MeshHandle m) { return renderer.meshData(m); }, vp.rayOrigin, vp.rayDirection);
        const ImGuiIO& io = ImGui::GetIO();
        if (hit) {
            if (io.KeyCtrl) ctx.toggleSelection(hit->entity);
            else if (io.KeyShift) ctx.addToSelection(hit->entity);
            else ctx.select(hit->entity);
        } else if (!io.KeyCtrl && !io.KeyShift) {
            ctx.clearSelection();
        }
    }

    if (!ctx.inputCaptured()) {
        ImGui::SetCursorScreenPos({imagePos.x + 8, imagePos.y + avail.y - 24});
        ImGui::TextDisabled("Click: select (Ctrl toggle, Shift add)   RMB + WASD/QE: fly   F: focus   "
                            "Ctrl+Z/Y: undo/redo   Ctrl+Shift+P: commands");
        // Camera position in its most readable unit (far worlds read "10000 km", not "1e+07").
        const glm::dvec3 cam = ctx.camera().position.toMeters();
        const f64 largest = std::max({std::abs(cam.x), std::abs(cam.y), std::abs(cam.z)});
        const LengthUnit u = largest >= 1.0 ? readableUnit(largest) : LengthUnit::Meter;
        char text[128];
        std::snprintf(text, sizeof(text), "%.3f, %.3f, %.3f %s", fromMeters(cam.x, u), fromMeters(cam.y, u),
                      fromMeters(cam.z, u), unitSymbol(u));
        ImGui::SetCursorScreenPos({imagePos.x + avail.x - ImGui::CalcTextSize(text).x - 8, imagePos.y + avail.y - 24});
        ImGui::TextDisabled("%s", text);
    }
}

// --------------------------------------------------------------- hierarchy --
HierarchyPanel::HierarchyPanel() : Panel("Hierarchy", DockSlot::Left) {}

namespace {
constexpr const char* kEntityDragPayload = "NOVA_ENTITY"; // a u64 UUID, dragged in the Hierarchy

// Lists every enabled command under "Entity/" (built-in and game-registered) as create entries.
void createMenuItems(EditorContext& ctx) {
    for (const Command& c : ctx.registry().commands()) {
        if (c.menuPath.rfind("Entity/", 0) != 0) continue;
        if (ImGui::MenuItem(c.menuPath.c_str() + 7, nullptr, false, c.enabled(ctx))) c.execute(ctx);
    }
}
} // namespace

void HierarchyPanel::onImGui(EditorContext& ctx) {
    Scene& scene = ctx.scene();
    // The tree: roots in view order, each child under its parent. `rows` is the display order of
    // every entity (depth first), for Shift ranges.
    std::vector<Entity> all;
    for (auto [handle, name] : scene.registry().view<NameComponent>().each()) all.push_back({handle, &scene});
    std::unordered_map<u64, std::vector<Entity>> kids;
    std::vector<Entity> roots;
    for (Entity e : all) {
        Entity up = scene.parentOf(e);
        if (up.valid() && up != e) kids[up.uuid()].push_back(e);
        else roots.push_back(e);
    }
    std::vector<Entity> rows;
    std::unordered_set<u64> listed;
    std::function<void(Entity)> flatten = [&](Entity e) {
        if (!listed.insert(e.uuid()).second) return;
        rows.push_back(e);
        if (auto it = kids.find(e.uuid()); it != kids.end())
            for (Entity c : it->second) flatten(c);
    };
    for (Entity r : roots) flatten(r);
    for (Entity e : all) // in a loop of parents (a hand-edited file): shown as roots
        if (!listed.contains(e.uuid())) {
            roots.push_back(e);
            flatten(e);
        }

    ImGui::TextDisabled("%s  (%zu entities, %zu selected)", scene.name().c_str(), rows.size(), ctx.selection().size());
    ImGui::Separator();

    std::vector<Entity> toDelete;
    Entity dropOn;       // a row an entity was dropped on: becomes the parent
    u64 dropped = 0;     // the dragged entity
    bool dropToRoot = false;
    std::unordered_set<u64> drawn;
    std::function<void(Entity)> drawNode = [&](Entity e) {
        if (!drawn.insert(e.uuid()).second) return;
        const auto kidsIt = kids.find(e.uuid());
        const bool hasKids = kidsIt != kids.end() && !kidsIt->second.empty();
        const char* icon = e.has<CameraComponent>()             ? "[C] "
                           : e.has<DirectionalLightComponent>() ? "[S] "
                           : e.has<PointLightComponent>()       ? "[L] "
                           : e.has<MeshComponent>()             ? "[M] "
                                                                : "[ ] ";
        const std::string label = std::string(icon) + e.name();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (ctx.isSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;
        const u64 uuid = e.uuid();
        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(uuid)), flags, "%s", label.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && m_anchor) {
                // Range from the anchor to this row (in display order).
                auto anchorIt = std::find_if(rows.begin(), rows.end(), [&](Entity r) { return r.uuid() == m_anchor; });
                auto hereIt = std::find(rows.begin(), rows.end(), e);
                if (anchorIt != rows.end() && hereIt != rows.end()) {
                    auto first = std::min(anchorIt, hereIt), last = std::max(anchorIt, hereIt);
                    std::vector<Entity> range(first, last + 1);
                    std::erase(range, e);
                    range.push_back(e); // clicked row is primary
                    ctx.setSelection(std::move(range));
                }
            } else if (io.KeyCtrl) {
                ctx.toggleSelection(e);
                m_anchor = uuid;
            } else {
                ctx.select(e);
                m_anchor = uuid;
            }
        }
        // Drag a row onto another to make it its child (the selection, if the row is selected).
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kEntityDragPayload, &uuid, sizeof(u64));
            ImGui::TextUnformatted(ctx.isSelected(e) && ctx.selection().size() > 1 ? "Selected entities" : e.name().c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload)) {
                std::memcpy(&dropped, payload->Data, sizeof(u64));
                dropOn = e;
            }
            ImGui::EndDragDropTarget();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (!ctx.isSelected(e)) ctx.select(e);
            if (ImGui::MenuItem("Focus")) ctx.camera().focus(e.transform().position);
            if (ImGui::MenuItem("Duplicate")) ctx.registry().execute("edit.duplicate");
            if (ImGui::MenuItem("Unparent", nullptr, false, scene.parentOf(e).valid())) {
                dropped = uuid;
                dropToRoot = true;
            }
            if (ImGui::MenuItem("Delete")) toDelete = ctx.selectionRoots();
            ImGui::EndPopup();
        }
        if (open && hasKids) {
            for (Entity c : kidsIt->second) drawNode(c);
            ImGui::TreePop();
        }
    };
    for (Entity r : roots) drawNode(r);

    // Below the rows: drop here to make an entity a root again.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, 24.0f)));
    if (ImGui::BeginDragDropTargetCustom(ImRect(at, ImVec2(at.x + std::max(rest.x, 1.0f), at.y + std::max(rest.y, 24.0f))),
                                         ImGui::GetID("##hierarchy_root"))) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityDragPayload)) {
            std::memcpy(&dropped, payload->Data, sizeof(u64));
            dropToRoot = true;
        }
        ImGui::EndDragDropTarget();
    }

    if (!toDelete.empty()) ctx.destroyEntities(toDelete.size() == 1 ? "Delete " + toDelete[0].name() : "Delete entities", toDelete);

    // A drop: the dragged entity (the selection's roots, if it is selected) changes parent,
    // keeping its pose in the world; one undo step.
    if (dropped && (dropOn.valid() || dropToRoot)) {
        Entity dragged = scene.findEntityByUuid(dropped);
        std::vector<Entity> moving = dragged.valid() && ctx.isSelected(dragged) ? ctx.selectionRoots() : std::vector<Entity>{dragged};
        std::erase_if(moving, [&](Entity m) { return !m.valid() || m == dropOn; });
        if (!moving.empty()) {
            const std::string what = moving.size() == 1 ? moving[0].name() : "entities";
            ctx.edit(dropToRoot ? "Unparent " + what : "Parent " + what + " to " + dropOn.name(), moving, [&] {
                for (Entity m : moving) scene.setParent(m, dropToRoot ? Entity{} : dropOn); // refuses loops
            });
        }
    }

    if (ImGui::BeginPopupContextWindow("hierarchy_ctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        createMenuItems(ctx);
        ImGui::EndPopup();
    }
}

// --------------------------------------------------------------- inspector --
InspectorPanel::InspectorPanel() : Panel("Inspector", DockSlot::Right) {}

void InspectorPanel::onImGui(EditorContext& ctx) {
    Entity e = ctx.selected();
    if (!e.valid()) {
        ImGui::TextDisabled("Select an entity.");
        return;
    }
    if (ctx.selection().size() > 1)
        ImGui::TextDisabled("%zu selected - editing the last one (gizmo moves all)", ctx.selection().size());

    auto& name = e.get<NameComponent>().name;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", name.c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##name", buf, sizeof(buf))) name = buf;

    const auto& editors = ctx.registry().componentEditors();
    for (size_t i = 0; i < editors.size(); ++i) {
        const ComponentEditor& ed = editors[i];
        if (!e.valid() || !ed.has(e)) continue;
        bool remove = false;
        ImGui::PushID(ed.name.c_str());
        const bool expanded = widgets::componentHeader(ed.name.c_str(), static_cast<bool>(ed.remove), remove, ed.defaultOpen);
        if (expanded && !remove && ed.draw) ed.draw(e, ctx);
        ImGui::PopID();
        if (remove) ed.remove(e);
    }
    if (!e.valid()) return;

    ImGui::Separator();
    if (ImGui::Button("Add Component", {-1, 0})) ImGui::OpenPopup("add_component");
    if (ImGui::BeginPopup("add_component")) {
        bool any = false;
        for (const ComponentEditor& ed : editors) {
            if (!ed.add || ed.has(e) || (ed.canAdd && !ed.canAdd(e))) continue;
            any = true;
            if (ImGui::MenuItem(ed.name.c_str())) ed.add(e);
        }
        if (!any) ImGui::TextDisabled("Nothing to add");
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------- renderer --
RendererPanel::RendererPanel() : Panel("Renderer", DockSlot::RightBottom) {}

void RendererPanel::onImGui(EditorContext& ctx) {
    auto& dev = ctx.app().device();
    const auto& caps = dev.caps();
    RenderSettings& s = ctx.renderer().settings();
    const RenderStats& st = ctx.renderer().stats();

    ImGui::Text("%s", caps.deviceName.c_str());
    ImGui::TextDisabled("%s | RT: %s%s", caps.apiName.c_str(), caps.rayQuery ? "ray query" : "unsupported",
                        caps.rayTracingPipeline ? " + pipeline" : "");
    ImGui::Text("Draws %u | Tris %llu | TLAS inst %u", st.drawCalls, static_cast<unsigned long long>(st.triangles),
                st.tlasInstances);
    ImGui::Text("GPU culling: %u groups, %u of %u instances visible", st.cullGroups, st.gpuVisible, st.gpuTested);
    ImGui::Text("Graph: %u passes (%u dropped) | %u transients in %u textures", st.graphPasses, st.graphCulled,
                st.transientTextures, st.pooledTextures);
    if (ImGui::TreeNode("Render graph")) {
        ImGui::TextUnformatted(ctx.renderer().lastGraph().c_str());
        ImGui::TreePop();
    }
    ImGui::Text("Physics: %s (%u bodies)", ctx.scene().isSimulating() ? "Jolt, running" : "stopped",
                ctx.scene().physicsBodyCount());
    ImGui::Separator();

    ImGui::BeginDisabled(!caps.rayQuery);
    ImGui::Checkbox("Ray-traced shadows", &s.rayTracedShadows);
    ImGui::EndDisabled();
    ImGui::Checkbox("Shadow maps (without RT)", &s.shadowMaps);
    ImGui::BeginDisabled(!caps.drawIndirectFirstInstance);
    ImGui::Checkbox("GPU culling", &s.gpuCulling);
    ImGui::EndDisabled();
    if (s.shadowMaps) ImGui::SliderFloat("Shadow distance", &s.shadowDistance, 10.0f, 500.0f, "%.0f m");
    ImGui::BeginDisabled(!caps.wireframe);
    ImGui::Checkbox("Wireframe", &s.wireframe);
    ImGui::EndDisabled();
    bool vsync = dev.vsync();
    if (ImGui::Checkbox("VSync", &vsync)) dev.setVSync(vsync);

    const char* views[] = {"Lit", "Normals", "Sun shadows", "Albedo"};
    int dv = static_cast<int>(s.debugView);
    if (ImGui::Combo("Debug view", &dv, views, 4)) s.debugView = static_cast<DebugView>(dv);
    const char* tms[] = {"ACES", "Reinhard", "None"};
    int tm = static_cast<int>(s.tonemapper);
    if (ImGui::Combo("Tonemapper", &tm, tms, 3)) s.tonemapper = static_cast<Tonemapper>(tm);
    ImGui::SliderFloat("Exposure", &s.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Shadow bias", &s.shadowBias, 0.0f, 0.2f, "%.3f");
    if (ImGui::TreeNode("Sky & ambient")) {
        ImGui::ColorEdit3("Zenith", glm::value_ptr(s.skyZenith));
        ImGui::ColorEdit3("Horizon", glm::value_ptr(s.skyHorizon));
        ImGui::ColorEdit3("Ground", glm::value_ptr(s.groundColor));
        ImGui::SliderFloat("Ambient", &s.ambientIntensity, 0.0f, 3.0f);
        ImGui::TreePop();
    }

    ShaderLibrary& lib = ctx.renderer().shaders();
    if (ImGui::TreeNode("postfx", "Post-process (%zu)", s.postProcess.size())) {
        if (s.postProcess.empty()) ImGui::TextDisabled("None. Add with settings().postProcess.push_back(...)");
        for (size_t i = 0; i < s.postProcess.size(); ++i) {
            PostProcessDesc& fx = s.postProcess[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Checkbox(fx.name.empty() ? "(unnamed)" : fx.name.c_str(), &fx.enabled);
            if (!lib.ready(fx.shader)) {
                ImGui::SameLine();
                ImGui::TextColored({1.0f, 0.45f, 0.4f, 1.0f}, "(not compiled)");
            }
            ImGui::DragFloat4("Params", glm::value_ptr(fx.params), 0.01f);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("shaders", "Game shaders (%zu)", lib.shaders().size())) {
        bool hot = lib.hotReload();
        if (ImGui::Checkbox("Hot reload", &hot)) lib.setHotReload(hot);
        ImGui::SameLine();
        ImGui::TextDisabled(ShaderLibrary::compilerAvailable() ? "runtime compiler: on" : "runtime compiler: off");
        for (ShaderHandle h : lib.shaders()) {
            ImGui::PushID(static_cast<int>(h.id));
            const bool failed = !lib.error(h).empty();
            const char* src = lib.source(h) == ShaderSource::Runtime ? "runtime"
                              : lib.source(h) == ShaderSource::Precompiled ? "spv" : "memory";
            ImGui::TextColored(failed ? ImVec4{1.0f, 0.45f, 0.4f, 1.0f} : ImVec4{0.8f, 0.8f, 0.82f, 1.0f}, "%s",
                               lib.path(h).filename().string().c_str());
            if (failed && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lib.error(h).c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s v%u", src, lib.version(h));
            ImGui::SameLine();
            if (ImGui::SmallButton("Reload")) lib.reload(h);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

// ------------------------------------------------------------------- world --
namespace {
// "4.29 km", "3.81 um": a length in its most readable unit.
std::string readable(f64 meters) {
    const LengthUnit u = readableUnit(meters);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4g %s", fromMeters(meters, u), unitSymbol(u));
    return buf;
}
// The world's size and every position in it: shrinking the world brings positions inside, so an
// undo puts back both, bit for bit.
struct WorldState {
    WorldSize size;
    std::vector<std::pair<entt::entity, GlobalPosition>> positions;

    static WorldState of(Scene& scene) {
        WorldState s{scene.worldSize(), {}};
        for (auto [e, t] : scene.registry().view<TransformComponent>().each()) s.positions.emplace_back(e, t.position);
        return s;
    }
    void apply(Scene& scene) const {
        scene.setWorldSize(size);
        auto& reg = scene.registry();
        for (const auto& [e, p] : positions)
            if (auto* t = reg.valid(e) ? reg.try_get<TransformComponent>(e) : nullptr) t->position = p.rewrapped();
    }
};
} // namespace

WorldPanel::WorldPanel() : Panel("World", DockSlot::RightBottom) {}

void WorldPanel::onImGui(EditorContext& ctx) {
    Scene& scene = ctx.scene();
    ImGui::TextDisabled("Circular on every axis  |  step %s", readable(WorldSpace::kUnit).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Positions are Q45.18 fixed point (int64): leaving one side enters the other");
    ImGui::Separator();

    // Size per axis. One undo step per edit (a drag is one edit); during Play the change applies
    // to the running world and goes away on Stop, like any other edit.
    static WorldState before;
    static bool editing = false;
    WorldSize size = scene.worldSize();
    bool changed = false, finished = false;
    const char* axisNames[3] = {"X", "Y", "Z"};
    if (!editing) before = WorldState::of(scene);
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(axisNames[i]);
        ImGui::SameLine(30.0f);
        ImGui::SetNextItemWidth(-1.0f);
        f64 meters = size.size(i);
        const f64 lo = WorldSpace::kMinSize, hi = WorldSpace::kMaxSize;
        if (ImGui::DragScalar("##size", ImGuiDataType_Double, &meters, 0.5f, &lo, &hi, "%.2f m",
                              ImGuiSliderFlags_Logarithmic)) {
            size.steps[i] = WorldSize::toSteps(meters);
            changed = true;
            editing = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) finished = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s: leaving +size/2 enters at -size/2", readable(size.size(i)).c_str());
        ImGui::PopID();
    }
    if (changed) scene.setWorldSize(size);
    auto pushUndo = [&ctx](const WorldState& old) {
        const WorldState now = WorldState::of(ctx.scene());
        if (now.size == old.size || ctx.isPlaying()) return;
        ctx.undoStack().push({"World Size", [&ctx, old] { old.apply(ctx.scene()); },
                              [&ctx, now] { now.apply(ctx.scene()); }});
    };
    if (finished) {
        editing = false;
        pushUndo(before);
    }
    if (ImGui::SmallButton("Default (16 km)")) {
        const WorldState old = WorldState::of(scene);
        scene.setWorldSize({});
        pushUndo(old);
    }
    ImGui::Separator();
    const glm::dvec3 cam = ctx.camera().position.toMeters();
    ImGui::Text("Camera  %s, %s, %s", readable(cam.x).c_str(), readable(cam.y).c_str(), readable(cam.z).c_str());
    if (scene.isSimulating()) {
        const glm::dvec3 o = scene.worldOrigin().toMeters();
        ImGui::Text("World origin  %s, %s, %s", readable(o.x).c_str(), readable(o.y).c_str(), readable(o.z).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("What the physics engine's (0, 0, 0) is: it follows the focus on a grid of cells");
    }
}

// ----------------------------------------------------------------- console --
ConsolePanel::ConsolePanel() : Panel("Console", DockSlot::Bottom) {}

void ConsolePanel::onImGui(EditorContext&) {
    ImGui::Checkbox("Auto-scroll", &autoScroll);
    ImGui::Separator();
    ImGui::BeginChild("log", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    for (const LogEntry& entry : Log::recent(400)) {
        ImVec4 color{0.8f, 0.8f, 0.82f, 1.0f};
        if (entry.level >= spdlog::level::err) color = {1.0f, 0.4f, 0.4f, 1.0f};
        else if (entry.level == spdlog::level::warn) color = {1.0f, 0.8f, 0.35f, 1.0f};
        else if (entry.level <= spdlog::level::debug) color = {0.55f, 0.55f, 0.6f, 1.0f};
        ImGui::TextColored(color, "%s", entry.text.c_str());
    }
    if (autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ----------------------------------------------------------------- history --
HistoryPanel::HistoryPanel() : Panel("History", DockSlot::Left) {}

void HistoryPanel::onImGui(EditorContext& ctx) {
    UndoStack& undo = ctx.undoStack();
    ImGui::BeginDisabled(ctx.isPlaying());
    if (ImGui::Button("Undo") && undo.canUndo()) ctx.undo();
    ImGui::SameLine();
    if (ImGui::Button("Redo") && undo.canRedo()) ctx.redo();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) undo.clear();
    ImGui::EndDisabled();
    ImGui::Separator();

    // Position 0 = initial state; row i+1 = after action i.
    if (ImGui::Selectable("(initial state)", undo.position() == 0) && !ctx.isPlaying()) undo.jumpTo(0);
    const auto& actions = undo.actions();
    for (size_t i = 0; i < actions.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool applied = i < undo.position();
        if (!applied) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(actions[i].name.c_str(), undo.position() == i + 1) && !ctx.isPlaying()) undo.jumpTo(i + 1);
        if (!applied) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (ctx.isPlaying()) ImGui::TextDisabled("History is frozen while playing.");
}

// ------------------------------------------------------------------ plugin --
void CorePanelsPlugin::onRegister(Registry& r) {
    r.panel<ViewportPanel>();
    r.panel<HierarchyPanel>();
    r.panel<InspectorPanel>();
    r.panel<RendererPanel>();
    r.panel<WorldPanel>();
    r.panel<ConsolePanel>();
    r.panel<HistoryPanel>();
}

} // namespace nova::editor::builtin
