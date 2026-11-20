#include "nova/editor/UndoStack.h"

namespace nova::editor {

namespace {
const std::string kEmpty;
}

void UndoStack::push(UndoAction action) {
    m_actions.resize(m_position); // drop the redo branch
    m_actions.push_back(std::move(action));
    if (m_actions.size() > m_capacity) m_actions.erase(m_actions.begin());
    m_position = m_actions.size();
    changed.emit();
}

bool UndoStack::undo() {
    if (!canUndo()) return false;
    m_applying = true;
    m_actions[m_position - 1].undo();
    m_applying = false;
    --m_position;
    changed.emit();
    return true;
}

bool UndoStack::redo() {
    if (!canRedo()) return false;
    m_applying = true;
    m_actions[m_position].redo();
    m_applying = false;
    ++m_position;
    changed.emit();
    return true;
}

void UndoStack::jumpTo(size_t position) {
    if (position > m_actions.size()) position = m_actions.size();
    while (m_position > position) undo();
    while (m_position < position) redo();
}

void UndoStack::clear() {
    m_actions.clear();
    m_position = 0;
    changed.emit();
}

const std::string& UndoStack::undoName() const { return canUndo() ? m_actions[m_position - 1].name : kEmpty; }
const std::string& UndoStack::redoName() const { return canRedo() ? m_actions[m_position].name : kEmpty; }

void UndoStack::setCapacity(size_t capacity) {
    m_capacity = capacity < 1 ? 1 : capacity;
    while (m_actions.size() > m_capacity) {
        m_actions.erase(m_actions.begin());
        if (m_position > 0) --m_position;
    }
}

} // namespace nova::editor
