#pragma once

/// @file
/// @brief Game code in a DLL that the editor loads and hot reloads while running.
///
/// @code
/// // game/MyGame.cpp — built with nova_add_game_module(MyGame game/MyGame.cpp ...)
/// struct MyGame : nova::editor::GameModule {
///     void onLoad(nova::editor::Registry& r) override {
///         nova::reflect::type<Health>("Health").field("hp", &Health::hp).component();
///         r.plugin<MyGamePlugin>();
///     }
///     void onNewScene(nova::editor::EditorContext& ctx) override { buildTestLevel(ctx); }
/// };
/// NOVA_GAME_MODULE(MyGame)
/// @endcode
/// Rebuild the DLL (or use *Game/Build and Reload*) and the editor swaps the code in place: the
/// scene is kept (reflected components and scripts), everything the module registered —
/// reflected types, plugins, panels, tools, commands, component editors, importers — is removed
/// and registered again by the new code.

#include "nova/editor/Registry.h"
#include "nova/editor/Signal.h"
#include "nova/platform/DynamicLibrary.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nova::editor {

/// @brief Version of the module ABI; a module built against another version is rejected.
inline constexpr int kGameModuleApiVersion = 1;

/// @brief Entry point of game code loaded by the editor (see NOVA_GAME_MODULE).
class GameModule {
public:
    virtual ~GameModule() = default;
    /// @brief Registers the game's reflected types and editor extensions (runs on every (re)load).
    virtual void onLoad(Registry& registry) = 0;
    /// @brief Called before the code is unloaded; registrations are removed automatically.
    virtual void onUnload(EditorContext& /*ctx*/) {}
    /// @brief Fills the editor's initial scene (called once at startup, after EditorApp::onNewScene).
    virtual void onNewScene(EditorContext& /*ctx*/) {}
};

/// @brief Where the game module is and how to rebuild it (EditorDesc::gameModule).
struct GameModuleDesc {
    std::filesystem::path library; ///< The game DLL (`$<TARGET_FILE:MyGame>`); empty = no game module.
    std::string buildCommand;      ///< Shell command that rebuilds it (e.g. `cmake --build build --target MyGame`).
    bool watch = true;             ///< Reload automatically when the DLL file changes.
};

/// @brief Loads, unloads and hot reloads a GameModule DLL inside the editor.
///
/// The DLL is copied to a temporary file before loading, so the original can be rebuilt while
/// the editor runs. A reload:
/// 1. stops Play mode, saves the scene in memory and clears the undo history;
/// 2. destroys every entity and ECS component storage (their code may live in the DLL);
/// 3. calls GameModule::onUnload, removes the module's registrations and unloads the DLL;
/// 4. loads the new DLL, calls GameModule::onLoad and restores the scene and the selection.
///
/// Only reflected state survives (as with Play/Stop). If the new DLL fails to load, the previous
/// code is loaded again.
class GameModuleHost {
public:
    /// @param ctx Editor context (scene, assets, selection, undo).
    /// @param registry Registry the module registers into.
    GameModuleHost(EditorContext& ctx, Registry& registry);
    /// @brief Unloads the module (waiting for a running build first).
    ~GameModuleHost();
    GameModuleHost(const GameModuleHost&) = delete;
    GameModuleHost& operator=(const GameModuleHost&) = delete;

    /// @brief Loads a game DLL (unloading the current one). @return False on error (see `error`).
    bool load(const std::filesystem::path& library, std::string* error = nullptr);
    /// @brief Reloads the DLL from its path, keeping the scene. @return False if the new code failed
    ///        to load (the previous code is restored).
    bool reload(std::string* error = nullptr);
    /// @brief Unloads the module; the scene keeps its entities minus the module's components.
    void unload();

    /// @brief True while a module is loaded.
    bool isLoaded() const { return m_module != nullptr; }
    /// @brief The loaded module, or `nullptr`.
    GameModule* module() const { return m_module; }
    /// @brief Path of the game DLL.
    const std::filesystem::path& library() const { return m_library; }
    /// @brief Id tagging the current module's registrations (changes on every reload).
    ModuleId moduleId() const { return m_id; }
    /// @brief Successful reloads so far.
    u32 reloadCount() const { return m_reloads; }
    /// @brief An exported symbol of the loaded DLL (game-defined extras), or `nullptr`.
    void* symbol(const char* name) const { return m_lib.symbol(name); }

    /// @name Build and watch
    /// @{
    std::string buildCommand; ///< Command run by build().
    bool watch = true;        ///< update() reloads when the DLL changes on disk.
    /// @brief Runs @ref buildCommand in the background; update() reloads when it succeeds.
    void build();
    /// @brief True while a build runs.
    bool isBuilding() const { return m_building; }
    /// @brief Whether the last finished build succeeded (true before any build).
    bool lastBuildSucceeded() const { return m_lastBuildOk; }
    /// @brief Call once per frame: finishes builds (logging their output) and watches the DLL.
    void update();
    /// @}

    /// @brief Emitted after a successful (re)load of new code.
    Signal<> reloaded;

private:
    bool loadInternal(const std::filesystem::path& file, std::string* error);
    std::string unloadInternal();
    void restoreScene(const std::string& json, const std::vector<u64>& selection);
    std::filesystem::path shadowCopy(std::string* error);

    EditorContext& m_ctx;
    Registry& m_registry;
    platform::DynamicLibrary m_lib;
    GameModule* m_module = nullptr;
    ModuleId m_id = 0;
    std::filesystem::path m_library;
    std::filesystem::path m_loadedCopy;
    std::filesystem::file_time_type m_loadedTime{};
    std::filesystem::file_time_type m_pendingTime{};
    double m_lastPoll = 0.0;
    u32 m_reloads = 0;
    u32 m_copies = 0;

    std::thread m_buildThread;
    std::atomic<bool> m_building{false};
    std::atomic<bool> m_buildFinished{false};
    int m_buildExitCode = 0;
    bool m_lastBuildOk = true;
    std::mutex m_buildMutex;
    std::vector<std::string> m_buildLines;
};

} // namespace nova::editor

/// @brief Exports a symbol from a game module.
#if defined(_WIN32)
    #define NOVA_MODULE_EXPORT __declspec(dllexport)
#else
    #define NOVA_MODULE_EXPORT __attribute__((visibility("default")))
#endif

/// @brief Declares `Type` (a GameModule subclass) as the entry point of this game DLL.
#define NOVA_GAME_MODULE(Type)                                                                     \
    extern "C" NOVA_MODULE_EXPORT nova::editor::GameModule* novaCreateGameModule() { return new Type(); } \
    extern "C" NOVA_MODULE_EXPORT int novaGameModuleApiVersion() { return nova::editor::kGameModuleApiVersion; }
