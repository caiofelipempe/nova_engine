// Nova's own editor: the built-in tools plus the sample game's code module (editor/game), loaded
// from a DLL and hot reloaded when it is rebuilt — exactly how a game-specific editor is set up.
#include <nova/editor/Editor.h>

#include <string>

int main() {
    const std::string build = std::string("\"") + NOVA_EDITOR_CMAKE + "\" --build \"" + NOVA_EDITOR_BUILD_DIR +
                              "\" --config " + NOVA_EDITOR_CONFIG + " --target NovaEditorGame";
    return nova::editor::run({
        .layoutFile = "nova_editor_layout.ini",
        .assetsDir = NOVA_EDITOR_ASSETS_DIR,
        .gameModule = {.library = NOVA_EDITOR_GAME_MODULE, .buildCommand = build},
    });
}
