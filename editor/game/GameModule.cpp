// The sample game's code module: everything game-specific the editor uses lives in this DLL and is
// hot reloaded when it is rebuilt (Game/Build and Reload, Ctrl+B).
#include "DemoScene.h"
#include "ExamplePlugin.h"

#include <nova/editor/Editor.h>

struct NovaSampleGame : nova::editor::GameModule {
    void onLoad(nova::editor::Registry& registry) override {
        reflectGameTypes();                 // components and scripts (inspector, save/load)
        registry.plugin<ExamplePlugin>();   // panels, tools, commands
    }
    void onNewScene(nova::editor::EditorContext& ctx) override { buildDemoScene(ctx); }
};

NOVA_GAME_MODULE(NovaSampleGame)
