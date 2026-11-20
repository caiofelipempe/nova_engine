#include "nova/editor/builtin/CoreCommands.h"

#include "nova/assets/GltfLoader.h"
#include "nova/core/Application.h"
#include "nova/core/Log.h"
#include "nova/editor/EditorContext.h"
#include "nova/renderer/Renderer.h"
#include "nova/scene/SceneSerializer.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

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
bool editing(EditorContext& ctx) { return !ctx.isPlaying(); }

std::string defaultScenePath(EditorContext& ctx) {
    if (!ctx.scenePath().empty()) return ctx.scenePath().string();
    return "scenes/" + ctx.scene().name() + ".scene.json";
}
} // namespace

Entity createMeshEntity(EditorContext& ctx, const std::string& name, MeshHandle mesh) {
    Entity e = ctx.scene().createEntity(name);
    e.transform().position = spawnPoint(ctx);
    if (mesh) e.add<MeshComponent>(mesh);
    ctx.select(e);
    return e;
}

void CoreCommandsPlugin::requestPrompt(Prompt prompt, const std::string& initialPath) {
    m_prompt = prompt;
    m_openPrompt = true;
    m_error.clear();
    std::snprintf(m_path, sizeof(m_path), "%s", initialPath.c_str());
}

void CoreCommandsPlugin::onRegister(Registry& r) {
    // ---- File
    Command newScene = make("file.new", "File/New Scene", [](EditorContext& ctx) {
        ctx.newScene();
        Entity cam = ctx.scene().createEntity("Main Camera");
        cam.add<CameraComponent>();
        cam.transform().position = {0.0f, 2.0f, 8.0f};
        cam.transform().lookAt({0.0f, 0.5f, 0.0f});
        Entity sun = ctx.scene().createEntity("Sun");
        sun.add<DirectionalLightComponent>();
        sun.transform().rotation = {glm::radians(-45.0f), glm::radians(30.0f), 0.0f};
    }, "Ctrl+N", 0);
    newScene.isEnabled = editing;
    r.command(std::move(newScene));

    Command open = make("file.open", "File/Open Scene...",
                        [this](EditorContext& ctx) { requestPrompt(Prompt::Open, defaultScenePath(ctx)); }, "Ctrl+O", 1);
    open.isEnabled = editing;
    r.command(std::move(open));

    Command save = make("file.save", "File/Save Scene", [this](EditorContext& ctx) {
        if (ctx.scenePath().empty()) {
            requestPrompt(Prompt::SaveAs, defaultScenePath(ctx));
            return;
        }
        std::string error;
        if (ctx.saveScene(ctx.scenePath(), &error)) NOVA_CORE_INFO("Saved {}", ctx.scenePath().string());
        else NOVA_CORE_ERROR("Save failed: {}", error);
    }, "Ctrl+S", 2);
    save.separatorBefore = true;
    save.isEnabled = editing;
    r.command(std::move(save));

    Command saveAs = make("file.saveAs", "File/Save Scene As...",
                          [this](EditorContext& ctx) { requestPrompt(Prompt::SaveAs, defaultScenePath(ctx)); },
                          "Ctrl+Shift+S", 3);
    saveAs.isEnabled = editing;
    r.command(std::move(saveAs));

    Command import = make("file.importGltf", "File/Import glTF...",
                          [this](EditorContext&) { requestPrompt(Prompt::ImportGltf, ""); }, "Ctrl+I", 10);
    import.separatorBefore = true;
    r.command(std::move(import));

    Command exit = make("file.exit", "File/Exit", [](EditorContext& ctx) { ctx.app().quit(); }, nullptr, 100);
    exit.separatorBefore = true;
    r.command(std::move(exit));

    // ---- Edit
    Command dup = make("edit.duplicate", "Edit/Duplicate", [](EditorContext& ctx) {
        ctx.select(duplicateEntity(ctx.selected()));
    }, "Ctrl+D", 0);
    dup.isEnabled = hasSelection;
    r.command(std::move(dup));
    Command del = make("edit.delete", "Edit/Delete", [](EditorContext& ctx) {
        Entity e = ctx.selected();
        ctx.clearSelection();
        ctx.scene().destroyEntity(e);
    }, "Delete", 1);
    del.isEnabled = hasSelection;
    r.command(std::move(del));
    Command focus = make("edit.focus", "Edit/Focus Selection",
                         [](EditorContext& ctx) { ctx.camera().focus(ctx.selected().transform().position); }, "F", 2);
    focus.isEnabled = hasSelection;
    focus.separatorBefore = true;
    r.command(std::move(focus));
    Command deselect = make("edit.deselect", "Edit/Deselect", [](EditorContext& ctx) { ctx.clearSelection(); }, "Escape", 3);
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
        ImGui::TextDisabled("%s", ctx.scenePath().empty() ? (ctx.scene().name() + " (unsaved)").c_str()
                                                          : ctx.scenePath().string().c_str());
        ImGui::Separator();
        ImGui::TextDisabled("%.0f FPS  %.2f ms", ctx.app().fps(), ctx.app().deltaTime() * 1000.0f);
    });

    r.uiHook([this](EditorContext& ctx) { drawPrompt(ctx); });
}

void CoreCommandsPlugin::drawPrompt(EditorContext& ctx) {
    static constexpr const char* kTitles[] = {"", "Open Scene", "Save Scene As", "Import glTF"};
    const char* title = kTitles[static_cast<int>(m_prompt)];
    if (m_openPrompt) {
        ImGui::OpenPopup(title);
        m_openPrompt = false;
    }
    if (m_prompt == Prompt::None || !ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted(m_prompt == Prompt::ImportGltf ? "Path to a .gltf or .glb file:"
                                                          : "Scene file (.scene.json), relative to the editor folder:");
    ImGui::SetNextItemWidth(520.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##path", m_path, sizeof(m_path), ImGuiInputTextFlags_EnterReturnsTrue);
    if (!m_error.empty()) ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", m_error.c_str());

    const char* action = m_prompt == Prompt::Open ? "Open" : m_prompt == Prompt::SaveAs ? "Save" : "Import";
    if (ImGui::Button(action, {120, 0}) || enter) {
        bool ok = false;
        std::string error;
        switch (m_prompt) {
            case Prompt::Open: ok = ctx.openScene(m_path, &error); break;
            case Prompt::SaveAs: ok = ctx.saveScene(m_path, &error); break;
            case Prompt::ImportGltf:
                if (auto root = loadGltf(ctx.scene(), ctx.renderer(), m_path)) {
                    ctx.select(*root);
                    ok = true;
                } else error = "import failed (see console)";
                break;
            case Prompt::None: break;
        }
        if (ok) {
            NOVA_CORE_INFO("{}: {}", action, m_path);
            m_prompt = Prompt::None;
            ImGui::CloseCurrentPopup();
        } else {
            m_error = error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {120, 0})) {
        m_prompt = Prompt::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace nova::editor::builtin
