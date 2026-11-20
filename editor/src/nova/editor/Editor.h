#pragma once

/// @file
/// @brief Nova editor framework — the only header a game's editor needs.
///
/// @code
/// #include <nova/editor/Editor.h>
///
/// struct MyGameEditor : nova::editor::EditorApp {
///     void onSetup(nova::editor::Registry& r) override { /* panels, commands, inspectors */ }
/// };
/// int main() { return nova::editor::run<MyGameEditor>(); }
/// @endcode

#include <nova/Nova.h>

#include "nova/editor/Command.h"
#include "nova/editor/EditorApp.h"
#include "nova/editor/EditorCamera.h"
#include "nova/editor/EditorContext.h"
#include "nova/editor/Panel.h"
#include "nova/editor/Picking.h"
#include "nova/editor/Registry.h"
#include "nova/editor/Signal.h"
#include "nova/editor/Tool.h"
#include "nova/editor/UndoStack.h"
#include "nova/editor/Widgets.h"

#include <imgui.h>

/// @brief Extensible editor framework (see EditorApp, Registry, Panel, Command, EditorContext).
namespace nova::editor {}
