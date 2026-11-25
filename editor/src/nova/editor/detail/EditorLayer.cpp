#include "nova/editor/detail/EditorLayer.h"

#include "nova/core/Log.h"
#include "nova/renderer/Renderer.h"
#include "nova/editor/builtin/CoreAssets.h"
#include "nova/editor/builtin/CoreCommands.h"
#include "nova/editor/builtin/CoreComponents.h"
#include "nova/editor/builtin/CorePanels.h"
#include "nova/editor/builtin/CoreTools.h"
#include "nova/editor/builtin/CoreVisuals.h"
#include "nova/platform/Input.h"

#include <imgui.h> // must precede ImGuizmo.h
#include <ImGuizmo.h>
#include <imgui_internal.h> // DockBuilder

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

namespace nova::editor {

namespace detail {

void pushEditor(Application& app, const EditorDesc& desc, Scope<EditorApp> editor) {
    app.pushLayer<EditorLayer>(desc, std::move(editor));
}

namespace {

// Menu tree built from command menu paths each frame (cheap: a few dozen nodes).
struct MenuNode {
    std::string name;
    const Command* command = nullptr; // leaf
    std::vector<MenuNode> children;   // submenu
    int order = 0;
};

MenuNode& child(MenuNode& parent, const std::string& name, int order) {
    for (auto& c : parent.children)
        if (!c.command && c.name == name) return c;
    MenuNode node;
    node.name = name;
    node.order = order;
    parent.children.push_back(std::move(node));
    return parent.children.back();
}

void sortTree(MenuNode& node, bool topLevel) {
    // Top level keeps registration order (File, Edit, ...); inside menus sort by `order` (stable).
    if (!topLevel)
        std::stable_sort(node.children.begin(), node.children.end(),
                         [](const MenuNode& a, const MenuNode& b) { return a.order < b.order; });
    for (auto& c : node.children) sortTree(c, false);
}

bool containsCaseInsensitive(const std::string& haystack, const char* needle) {
    if (!*needle) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle, needle + std::strlen(needle),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

} // namespace

EditorLayer::EditorLayer(const EditorDesc& desc, Scope<EditorApp> app)
    : Layer("Editor"), m_desc(desc), m_app(std::move(app)) {
    m_context.setRegistry(&m_registry);
}

EditorLayer::~EditorLayer() = default;

void EditorLayer::onAttach() {
    // Per-game layout file. ImGui loads it lazily on the first NewFrame.
    ImGui::GetIO().IniFilename = m_desc.layoutFile.c_str();

    // Assets first: plugins and onSetup may add importers or look assets up.
    m_context.assets().setRenderer(&Application::get().renderer());
    m_context.assets().setRoot(m_desc.assetsDir);
    NOVA_CORE_INFO("Assets: {} ({} assets)", m_context.assets().root().string(), m_context.assets().assets().size());

    // Order matters only for menu order (File, Edit, Entity, Play, View, ...).
    if (m_desc.builtinCommands) {
        m_registry.plugin<builtin::CoreCommandsPlugin>();
        registerFrameworkCommands();
    }
    if (m_desc.builtinPanels) {
        m_registry.plugin<builtin::CorePanelsPlugin>();
        m_registry.plugin<builtin::CoreToolsPlugin>();
        m_registry.plugin<builtin::CoreVisualsPlugin>();
        m_registry.plugin<builtin::CoreAssetsPlugin>();
    }
    if (m_desc.builtinComponents) m_registry.plugin<builtin::CoreComponentsPlugin>();

    m_app->onSetup(m_registry);
    setupGameModule();
    // Every reflected component/script gets an inspector, unless one with that name was registered.
    m_registry.addReflectedEditors();
    m_registry.attachAll();
    m_app->onNewScene(m_context);
    if (m_gameModule && m_gameModule->module()) {
        ModuleScope scope(m_gameModule->moduleId());
        m_gameModule->module()->onNewScene(m_context);
    }
    NOVA_CORE_INFO("Editor ready: {} panels, {} commands, {} component editors", m_registry.panels().size(),
                   m_registry.commands().size(), m_registry.componentEditors().size());
}

void EditorLayer::onDetach() {
    m_context.stop();
    m_context.setGameModule(nullptr);
    m_gameModule.reset(); // unload the game code while its panels can still detach
    m_registry.detachAll();
    m_scene.clear();
    m_scene.stopSimulation();
    // The ImGui context outlives this layer: save now and stop it from using our string.
    ImGui::SaveIniSettingsToDisk(m_desc.layoutFile.c_str());
    ImGui::GetIO().IniFilename = nullptr;
}

void EditorLayer::setupGameModule() {
    const GameModuleDesc& desc = m_desc.gameModule;
    if (desc.library.empty()) return;
    m_gameModule = makeScope<GameModuleHost>(m_context, m_registry);
    m_gameModule->buildCommand = desc.buildCommand;
    m_gameModule->watch = desc.watch;
    m_context.setGameModule(m_gameModule.get());
    std::string error;
    if (!m_gameModule->load(desc.library, &error)) NOVA_CORE_ERROR("Game module: {}", error);

    GameModuleHost* host = m_gameModule.get();
    Command reload;
    reload.id = "game.reload";
    reload.menuPath = "Game/Reload Code";
    reload.shortcut = *KeyChord::parse("Ctrl+R");
    reload.execute = [host](EditorContext&) {
        std::string err;
        if (!host->reload(&err) && !err.empty()) NOVA_CORE_ERROR("Reload: {}", err);
    };
    reload.isEnabled = [host](EditorContext&) { return !host->isBuilding(); };
    m_registry.command(std::move(reload));

    Command build;
    build.id = "game.build";
    build.menuPath = "Game/Build and Reload";
    build.shortcut = *KeyChord::parse("Ctrl+B");
    build.order = 1;
    build.execute = [host](EditorContext&) { host->build(); };
    build.isEnabled = [host](EditorContext&) { return !host->isBuilding() && !host->buildCommand.empty(); };
    m_registry.command(std::move(build));

    Command watch;
    watch.id = "game.watch";
    watch.menuPath = "Game/Reload When the DLL Changes";
    watch.order = 2;
    watch.separatorBefore = true;
    watch.execute = [host](EditorContext&) { host->watch = !host->watch; };
    watch.isChecked = [host](EditorContext&) { return host->watch; };
    m_registry.command(std::move(watch));

    m_registry.menuBarWidget([host](EditorContext&) {
        ImGui::Separator();
        const std::string name = host->library().stem().string();
        if (host->isBuilding()) ImGui::TextColored({1.0f, 0.8f, 0.3f, 1.0f}, "%s: building...", name.c_str());
        else if (!host->isLoaded()) ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s: not loaded", name.c_str());
        else if (!host->lastBuildSucceeded()) ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s: build failed", name.c_str());
        else ImGui::TextDisabled("%s (reload %u)", name.c_str(), host->reloadCount());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Game code: %s\nCtrl+B build and reload, Ctrl+R reload", host->library().string().c_str());
    });
}

void EditorLayer::registerFrameworkCommands() {
    Command reset;
    reset.id = "window.resetLayout";
    reset.menuPath = "Window/Reset Layout";
    reset.separatorBefore = true;
    reset.order = 100;
    reset.execute = [this](EditorContext&) { m_resetLayout = true; };
    m_registry.command(std::move(reset));

    Command palette;
    palette.id = "window.commandPalette";
    palette.menuPath = "Window/Command Palette...";
    palette.shortcut = *KeyChord::parse("Ctrl+Shift+P");
    palette.order = 101;
    palette.showInPalette = false;
    palette.execute = [this](EditorContext&) { m_openPalette = true; };
    m_registry.command(std::move(palette));
}

// ------------------------------------------------------------------ update --
void EditorLayer::onUpdate(f32 dt) {
    if (m_gameModule) m_gameModule->update(); // finished builds, changed DLL → reload
    m_context.validateSelection();
    m_app->onUpdate(m_context, dt);

    const auto& panels = m_registry.panels();
    for (size_t i = 0; i < panels.size(); ++i)
        if (panels[i]->open || panels[i]->updateWhenClosed) panels[i]->onUpdate(m_context, dt);

    dispatchShortcuts();

    if (m_context.playState() == PlayState::Play) m_scene.update(dt);
    m_context.validateSelection();
}

void EditorLayer::dispatchShortcuts() {
    if (ImGui::GetIO().WantTextInput || m_context.inputCaptured()) return;
    const bool ctrl = Input::isKeyDown(Key::LeftControl) || Input::isKeyDown(Key::RightControl);
    const bool shift = Input::isKeyDown(Key::LeftShift) || Input::isKeyDown(Key::RightShift);
    const bool alt = Input::isKeyDown(Key::LeftAlt) || Input::isKeyDown(Key::RightAlt);
    for (const Command& c : m_registry.commands()) {
        if (!c.shortcut.valid() || c.shortcut.ctrl != ctrl || c.shortcut.shift != shift || c.shortcut.alt != alt)
            continue;
        if (!Input::isKeyPressed(c.shortcut.key) || !c.enabled(m_context)) continue;
        c.execute(m_context);
        break; // one shortcut per frame; also: execute may register commands
    }
}

void EditorLayer::onRender(rhi::CommandList& cmd) {
    const auto& panels = m_registry.panels();
    for (size_t i = 0; i < panels.size(); ++i)
        if (panels[i]->open) panels[i]->onRender(m_context, cmd);
}

// ---------------------------------------------------------------------- UI --
void EditorLayer::onImGui() {
    ImGuizmo::BeginFrame();
    drawMainMenu();

    ImGuiID dockId = ImGui::GetID("NovaDockSpace");
    if (m_resetLayout || !ImGui::DockBuilderGetNode(dockId)) {
        buildDefaultLayout(dockId);
        m_resetLayout = false;
    }
    ImGui::DockSpaceOverViewport(dockId, ImGui::GetMainViewport());

    drawPanels();
    for (size_t i = 0; i < m_registry.uiHooks().size(); ++i) m_registry.uiHooks()[i].draw(m_context);
    drawCommandPalette();

    // Automatic undo for the selection: whatever changed it this frame (inspector, gizmo, custom
    // panels, commands) becomes one step once the user lets go of the widget/gizmo.
    Tool* tool = m_context.activeTool();
    const bool interacting = ImGui::IsAnyItemActive() || ImGuizmo::IsUsing() || m_context.inputCaptured() ||
                             (tool && tool->isInteracting());
    m_context.updateEditTracking(interacting);
}

void EditorLayer::drawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) return;

    MenuNode root;
    for (const Command& c : m_registry.commands()) {
        auto parts = splitMenuPath(c.menuPath);
        if (parts.empty()) continue;
        MenuNode* node = &root;
        for (size_t i = 0; i + 1 < parts.size(); ++i) node = &child(*node, parts[i], c.order);
        MenuNode leaf;
        leaf.name = parts.back();
        leaf.command = &c;
        leaf.order = c.order;
        node->children.push_back(std::move(leaf));
    }
    child(root, "Window", 0); // always present: lists the panels
    sortTree(root, true);
    // Top-level menus keep the place where they first appeared (hot reloads re-register commands).
    for (const MenuNode& m : root.children)
        if (std::find(m_menuOrder.begin(), m_menuOrder.end(), m.name) == m_menuOrder.end()) m_menuOrder.push_back(m.name);
    auto slot = [this](const std::string& name) { return std::find(m_menuOrder.begin(), m_menuOrder.end(), name) - m_menuOrder.begin(); };
    std::stable_sort(root.children.begin(), root.children.end(),
                     [&](const MenuNode& a, const MenuNode& b) { return slot(a.name) < slot(b.name); });
    // Conventional placement: game menus (Tools, ...) before Window and Help.
    std::stable_partition(root.children.begin(), root.children.end(),
                          [](const MenuNode& m) { return m.name != "Window" && m.name != "Help"; });

    std::function<void(const MenuNode&)> drawItems = [&](const MenuNode& node) {
        for (const MenuNode& item : node.children) {
            if (item.command) {
                const Command& c = *item.command;
                if (c.separatorBefore) ImGui::Separator();
                const std::string shortcut = c.shortcut.toString();
                const bool checked = c.isChecked && c.isChecked(m_context);
                if (ImGui::MenuItem(item.name.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), checked,
                                    c.enabled(m_context)))
                    c.execute(m_context);
            } else if (ImGui::BeginMenu(item.name.c_str())) {
                drawItems(item);
                ImGui::EndMenu();
            }
        }
    };

    for (const MenuNode& menu : root.children) {
        if (menu.command) continue; // top-level commands are not supported in the bar
        if (!ImGui::BeginMenu(menu.name.c_str())) continue;
        if (menu.name == "Window")
            for (auto& p : m_registry.panels()) ImGui::MenuItem(p->title().c_str(), nullptr, &p->open);
        drawItems(menu);
        ImGui::EndMenu();
    }

    // Each widget in an ID scope of its own: its items may share labels with the menus ("Play").
    for (size_t i = 0; i < m_registry.menuBarWidgets().size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 0x4D42); // "MB": apart from the menus' own IDs
        m_registry.menuBarWidgets()[i].draw(m_context);
        ImGui::PopID();
    }
    ImGui::EndMainMenuBar();
}

void EditorLayer::drawPanels() {
    const auto& panels = m_registry.panels();
    for (size_t i = 0; i < panels.size(); ++i) {
        Panel& p = *panels[i];
        if (!p.open) continue;
        dockNewPanel(p);
        if (p.noPadding) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        const bool visible = ImGui::Begin(p.title().c_str(), &p.open, p.windowFlags);
        if (p.noPadding) ImGui::PopStyleVar();
        if (visible) p.onImGui(m_context);
        ImGui::End();
    }
}

// A panel the saved layout does not know (added since the .ini was written) joins the dock node
// of another panel with the same DockSlot instead of floating.
void EditorLayer::dockNewPanel(Panel& panel) {
    if (!m_dockChecked.insert(&panel).second) return; // once per panel
    if (panel.dockSlot == DockSlot::Floating || ImGui::FindWindowSettingsByID(ImHashStr(panel.title().c_str())))
        return;
    for (auto& other : m_registry.panels()) {
        if (other.get() == &panel || other->dockSlot != panel.dockSlot) continue;
        if (ImGuiWindow* w = ImGui::FindWindowByName(other->title().c_str()); w && w->DockId) {
            ImGui::SetNextWindowDockID(w->DockId, ImGuiCond_FirstUseEver);
            return;
        }
    }
}

void EditorLayer::buildDefaultLayout(unsigned int dockId) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = dockId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, nullptr, &center);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, nullptr, &right);

    for (auto& p : m_registry.panels()) {
        ImGuiID target = 0;
        switch (p->dockSlot) {
            case DockSlot::Left: target = left; break;
            case DockSlot::Right: target = right; break;
            case DockSlot::RightBottom: target = rightBottom; break;
            case DockSlot::Bottom: target = bottom; break;
            case DockSlot::Center: target = center; break;
            case DockSlot::Floating: break;
        }
        if (target) ImGui::DockBuilderDockWindow(p->title().c_str(), target);
        p->open = true;
    }
    ImGui::DockBuilderFinish(dockId);
}

void EditorLayer::drawCommandPalette() {
    if (m_openPalette) {
        ImGui::OpenPopup("##commandPalette");
        m_paletteFilter[0] = '\0';
        m_paletteSelection = 0;
        m_openPalette = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 60.0f}, ImGuiCond_Always,
                            {0.5f, 0.0f});
    ImGui::SetNextWindowSize({560.0f, 0.0f});
    if (!ImGui::BeginPopup("##commandPalette")) return;

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", "Type a command...", m_paletteFilter, sizeof(m_paletteFilter)))
        m_paletteSelection = 0;

    std::vector<const Command*> matches;
    for (const Command& c : m_registry.commands()) {
        if (!c.showInPalette || !c.enabled(m_context)) continue;
        const std::string& label = c.menuPath.empty() ? c.id : c.menuPath;
        if (containsCaseInsensitive(label, m_paletteFilter)) matches.push_back(&c);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_paletteSelection++;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_paletteSelection--;
    m_paletteSelection = std::clamp(m_paletteSelection, 0, std::max(0, static_cast<int>(matches.size()) - 1));

    const Command* run = nullptr;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !matches.empty()) run = matches[m_paletteSelection];

    ImGui::BeginChild("##results", {0.0f, std::min(320.0f, 26.0f * static_cast<float>(matches.size()) + 8.0f)});
    for (int i = 0; i < static_cast<int>(matches.size()); ++i) {
        const Command& c = *matches[i];
        const std::string& label = c.menuPath.empty() ? c.id : c.menuPath;
        ImGui::PushID(i);
        if (ImGui::Selectable(label.c_str(), i == m_paletteSelection)) run = &c;
        if (i == m_paletteSelection) ImGui::SetScrollHereY();
        if (c.shortcut.valid()) {
            const std::string s = c.shortcut.toString();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(s.c_str()).x);
            ImGui::TextDisabled("%s", s.c_str());
        }
        ImGui::PopID();
    }
    if (matches.empty()) ImGui::TextDisabled("No matching command");
    ImGui::EndChild();

    if (run) {
        ImGui::CloseCurrentPopup();
        run->execute(m_context);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace detail
} // namespace nova::editor
