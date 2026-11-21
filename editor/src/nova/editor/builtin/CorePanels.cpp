#include "nova/editor/builtin/CorePanels.h"

#include "nova/core/Application.h"
#include "nova/core/Log.h"
#include "nova/editor/Picking.h"
#include "nova/editor/Tool.h"
#include "nova/editor/Widgets.h"
#include "nova/editor/builtin/CoreTools.h"

#include <imgui.h> // must precede ImGuizmo.h
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
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

    glm::vec3 position = vp.rayOrigin + vp.rayDirection * 6.0f;
    bool onSurface = false;
    if (hit) {
        position = hit->point;
        onSurface = hit->normal.y > 0.5f;
    } else if (vp.rayDirection.y < -1e-4f) {
        position = vp.rayOrigin + vp.rayDirection * (-vp.rayOrigin.y / vp.rayDirection.y);
        onSurface = true;
    }
    ctx.instantiateAsset(guid, position, [&](std::vector<Entity>& created) {
        if (!onSurface) return;
        f32 minY = std::numeric_limits<f32>::max();
        for (Entity e : created) {
            const auto* mesh = e.tryGet<MeshComponent>();
            if (!mesh) continue;
            const Bounds b = renderer.meshBounds(mesh->mesh);
            const glm::mat4 m = e.transform().matrix();
            for (int i = 0; i < 8; ++i) {
                const glm::vec3 corner{i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z};
                minY = std::min(minY, glm::vec3(m * glm::vec4(corner, 1.0f)).y);
            }
        }
        if (minY == std::numeric_limits<f32>::max()) return;
        for (Entity e : created) e.transform().position.y += position.y - minY; // rest on the surface
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
        vp.rayOrigin = glm::vec3(nearP) / nearP.w;
        vp.rayDirection = glm::normalize(glm::vec3(farP) / farP.w - vp.rayOrigin);
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
    }
}

// --------------------------------------------------------------- hierarchy --
HierarchyPanel::HierarchyPanel() : Panel("Hierarchy", DockSlot::Left) {}

namespace {
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
    // Display order: same as the view iteration, captured once so Shift ranges are well defined.
    std::vector<Entity> rows;
    for (auto [handle, name] : scene.registry().view<NameComponent>().each()) rows.push_back({handle, &scene});

    ImGui::TextDisabled("%s  (%zu entities, %zu selected)", scene.name().c_str(), rows.size(), ctx.selection().size());
    ImGui::Separator();

    std::vector<Entity> toDelete;
    for (size_t i = 0; i < rows.size(); ++i) {
        Entity e = rows[i];
        ImGui::PushID(static_cast<int>(entt::to_integral(e.handle())));
        const char* icon = e.has<CameraComponent>()             ? "[C] "
                           : e.has<DirectionalLightComponent>() ? "[S] "
                           : e.has<PointLightComponent>()       ? "[L] "
                           : e.has<MeshComponent>()             ? "[M] "
                                                                : "[ ] ";
        const std::string label = std::string(icon) + e.name();
        if (ImGui::Selectable(label.c_str(), ctx.isSelected(e))) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift && m_anchor) {
                // Range from the anchor to this row (in display order).
                auto anchorIt = std::find_if(rows.begin(), rows.end(), [&](Entity r) { return r.uuid() == m_anchor; });
                if (anchorIt != rows.end()) {
                    size_t a = static_cast<size_t>(anchorIt - rows.begin()), b = i;
                    if (a > b) std::swap(a, b);
                    std::vector<Entity> range(rows.begin() + static_cast<std::ptrdiff_t>(a),
                                              rows.begin() + static_cast<std::ptrdiff_t>(b + 1));
                    std::erase(range, e);
                    range.push_back(e); // clicked row is primary
                    ctx.setSelection(std::move(range));
                }
            } else if (io.KeyCtrl) {
                ctx.toggleSelection(e);
                m_anchor = e.uuid();
            } else {
                ctx.select(e);
                m_anchor = e.uuid();
            }
        }
        if (ImGui::BeginPopupContextItem()) {
            if (!ctx.isSelected(e)) ctx.select(e);
            if (ImGui::MenuItem("Focus")) ctx.camera().focus(e.transform().position);
            if (ImGui::MenuItem("Duplicate")) ctx.registry().execute("edit.duplicate");
            if (ImGui::MenuItem("Delete")) toDelete = ctx.selection();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!toDelete.empty()) ctx.destroyEntities(toDelete.size() == 1 ? "Delete " + toDelete[0].name() : "Delete entities", toDelete);

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
    ImGui::Text("Physics: %s (%u bodies)", ctx.scene().isSimulating() ? "Jolt, running" : "stopped",
                ctx.scene().physics() ? ctx.scene().physics()->bodyCount() : 0u);
    ImGui::Separator();

    ImGui::BeginDisabled(!caps.rayQuery);
    ImGui::Checkbox("Ray-traced shadows", &s.rayTracedShadows);
    ImGui::EndDisabled();
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
    r.panel<ConsolePanel>();
    r.panel<HistoryPanel>();
}

} // namespace nova::editor::builtin
