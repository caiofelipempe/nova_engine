#pragma once

/// @file
/// @brief Entry point for game-specific editors.

#include "nova/core/Application.h"
#include "nova/editor/EditorContext.h"
#include "nova/editor/GameModule.h"
#include "nova/editor/Registry.h"

#include <filesystem>
#include <string>

namespace nova::editor {

/// @brief Startup configuration of an editor.
struct EditorDesc {
    ApplicationDesc app{.name = "Nova Editor", .maximized = true}; ///< Window/device settings.
    /// @brief ImGui layout file (docking, window sizes). Use a distinct name per game editor.
    std::string layoutFile = "editor_layout.ini";
    /// @brief The project's asset folder (created if missing), shown in the Assets panel.
    std::filesystem::path assetsDir = "assets";
    /// @brief Game code DLL loaded at startup and hot reloaded (see GameModule.h); empty = none.
    GameModuleDesc gameModule;
    bool builtinPanels = true;     ///< Viewport, Hierarchy, Inspector, Renderer, Console, History and Assets panels.
    bool builtinCommands = true;   ///< File/Edit/Entity/Play/Window menus, shortcuts and the command palette.
    bool builtinComponents = true; ///< Inspector support for the engine's components.
};

/// @brief Base class of a game's editor: register the game's tools and set up its scene.
///
/// @code
/// struct MyGameEditor : nova::editor::EditorApp {
///     void onSetup(nova::editor::Registry& r) override {
///         r.panel<WavesPanel>();
///         r.component<Health>("Health", [](Health& h) { ImGui::DragFloat("HP", &h.current); });
///         r.command("Tools/Spawn Wave", [](nova::editor::EditorContext& ctx) { spawnWave(ctx.scene()); }, "Ctrl+W");
///     }
///     void onNewScene(nova::editor::EditorContext& ctx) override { loadLevel(ctx.scene()); }
/// };
/// int main() { return nova::editor::run<MyGameEditor>({.layoutFile = "mygame_editor.ini"}); }
/// @endcode
/// Built-in panels, commands and component editors are registered *before* onSetup(), so the
/// game can also replace or remove them (Registry::removePanel, Registry::command with the same id...).
class EditorApp {
public:
    virtual ~EditorApp() = default;
    /// @brief Register panels, commands, component editors and plugins.
    virtual void onSetup(Registry& /*registry*/) {}
    /// @brief Populate the initial scene (called once, after onSetup()).
    virtual void onNewScene(EditorContext& /*ctx*/) {}
    /// @brief Per-frame hook, before panels update.
    virtual void onUpdate(EditorContext& /*ctx*/, f32 /*dt*/) {}
};

namespace detail {
/// @brief Pushes the editor layer for `editor` onto `app` (implementation of run()).
void pushEditor(Application& app, const EditorDesc& desc, Scope<EditorApp> editor);
} // namespace detail

/// @brief Creates the application and runs the editor until the window closes.
/// @tparam T The game's EditorApp subclass (or EditorApp itself for a plain editor).
/// @param desc Editor configuration.
/// @param args Arguments forwarded to `T`'s constructor.
/// @return Process exit code (0).
template <typename T = EditorApp, typename... Args>
int run(const EditorDesc& desc = {}, Args&&... args) {
    Application app(desc.app);
    detail::pushEditor(app, desc, makeScope<T>(std::forward<Args>(args)...));
    app.run();
    return 0;
}

} // namespace nova::editor
