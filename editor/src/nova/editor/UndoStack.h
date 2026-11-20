#pragma once

/// @file
/// @brief Undo/redo history.

#include "nova/editor/Signal.h"

#include <functional>
#include <string>
#include <vector>

namespace nova::editor {

/// @brief One undoable step.
struct UndoAction {
    std::string name;            ///< Shown in Edit/Undo and the History panel ("Move Crate").
    std::function<void()> undo;  ///< Reverts the change.
    std::function<void()> redo;  ///< Re-applies it.
};

/// @brief Linear undo/redo history.
///
/// Usually filled through EditorContext (edit(), recordCreation(), destroyEntities(), automatic
/// tracking of the selection), which builds UUID-based snapshot actions that stay valid when
/// entities are deleted and recreated. Push custom actions for state outside the scene.
class UndoStack {
public:
    /// @brief Adds an already-applied action; discards anything that was undone.
    void push(UndoAction action);
    /// @brief True if there is something to undo.
    bool canUndo() const { return m_position > 0; }
    /// @brief True if there is something to redo.
    bool canRedo() const { return m_position < m_actions.size(); }
    /// @brief Reverts the last applied action. @return False if there was none.
    bool undo();
    /// @brief Re-applies the next undone action. @return False if there was none.
    bool redo();
    /// @brief Undoes/redoes until `position` actions are applied (History panel).
    void jumpTo(size_t position);
    /// @brief Forgets the whole history.
    void clear();

    /// @brief All actions, oldest first; the first position() are applied.
    const std::vector<UndoAction>& actions() const { return m_actions; }
    /// @brief Number of applied actions.
    size_t position() const { return m_position; }
    /// @brief Name of the action Undo would revert ("" if none).
    const std::string& undoName() const;
    /// @brief Name of the action Redo would apply ("" if none).
    const std::string& redoName() const;
    /// @brief True while an undo/redo callback runs (edits made then must not be recorded).
    bool isApplying() const { return m_applying; }
    /// @brief Maximum number of actions kept (oldest are dropped). Default 256.
    void setCapacity(size_t capacity);

    /// @brief Emitted after push, undo, redo, jump or clear.
    Signal<> changed;

private:
    std::vector<UndoAction> m_actions;
    size_t m_position = 0;
    size_t m_capacity = 256;
    bool m_applying = false;
};

} // namespace nova::editor
