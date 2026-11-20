# ------------------------------------------------------------ editor library --
# Nova::Editor — the extensible editor framework games build their own editors on.
find_package(imguizmo CONFIG REQUIRED)

file(GLOB_RECURSE NOVA_EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nova/editor/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nova/editor/*.h)

if(NOVA_SHARED)
    add_library(NovaEditorKit SHARED ${NOVA_EDITOR_SOURCES})
    set_target_properties(NovaEditorKit PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(NovaEditorKit PRIVATE NOVA_BUILDING_EDITOR)
else()
    add_library(NovaEditorKit STATIC ${NOVA_EDITOR_SOURCES})
endif()
add_library(Nova::Editor ALIAS NovaEditorKit)
target_include_directories(NovaEditorKit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(NovaEditorKit PUBLIC Nova::Engine PRIVATE imguizmo::imguizmo)
set_target_properties(NovaEditorKit PROPERTIES FOLDER "Engine")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${NOVA_EDITOR_SOURCES})

# --------------------------------------------------------------- editor app --
# Nova's own editor: an example of a game-specific editor built on Nova::Editor. The sample game's
# code (components, scripts, panels, tools, demo scene) is a separate DLL the editor hot reloads.
if(PROJECT_IS_TOP_LEVEL AND NOVA_SHARED)
    nova_add_game_module(NovaEditorGame
        game/GameModule.cpp
        game/DemoScene.cpp
        game/DemoScene.h
        game/ExamplePlugin.cpp
        game/ExamplePlugin.h)
    set_target_properties(NovaEditorGame PROPERTIES FOLDER "Apps")

    nova_add_editor(NovaEditor app/main.cpp)
    add_dependencies(NovaEditor NovaEditorGame)
    set_target_properties(NovaEditor PROPERTIES FOLDER "Apps")
    target_compile_definitions(NovaEditor PRIVATE
        # The sample project's assets live in the repository (models, textures, prefabs, scenes).
        NOVA_EDITOR_ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets"
        NOVA_EDITOR_GAME_MODULE="$<TARGET_FILE:NovaEditorGame>"
        NOVA_EDITOR_CMAKE="${CMAKE_COMMAND}"
        NOVA_EDITOR_BUILD_DIR="${CMAKE_BINARY_DIR}"
        NOVA_EDITOR_CONFIG="$<CONFIG>")
endif()