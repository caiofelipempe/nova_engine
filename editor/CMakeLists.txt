# ------------------------------------------------------------ editor library --
# Nova::Editor — the extensible editor framework games build their own editors on.
find_package(imguizmo CONFIG REQUIRED)

file(GLOB_RECURSE NOVA_EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nova/editor/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nova/editor/*.h)

add_library(NovaEditorKit STATIC ${NOVA_EDITOR_SOURCES})
add_library(Nova::Editor ALIAS NovaEditorKit)
target_include_directories(NovaEditorKit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(NovaEditorKit PUBLIC Nova::Engine PRIVATE imguizmo::imguizmo)
set_target_properties(NovaEditorKit PROPERTIES FOLDER "Engine")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${NOVA_EDITOR_SOURCES})

# --------------------------------------------------------------- editor app --
# Nova's own editor: an example of a game-specific editor built on Nova::Editor.
if(PROJECT_IS_TOP_LEVEL)
    nova_add_editor(NovaEditor
        app/main.cpp
        app/DemoScene.cpp
        app/DemoScene.h
        app/ExamplePlugin.cpp
        app/ExamplePlugin.h)
    set_target_properties(NovaEditor PROPERTIES FOLDER "Apps")
endif()
