#pragma once

/// @file
/// @brief Shared editor state and services (scene, selection, undo, play mode, camera, tools).

#include "nova/assets/AssetDatabase.h"
#include "nova/editor/EditorCamera.h"
#include "nova/editor/Signal.h"
#include "nova/editor/UndoStack.h"
#include "nova/scene/Scene.h"
#include "nova/scene/SceneSerializer.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nova {
class Application;
class Renderer;
} // namespace nova

namespace nova::editor {

class Registry;
class Tool;
class GameModuleHost;
class EditorContext;

/// @brief ImGui drag-and-drop payload type for assets (payload: one AssetGuid).
inline constexpr const char* kAssetDragPayload = "NOVA_ASSET";

/// @brief Edit / Play / Pause.
enum class PlayState {
    Edit,  ///< Editing: the scene does not update.
    Play,  ///< Simulating: scripts, systems and physics run every frame.
    Pause, ///< Simulation frozen; Play resumes, Stop restores the edit state.
};

/// @brief An in-progress, undoable modification of some entities (see EditorContext::beginEdit).
///
/// Captures the entities' state at creation; commit() captures it again and records one undo
/// step if anything changed. Destroying an uncommitted transaction cancels it (nothing recorded,
/// changes are kept). Move-only.
class EditTransaction {
public:
    EditTransaction() = default;
    EditTransaction(EditTransaction&& o) noexcept
        : m_ctx(std::exchange(o.m_ctx, nullptr)), m_name(std::move(o.m_name)), m_before(std::move(o.m_before)) {}
    EditTransaction& operator=(EditTransaction&& o) noexcept {
        m_ctx = std::exchange(o.m_ctx, nullptr);
        m_name = std::move(o.m_name);
        m_before = std::move(o.m_before);
        return *this;
    }
    /// @brief Records the change (if any). @return True if an undo step was pushed.
    bool commit();
    /// @brief True until committed.
    bool active() const { return m_ctx != nullptr; }

private:
    friend class EditorContext;
    EditorContext* m_ctx = nullptr;
    std::string m_name;
    std::vector<std::pair<u64, std::string>> m_before; // uuid -> entity JSON
};

/// @brief An in-progress, undoable creation of entities (see EditorContext::beginCreation).
///
/// Every entity that appears between creation and commit() becomes one undo step
/// (undo deletes them, redo brings them back). Move-only.
class CreationTransaction {
public:
    CreationTransaction() = default;
    CreationTransaction(CreationTransaction&& o) noexcept
        : m_ctx(std::exchange(o.m_ctx, nullptr)), m_name(std::move(o.m_name)), m_before(std::move(o.m_before)) {}
    CreationTransaction& operator=(CreationTransaction&& o) noexcept {
        m_ctx = std::exchange(o.m_ctx, nullptr);
        m_name = std::move(o.m_name);
        m_before = std::move(o.m_before);
        return *this;
    }
    /// @brief Records the created entities. @return The entities created since the transaction began.
    std::vector<Entity> commit();
    /// @brief True until committed.
    bool active() const { return m_ctx != nullptr; }

private:
    friend class EditorContext;
    EditorContext* m_ctx = nullptr;
    std::string m_name;
    std::unordered_set<u64> m_before;
};

/// @brief Shared state and services every panel, command, tool and plugin works with.
///
/// Passed to Panel callbacks, Command::execute and Tool callbacks. Changes are broadcast
/// through the public Signal members so extensions stay decoupled from each other.
class EditorContext {
public:
    /// @param scene The scene being edited (must outlive the context).
    explicit EditorContext(Scene& scene);
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
    /// Several entities can be selected; the last selected one is the *primary* (inspector, gizmo pivot).
    /// @{
    /// @brief All selected entities; the last one is the primary.
    const std::vector<Entity>& selection() const { return m_selection; }
    /// @brief The primary selected entity (invalid when nothing is selected).
    Entity selected() const { return m_selection.empty() ? Entity{} : m_selection.back(); }
    /// @brief True if `entity` is selected.
    bool isSelected(Entity entity) const;
    /// @brief Selects only `entity` (an invalid one clears the selection).
    void select(Entity entity);
    /// @brief Replaces the selection (invalid/duplicate entries are dropped; last = primary).
    void setSelection(std::vector<Entity> entities);
    /// @brief Adds `entity` and makes it primary.
    void addToSelection(Entity entity);
    /// @brief Removes `entity` from the selection.
    void removeFromSelection(Entity entity);
    /// @brief Adds or removes `entity`.
    void toggleSelection(Entity entity);
    /// @brief Clears the selection.
    void clearSelection() { setSelection({}); }
    /// @brief Emitted when the selection changes (argument: new primary, possibly invalid).
    Signal<Entity> selectionChanged;
    /// @}

    /// @name Undo / redo
    /// Every change made through these helpers — and every change to the *selected* entities made
    /// by any panel, gizmo or command (automatic tracking) — becomes an undo step.
    /// @{
    /// @brief The history.
    UndoStack& undoStack() { return m_undo; }
    /// @brief Undoes the last step (no-op while playing).
    void undo();
    /// @brief Redoes the next step (no-op while playing).
    void redo();
    /// @brief Starts an undoable edit of `entities` (commit when the interaction ends).
    EditTransaction beginEdit(std::string name, const std::vector<Entity>& entities);
    /// @brief Runs `change` as one undoable edit of `entities`.
    void edit(std::string name, const std::vector<Entity>& entities, const std::function<void()>& change);
    /// @brief Starts recording entity creation (commit when done).
    CreationTransaction beginCreation(std::string name);
    /// @brief Runs `create` and records every entity it creates as one undo step. @return The created entities.
    std::vector<Entity> recordCreation(std::string name, const std::function<void()>& create);
    /// @brief Destroys entities as one undo step (they are restored, components and scripts included, on undo).
    void destroyEntities(std::string name, const std::vector<Entity>& entities);
    /// @brief Automatic tracking of the selection, called by the editor once per frame after the UI.
    ///
    /// When nothing is being interacted with, compares the selected entities with their state
    /// after the previous call and records the difference as "Edit <name>". While `interacting`
    /// (a widget is held, a gizmo dragged...) the comparison waits, so a whole drag is one step.
    void updateEditTracking(bool interacting);
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
    /// @brief Clears the scene (stopping play first), the history and the file path.
    void newScene();
    /// @brief Loads a scene file (stopping play first; clears the history). @return False on error.
    bool openScene(const std::filesystem::path& path, std::string* error = nullptr);
    /// @brief Saves to `path` and remembers it. Creates missing directories. @return False on error.
    bool saveScene(const std::filesystem::path& path, std::string* error = nullptr);
    /// @brief File the scene was last opened from / saved to (empty for a new scene).
    const std::filesystem::path& scenePath() const { return m_scenePath; }
    /// @brief Emitted after the scene's entities were replaced (new, open, or stop restoring the snapshot).
    Signal<> sceneReloaded;
    /// @}

    /// @name Assets
    /// @{
    /// @brief The project's assets (root = EditorDesc::assetsDir). Scenes reference assets by GUID.
    AssetDatabase& assets() { return m_assets; }
    /// @brief Instantiates an asset (model, prefab...) at `position` as one undo step and selects the result.
    /// @param adjust Optional tweak of the new entities, recorded in the same undo step (e.g. placement).
    /// @return The created entities (empty if the asset cannot be placed).
    std::vector<Entity> instantiateAsset(AssetGuid guid, const GlobalPosition& position,
                                         const std::function<void(std::vector<Entity>&)>& adjust = {});
    /// @brief Point in front of the editor camera where new things are placed.
    GlobalPosition spawnPoint() const;
    /// @}

    /// @brief The game code module host (`nullptr` when the editor has no EditorDesc::gameModule).
    GameModuleHost* gameModule() const { return m_gameModule; }
    /// @brief Set by the editor.
    void setGameModule(GameModuleHost* host) { m_gameModule = host; }

    /// @name Viewport camera, tools and input
    /// @{
    /// @brief The editor's fly camera.
    EditorCamera& camera() { return m_camera; }
    /// @brief The active viewport tool (`nullptr` before the editor starts).
    Tool* activeTool() const { return m_activeTool; }
    /// @brief Switches the active tool (calls onDeactivate / onActivate).
    void setActiveTool(Tool* tool);
    /// @brief True while a tool captures the mouse/keyboard (e.g. flying the camera); shortcuts are suppressed.
    bool inputCaptured() const { return m_inputCaptured; }
    /// @brief Set by tools that capture input.
    void setInputCaptured(bool captured) { m_inputCaptured = captured; }
    /// @}

    /// @brief Drops destroyed entities from the selection. Called by the editor each frame.
    void validateSelection();

private:
    friend class EditTransaction;
    friend class CreationTransaction;

    using Snapshot = std::vector<std::pair<u64, std::string>>;
    Snapshot snapshot(const std::vector<Entity>& entities) const;
    void restore(const Snapshot& snapshot);
    void destroyByUuid(const std::vector<u64>& uuids);
    std::unordered_set<u64> allUuids() const;
    void pushRecorded(UndoAction action);

    Scene* m_scene;
    Registry* m_registry = nullptr;
    std::vector<Entity> m_selection;
    UndoStack m_undo;
    Snapshot m_trackBaseline;
    bool m_trackValid = false;
    PlayState m_playState = PlayState::Edit;
    std::string m_snapshot; // scene JSON captured by play()
    std::filesystem::path m_scenePath;
    AssetDatabase m_assets;
    GameModuleHost* m_gameModule = nullptr;
    EditorCamera m_camera;
    Tool* m_activeTool = nullptr;
    bool m_inputCaptured = false;
};

} // namespace nova::editor
