cmake_minimum_required(VERSION 3.25)

project(NovaEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# All executables land in build/bin/<Config> so DLLs and the exe live side by side
# (shaders are copied next to each app by nova_add_app regardless).
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ---------------------------------------------------------------------------
# Backend selection. Each backend lives in its own folder and is compiled only
# when enabled, so adding e.g. a D3D12 RHI or an SDL3 window is additive.
# ---------------------------------------------------------------------------
option(NOVA_RHI_VULKAN       "Build the Vulkan RHI backend"      ON)
option(NOVA_PLATFORM_GLFW    "Build the GLFW window backend"     ON)
option(NOVA_PHYSICS_JOLT     "Build the Jolt Physics backend"    ON)
# Editor/sandbox are on when this is the top-level project, off when a game embeds the engine
# via add_subdirectory().
option(NOVA_BUILD_EDITOR     "Build the editor library (Nova::Editor) and, top-level, the NovaEditor app" ON)
option(NOVA_BUILD_SANDBOX    "Build the runtime sandbox sample"  ${PROJECT_IS_TOP_LEVEL})
option(NOVA_BUILD_TESTS      "Build the headless engine tests"   ${PROJECT_IS_TOP_LEVEL})
# Shared engine/editor libraries let game code live in a DLL that the editor reloads while running.
option(NOVA_SHARED           "Build Nova::Engine and Nova::Editor as shared libraries (hot reload)" ON)
# Runtime GLSL -> SPIR-V compilation (shaderc from the Vulkan SDK). Turn off for final builds that
# ship only precompiled shaders (nova_add_game_shaders): no compiler inside the game.
option(NOVA_SHADER_COMPILER  "Compile game shaders at runtime (shaderc), with hot reload" ON)

if(NOT NOVA_RHI_VULKAN)
    message(FATAL_ERROR "At least one RHI backend is required (only Vulkan is implemented).")
endif()
if(NOT NOVA_PLATFORM_GLFW)
    message(FATAL_ERROR "At least one platform backend is required (only GLFW is implemented).")
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/NovaShaders.cmake)

add_subdirectory(engine)

if(NOVA_BUILD_EDITOR)
    add_subdirectory(editor)
endif()
if(NOVA_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()
if(NOVA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# API reference (Doxygen). Optional: only when doxygen is installed.
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND AND PROJECT_IS_TOP_LEVEL)
    add_custom_target(NovaDocs
        COMMAND ${DOXYGEN_EXECUTABLE} docs/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API reference in build/docs/html"
        VERBATIM)
    set_target_properties(NovaDocs PROPERTIES FOLDER "Docs")
endif()
