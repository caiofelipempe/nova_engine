#include "nova/editor/builtin/CorePanels.h"

#include "nova/core/Application.h"
#include "nova/core/Log.h"
#include "nova/editor/Widgets.h"

#include <imgui.h> // must precede ImGuizmo.h
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>
#include <cstdio>

namespace nova::editor::builtin {

// ---------------------------------------------------------------- viewport --
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
    m_hovered = ImGui::IsWindowHovered();
    const ImVec2 imagePos = ImGui::GetCursorScreenPos();
    if (auto* imgui = ctx.app().imgui()) ImGui::Image(static_cast<ImTextureID>(imgui->textureId(m_view->output())), avail);

    // Toolbar overlay
    ImGui::SetCursorScreenPos({imagePos.x + 8, imagePos.y + 8});
    const char* modes[] = {"Move (W)", "Rotate (E)", "Scale (R)"};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        const bool active = static_cast<int>(gizmoMode) == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(modes[i])) gizmoMode = static_cast<GizmoMode>(i);
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Local", &gizmoLocal);
    ImGui::SameLine();
    ImGui::Checkbox("Scene camera", &useSceneCamera);

    // Transform gizmo
    Entity selected = ctx.selected();
    if (selected.valid() && !useSceneCamera) {
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(imagePos.x, imagePos.y, avail.x, avail.y);
        const f32 aspect = avail.y > 0 ? avail.x / avail.y : 1.0f;
        CameraData cam = ctx.camera().data(aspect);
        glm::mat4 proj = cam.projection;
        proj[1][1] *= -1.0f; // ImGuizmo expects GL-style Y-up clip space
        auto& t = selected.transform();
        glm::mat4 model = t.matrix();
        const ImGuizmo::OPERATION op = gizmoMode == GizmoMode::Translate ? ImGuizmo::TRANSLATE
                                       : gizmoMode == GizmoMode::Rotate  ? ImGuizmo::ROTATE
                                                                         : ImGuizmo::SCALE;
        const bool snap = ImGui::GetIO().KeyCtrl;
        const float snapValue = gizmoMode == GizmoMode::Rotate ? 15.0f : 0.5f;
        const float snaps[3] = {snapValue, snapValue, snapValue};
        if (ImGuizmo::Manipulate(glm::value_ptr(cam.view), glm::value_ptr(proj), op,
                                 gizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(model), nullptr,
                                 snap ? snaps : nullptr)) {
            glm::vec3 scale, translation, skew;
            glm::vec4 persp;
            glm::quat rot;
            if (glm::decompose(model, scale, rot, translation, skew, persp)) {
                t.position = translation;
                t.rotation = glm::eulerAngles(rot);
                t.scale = scale;
                if (ctx.scene().isSimulating()) ctx.scene().syncTransformToPhysics(selected);
            }
        }
    }

    if (!ctx.inputCaptured()) {
        ImGui::SetCursorScreenPos({imagePos.x + 8, imagePos.y + avail.y - 24});
        ImGui::TextDisabled("RMB + WASD/QE: fly   Wheel: speed   F: focus   Ctrl: snap   Ctrl+Shift+P: commands");
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
    ImGui::TextDisabled("%s  (%zu entities)", scene.name().c_str(),
                        static_cast<size_t>(scene.registry().view<NameComponent>().size()));
    ImGui::Separator();

    Entity toDelete;
    for (auto [handle, name] : scene.registry().view<NameComponent>().each()) {
        Entity e{handle, &scene};
        ImGui::PushID(static_cast<int>(entt::to_integral(handle)));
        const char* icon = e.has<CameraComponent>()             ? "[C] "
                           : e.has<DirectionalLightComponent>() ? "[S] "
                           : e.has<PointLightComponent>()       ? "[L] "
                           : e.has<MeshComponent>()             ? "[M] "
                                                                : "[ ] ";
        const std::string label = std::string(icon) + name.name;
        if (ImGui::Selectable(label.c_str(), ctx.selected() == e)) ctx.select(e);
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Focus")) ctx.camera().focus(e.transform().position);
            if (ImGui::MenuItem("Delete")) toDelete = e;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (toDelete.valid()) {
        if (ctx.selected() == toDelete) ctx.clearSelection();
        scene.destroyEntity(toDelete);
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

// ------------------------------------------------------------------ plugin --
void CorePanelsPlugin::onRegister(Registry& r) {
    r.panel<ViewportPanel>();
    r.panel<HierarchyPanel>();
    r.panel<InspectorPanel>();
    r.panel<RendererPanel>();
    r.panel<ConsolePanel>();

    auto setGizmo = [&r](GizmoMode mode) {
        return [&r, mode](EditorContext&) {
            if (auto* vp = r.findPanel<ViewportPanel>()) vp->gizmoMode = mode;
        };
    };
    auto isGizmo = [&r](GizmoMode mode) {
        return [&r, mode](EditorContext&) {
            auto* vp = r.findPanel<ViewportPanel>();
            return vp && vp->gizmoMode == mode;
        };
    };
    const std::pair<const char*, GizmoMode> modes[] = {
        {"Translate", GizmoMode::Translate}, {"Rotate", GizmoMode::Rotate}, {"Scale", GizmoMode::Scale}};
    const char* keys[] = {"W", "E", "R"};
    for (int i = 0; i < 3; ++i) {
        Command c;
        c.id = std::string("viewport.") + modes[i].first;
        c.menuPath = std::string("View/Gizmo/") + modes[i].first;
        c.shortcut = *KeyChord::parse(keys[i]);
        c.order = i;
        c.execute = setGizmo(modes[i].second);
        c.isChecked = isGizmo(modes[i].second);
        r.command(std::move(c));
    }
    Command local;
    local.id = "viewport.localSpace";
    local.menuPath = "View/Gizmo/Local Space";
    local.separatorBefore = true;
    local.order = 10;
    local.execute = [&r](EditorContext&) {
        if (auto* vp = r.findPanel<ViewportPanel>()) vp->gizmoLocal = !vp->gizmoLocal;
    };
    local.isChecked = [&r](EditorContext&) {
        auto* vp = r.findPanel<ViewportPanel>();
        return vp && vp->gizmoLocal;
    };
    r.command(std::move(local));
}

} // namespace nova::editor::builtin
