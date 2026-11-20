# Offline GLSL -> SPIR-V compilation with glslc (ships with the Vulkan SDK).
#
#   nova_add_shader(<target> <source> [OUTPUT <name.spv>] [DEFINES A=1 B ...])
#
# Outputs go to ${NOVA_SHADER_OUTPUT_DIR}; apps copy that folder next to the exe.

set(NOVA_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders CACHE INTERNAL "")

find_program(NOVA_GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)

function(nova_add_shader TARGET SOURCE)
    cmake_parse_arguments(ARG "" "OUTPUT" "DEFINES;DEPENDS" ${ARGN})

    get_filename_component(_name ${SOURCE} NAME)
    if(ARG_OUTPUT)
        set(_out ${NOVA_SHADER_OUTPUT_DIR}/${ARG_OUTPUT})
    else()
        set(_out ${NOVA_SHADER_OUTPUT_DIR}/${_name}.spv)
    endif()

    set(_defs "")
    foreach(_d ${ARG_DEFINES})
        list(APPEND _defs "-D${_d}")
    endforeach()

    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${NOVA_SHADER_OUTPUT_DIR}
        COMMAND ${NOVA_GLSLC} --target-env=vulkan1.3 -O -g ${_defs} ${SOURCE} -o ${_out}
        DEPENDS ${SOURCE} ${ARG_DEPENDS}
        COMMENT "glslc ${_name} -> ${_out}"
        VERBATIM)

    target_sources(${TARGET} PRIVATE ${_out})
    set_source_files_properties(${_out} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
endfunction()

# Creates an executable that uses the engine — everything an app needs in one call:
#
#   nova_add_app(MyGame src/main.cpp src/Player.cpp)
function(nova_add_app NAME)
    add_executable(${NAME} ${ARGN})
    target_link_libraries(${NAME} PRIVATE Nova::Engine)
    nova_copy_shaders(${NAME})
endfunction()

# Creates a game-specific editor executable (requires NOVA_BUILD_EDITOR):
#
#   nova_add_editor(MyGameEditor editor/main.cpp editor/WavePanel.cpp)
function(nova_add_editor NAME)
    nova_add_app(${NAME} ${ARGN})
    target_link_libraries(${NAME} PRIVATE Nova::Editor)
endfunction()

# Creates the game's code module: a DLL the editor loads and hot reloads (needs NOVA_SHARED).
# The sources must contain one NOVA_GAME_MODULE(MyGameModule) (see nova/editor/GameModule.h).
#
#   nova_add_game_module(MyGame game/MyGame.cpp game/Player.cpp)
#   # editor: run<...>({.gameModule = {.library = "$<TARGET_FILE:MyGame>" (via a definition), ...}})
function(nova_add_game_module NAME)
    if(NOT NOVA_SHARED)
        message(FATAL_ERROR "nova_add_game_module(${NAME}) requires NOVA_SHARED=ON")
    endif()
    add_library(${NAME} SHARED ${ARGN})
    target_link_libraries(${NAME} PRIVATE Nova::Editor)
endfunction()

# Copies the compiled shader folder next to an application's executable.
function(nova_copy_shaders APP)
    add_custom_command(TARGET ${APP} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${NOVA_SHADER_OUTPUT_DIR} $<TARGET_FILE_DIR:${APP}>/shaders
        VERBATIM)
    add_dependencies(${APP} NovaShaders)
    set_target_properties(${APP} PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${APP}>)
endfunction()
