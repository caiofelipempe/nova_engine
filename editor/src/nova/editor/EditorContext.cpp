#include "nova/editor/EditorContext.h"

#include "nova/core/Application.h"

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
        m_snapshot.clear();
        for (auto [e, t] : m_scene->registry().view<TransformComponent>().each()) m_snapshot.emplace_back(e, t);
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
    m_scene->stopSimulation();
    auto& reg = m_scene->registry();
    for (auto& [e, t] : m_snapshot)
        if (reg.valid(e) && reg.all_of<TransformComponent>(e)) reg.replace<TransformComponent>(e, t);
    m_snapshot.clear();
    m_playState = PlayState::Edit;
    validateSelection();
    playStateChanged.emit(m_playState);
}

} // namespace nova::editor
