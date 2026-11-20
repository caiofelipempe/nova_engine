// Nova's own editor: the built-in tools plus an example plugin, built on Nova::Editor exactly as a
// game-specific editor would be.
#include "DemoScene.h"
#include "ExamplePlugin.h"

#include <nova/editor/Editor.h>

struct NovaEditorApp : nova::editor::EditorApp {
    void onSetup(nova::editor::Registry& registry) override { registry.plugin<ExamplePlugin>(); }
    void onNewScene(nova::editor::EditorContext& ctx) override { buildDemoScene(ctx); }
};

int main() {
    reflectGameTypes(); // game types must be reflected before scenes are loaded or edited
    return nova::editor::run<NovaEditorApp>({.layoutFile = "nova_editor_layout.ini", .assetsDir = NOVA_EDITOR_ASSETS_DIR});
}
