#pragma once

/// @file
/// @brief Shared editor state and services (scene, selection, play mode, camera).

#include "nova/editor/EditorCamera.h"
#include "nova/editor/Signal.h"
#include "nova/scene/Scene.h"
#include "nova/scene/SceneSerializer.h"

#include <filesystem>
#include <string>

namespace nova {
class Application;
class Renderer;
} // namespace nova

namespace nova::editor {

class Registry;

/// @brief Edit / Play / Pause.
enum class PlayState {
    Edit,  ///< Editing: the scene does not update.
    Play,  ///< Simulating: scripts, systems and physics run every frame.
    Pause, ///< Simulation frozen; Play resumes, Stop restores the edit state.
};

/// @brief Shared state and services every panel, command and plugin works with.
///
/// Passed to Panel callbacks and Command::execute. Changes are broadcast through the public
/// Signal members so extensions stay decoupled from each other.
class EditorContext {
public:
    /// @param scene The scene being edited (must outlive the context).
    explicit EditorContext(Scene& scene) : m_scene(&scene) {}
    EditorContext(const EditorContext&) = delete;
    EditorContext& operator=(const EditorContext&) = delete;

    /// @brief The scene being edited.
    Scene& scene() { return *m_scene; }
    /// @brief The running application. @pre Called from inside the editor (not in headless tests).
    Application& app();
    /// @brief The engine renderer. @pre Called from inside the editor (not in headless tests).
    Renderer& renderer();
    /// @brief The extension registry (panels, commands...). @pre Set by the editor.
    Registry& registry() { return *m_registry; }
    /// @brief Connects the registry (done by the editor).
    void setRegistry(Registry* registry) { m_registry = registry; }

    /// @name Selection
    /// @{
    /// @brief The selected entity (invalid when nothing is selected).
    Entity selected() const { return m_selected; }
    /// @brief Selects an entity (an invalid one clears the selection). Emits selectionChanged on change.
    void select(Entity entity);
    /// @brief Clears the selection.
    void clearSelection() { select({}); }
    /// @brief Emitted when the selection changes (argument: new selection, possibly invalid).
    Signal<Entity> selectionChanged;
    /// @}

    /// @name Play mode
    /// @{
    /// @brief Current mode.
    PlayState playState() const { return m_playState; }
    /// @brief True in Play or Pause (the simulation exists).
    bool isPlaying() const { return m_playState != PlayState::Edit; }
    /// @brief Edit → Play: snapshots the whole scene and starts physics. Pause → Play: resumes.
    void play();
    /// @brief Play → Pause (no-op otherwise).
    void pause();
    /// @brief Play/Pause → Edit: stops physics and restores the scene exactly as it was at play()
    ///        (entities created during play disappear, destroyed ones come back). The selection
    ///        is kept by UUID.
    void stop();
    /// @brief Emitted after the play state changes.
    Signal<PlayState> playStateChanged;
    /// @}

    /// @name Scene files
    /// @{
    /// @brief Clears the scene (stopping play first) and forgets the file path.
    void newScene();
    /// @brief Loads a scene file (stopping play first). @return False on error (`error` filled).
    bool openScene(const std::filesystem::path& path, std::string* error = nullptr);
    /// @brief Saves to `path` and remembers it. Creates missing directories. @return False on error.
    bool saveScene(const std::filesystem::path& path, std::string* error = nullptr);
    /// @brief File the scene was last opened from / saved to (empty for a new scene).
    const std::filesystem::path& scenePath() const { return m_scenePath; }
    /// @brief Emitted after the scene's entities were replaced (new, open, or stop restoring the snapshot).
    Signal<> sceneReloaded;
    /// @brief How meshes are referenced in files (the editor sets mesh names via the renderer).
    SerializeOptions fileOptions;
    /// @}

    /// @name Viewport camera and input
    /// @{
    /// @brief The editor's fly camera.
    EditorCamera& camera() { return m_camera; }
    /// @brief True while a tool captures the mouse/keyboard (e.g. flying the camera); shortcuts are suppressed.
    bool inputCaptured() const { return m_inputCaptured; }
    /// @brief Set by tools that capture input.
    void setInputCaptured(bool captured) { m_inputCaptured = captured; }
    /// @}

    /// @brief Drops the selection if the selected entity was destroyed. Called by the editor each frame.
    void validateSelection();

private:
    Scene* m_scene;
    Registry* m_registry = nullptr;
    Entity m_selected;
    PlayState m_playState = PlayState::Edit;
    std::string m_snapshot; // scene JSON captured by play()
    std::filesystem::path m_scenePath;
    EditorCamera m_camera;
    bool m_inputCaptured = false;
};

} // namespace nova::editor
