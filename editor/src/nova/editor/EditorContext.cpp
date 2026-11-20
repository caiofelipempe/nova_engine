#include "nova/editor/EditorContext.h"

#include "nova/core/Application.h"
#include "nova/core/Log.h"

namespace nova::editor {

Application& EditorContext::app() { return Application::get(); }
Renderer& EditorContext::renderer() { return Application::get().renderer(); }

void EditorContext::select(Entity entity) {
    if (!entity.valid()) entity = {};
    if (entity == m_selected) return;
    m_selected = entity;
    selectionChanged.emit(m_selected);
}

void EditorContext::validateSelection() {
    if (m_selected.scene() && !m_selected.valid()) select({});
}

void EditorContext::play() {
    if (m_playState == PlayState::Play) return;
    if (m_playState == PlayState::Edit) {
        // Default options = mesh ids: exact within this session, no name lookups needed.
        m_snapshot = serializeScene(*m_scene);
        m_scene->startSimulation();
    }
    m_playState = PlayState::Play;
    playStateChanged.emit(m_playState);
}

void EditorContext::pause() {
    if (m_playState != PlayState::Play) return;
    m_playState = PlayState::Pause;
    playStateChanged.emit(m_playState);
}

void EditorContext::stop() {
    if (m_playState == PlayState::Edit) return;
    const u64 selectedUuid = m_selected.valid() ? m_selected.uuid() : 0;
    m_scene->stopSimulation();
    std::string error;
    if (!deserializeScene(*m_scene, m_snapshot, {}, &error)) NOVA_CORE_ERROR("Editor: failed to restore scene: {}", error);
    m_snapshot.clear();
    m_playState = PlayState::Edit;

    m_selected = {}; // handles changed: re-select by UUID
    select(m_scene->findEntityByUuid(selectedUuid));
    if (!m_selected.valid() && selectedUuid) selectionChanged.emit(m_selected);
    sceneReloaded.emit();
    playStateChanged.emit(m_playState);
}

void EditorContext::newScene() {
    stop();
    m_scene->clear();
    m_scene->setName("Untitled");
    m_scenePath.clear();
    clearSelection();
    sceneReloaded.emit();
}

bool EditorContext::openScene(const std::filesystem::path& path, std::string* error) {
    stop();
    clearSelection();
    const bool ok = loadScene(*m_scene, path, fileOptions, error);
    if (ok) m_scenePath = path;
    sceneReloaded.emit();
    return ok;
}

bool EditorContext::saveScene(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (!nova::saveScene(*m_scene, path, fileOptions, error)) return false;
    m_scenePath = path;
    m_scene->setName(sceneNameFromPath(path));
    return true;
}

} // namespace nova::editor
