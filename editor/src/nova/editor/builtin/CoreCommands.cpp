#include "nova/editor/builtin/CoreCommands.h"

#include "nova/assets/GltfLoader.h"
#include "nova/core/Application.h"
#include "nova/editor/EditorContext.h"

#include <imgui.h>

#include <algorithm>

namespace nova::editor::builtin {

namespace {
glm::vec3 spawnPoint(EditorContext& ctx) {
    const EditorCamera& cam = ctx.camera();
    glm::vec3 p = cam.position + cam.forward() * 6.0f;
    p.y = std::max(p.y, 0.5f);
    return p;
}

Command make(const char* id, const char* menuPath, std::function<void(EditorContext&)> fn, const char* shortcut = nullptr,
             int order = 0) {
    Command c;
    c.id = id;
    c.menuPath = menuPath;
    c.execute = std::move(fn);
    c.order = order;
    if (shortcut) c.shortcut = *KeyChord::parse(shortcut);
    return c;
}

bool hasSelection(EditorContext& ctx) { return ctx.selected().valid(); }
} // namespace

Entity createMeshEntity(EditorContext& ctx, const std::string& name, MeshHandle mesh) {
    Entity e = ctx.scene().createEntity(name);
    e.transform().position = spawnPoint(ctx);
    if (mesh) e.add<MeshComponent>(mesh);
    ctx.select(e);
    return e;
}

void CoreCommandsPlugin::onRegister(Registry& r) {
    // ---- File
    r.command(make("file.importGltf", "File/Import glTF...", [this](EditorContext&) { m_openImport = true; }, "Ctrl+I"));
    Command exit = make("file.exit", "File/Exit", [](EditorContext& ctx) { ctx.app().quit(); }, nullptr, 100);
    exit.separatorBefore = true;
    r.command(std::move(exit));

    // ---- Edit
    Command del = make("edit.delete", "Edit/Delete", [](EditorContext& ctx) {
        Entity e = ctx.selected();
        ctx.clearSelection();
        ctx.scene().destroyEntity(e);
    }, "Delete");
    del.isEnabled = hasSelection;
    r.command(std::move(del));
    Command focus = make("edit.focus", "Edit/Focus Selection",
                         [](EditorContext& ctx) { ctx.camera().focus(ctx.selected().transform().position); }, "F", 1);
    focus.isEnabled = hasSelection;
    r.command(std::move(focus));
    Command deselect = make("edit.deselect", "Edit/Deselect", [](EditorContext& ctx) { ctx.clearSelection(); }, "Escape", 2);
    deselect.isEnabled = hasSelection;
    r.command(std::move(deselect));

    // ---- Entity (also listed in the Hierarchy's context menu)
    r.command(make("entity.empty", "Entity/Create Empty",
                   [](EditorContext& ctx) { createMeshEntity(ctx, "Entity", {}); }, nullptr, 0));
    r.command(make("entity.cube", "Entity/Create Cube",
                   [](EditorContext& ctx) { createMeshEntity(ctx, "Cube", ctx.renderer().cube()); }, nullptr, 1));
    r.command(make("entity.sphere", "Entity/Create Sphere",
                   [](EditorContext& ctx) { createMeshEntity(ctx, "Sphere", ctx.renderer().sphere()); }, nullptr, 2));
    r.command(make("entity.plane", "Entity/Create Plane",
                   [](EditorContext& ctx) { createMeshEntity(ctx, "Plane", ctx.renderer().plane()); }, nullptr, 3));
    Command light = make("entity.pointLight", "Entity/Create Point Light", [](EditorContext& ctx) {
        Entity e = createMeshEntity(ctx, "Point Light", {});
        e.add<PointLightComponent>();
    }, nullptr, 10);
    light.separatorBefore = true;
    r.command(std::move(light));

    // ---- Play
    Command play = make("play.play", "Play/Play", [](EditorContext& ctx) { ctx.play(); }, "F5", 0);
    play.isEnabled = [](EditorContext& ctx) { return ctx.playState() != PlayState::Play; };
    r.command(std::move(play));
    Command pause = make("play.pause", "Play/Pause", [](EditorContext& ctx) { ctx.pause(); }, "F6", 1);
    pause.isEnabled = [](EditorContext& ctx) { return ctx.playState() == PlayState::Play; };
    r.command(std::move(pause));
    Command stop = make("play.stop", "Play/Stop", [](EditorContext& ctx) { ctx.stop(); }, "Shift+F5", 2);
    stop.isEnabled = [](EditorContext& ctx) { return ctx.isPlaying(); };
    r.command(std::move(stop));

    // ---- Menu bar: play controls + status
    r.menuBarWidget([](EditorContext& ctx) {
        ImGui::Separator();
        const PlayState s = ctx.playState();
        if (s != PlayState::Play && ImGui::MenuItem(s == PlayState::Pause ? "Resume" : "Play")) ctx.play();
        if (s == PlayState::Play && ImGui::MenuItem("Pause")) ctx.pause();
        ImGui::BeginDisabled(s == PlayState::Edit);
        if (ImGui::MenuItem("Stop")) ctx.stop();
        ImGui::EndDisabled();
        const ImVec4 color = s == PlayState::Edit ? ImVec4(0.6f, 0.6f, 0.65f, 1) : ImVec4(0.4f, 0.9f, 0.5f, 1);
        ImGui::TextColored(color, "%s", s == PlayState::Edit ? "EDIT" : s == PlayState::Play ? "PLAYING" : "PAUSED");
        ImGui::Separator();
        ImGui::TextDisabled("%.0f FPS  %.2f ms", ctx.app().fps(), ctx.app().deltaTime() * 1000.0f);
    });

    // ---- glTF import dialog
    r.uiHook([this](EditorContext& ctx) {
        if (m_openImport) {
            ImGui::OpenPopup("Import glTF");
            m_openImport = false;
        }
        if (!ImGui::BeginPopupModal("Import glTF", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::TextUnformatted("Path to a .gltf or .glb file:");
        ImGui::SetNextItemWidth(500.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##path", m_importPath, sizeof(m_importPath));
        if (ImGui::Button("Import", {120, 0})) {
            if (auto root = loadGltf(ctx.scene(), ctx.renderer(), m_importPath)) ctx.select(*root);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    });
}

} // namespace nova::editor::builtin
