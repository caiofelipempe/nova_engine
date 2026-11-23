find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_path(CGLTF_INCLUDE_DIRS "cgltf.h" REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED) # header-only (vcpkg "miniaudio")

# fpm: header-only fixed-point numbers (deterministic arithmetic, sqrt and trigonometry) for the
# simulation state. Not in vcpkg; only its headers are used (SOURCE_SUBDIR skips its test build).
include(FetchContent)
FetchContent_Declare(fpm
    GIT_REPOSITORY https://github.com/MikeLankamp/fpm.git
    GIT_TAG        8b488e3c1f9afb981cad31292b7b0be05926661e # v1.1.0
    SOURCE_SUBDIR  headers-only)
FetchContent_MakeAvailable(fpm)
add_library(nova_fpm INTERFACE)
target_include_directories(nova_fpm SYSTEM INTERFACE ${fpm_SOURCE_DIR}/include)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/nova)

# Backend-agnostic engine code.
file(GLOB_RECURSE NOVA_CORE_SOURCES CONFIGURE_DEPENDS
    ${SRC}/core/*.cpp      ${SRC}/core/*.h
    ${SRC}/platform/*.h    ${SRC}/platform/Window.cpp ${SRC}/platform/Input.cpp ${SRC}/platform/FileSystem.cpp ${SRC}/platform/DynamicLibrary.cpp
    ${SRC}/rhi/*.h         ${SRC}/rhi/Device.cpp
    ${SRC}/renderer/*.cpp  ${SRC}/renderer/*.h
    ${SRC}/scene/*.cpp     ${SRC}/scene/*.h
    ${SRC}/physics/*.cpp   ${SRC}/physics/*.h
    ${SRC}/physics/backend/*.cpp ${SRC}/physics/backend/*.h
    ${SRC}/reflect/*.cpp   ${SRC}/reflect/*.h
    ${SRC}/assets/*.cpp    ${SRC}/assets/*.h
    ${SRC}/audio/*.cpp     ${SRC}/audio/*.h
    ${SRC}/math/*.cpp      ${SRC}/math/*.h
    ${SRC}/ui/*.h          ${SRC}/ui/ImGuiTheme.cpp ${SRC}/ui/ImGuiSync.cpp
    ${SRC}/Nova.h)

if(NOVA_SHARED)
    add_library(NovaEngine SHARED ${NOVA_CORE_SOURCES})
    # Functions/classes are exported automatically; data uses NOVA_API (see Base.h).
    set_target_properties(NovaEngine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(NovaEngine PUBLIC NOVA_SHARED PRIVATE NOVA_BUILDING_ENGINE)
    # Dear ImGui is a static library, so every module that uses Nova gets its own copy of it:
    # this file, compiled into each consumer (exe, editor, game DLL), keeps their ImGui context
    # in sync with the engine's.
    target_sources(NovaEngine INTERFACE ${SRC}/ui/ImGuiModuleSync.cpp)
else()
    add_library(NovaEngine STATIC ${NOVA_CORE_SOURCES})
endif()
add_library(Nova::Engine ALIAS NovaEngine)
target_include_directories(NovaEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src PRIVATE ${CGLTF_INCLUDE_DIRS} ${STB_INCLUDE_DIRS})

target_link_libraries(NovaEngine
    PUBLIC  glm::glm EnTT::EnTT spdlog::spdlog imgui::imgui nova_fpm
    PRIVATE nlohmann_json::nlohmann_json)

target_compile_definitions(NovaEngine PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
    $<$<CONFIG:Debug>:NOVA_DEBUG>
    $<$<PLATFORM_ID:Windows>:NOMINMAX WIN32_LEAN_AND_MEAN>)

if(MSVC)
    target_compile_options(NovaEngine PUBLIC /W4 /permissive- /utf-8 /MP /wd4201 /wd4100)
else()
    target_compile_options(NovaEngine PUBLIC -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- backends --
if(NOVA_RHI_VULKAN)
    file(GLOB NOVA_VK_SOURCES CONFIGURE_DEPENDS ${SRC}/rhi/vulkan/*.cpp ${SRC}/rhi/vulkan/*.h)
    target_sources(NovaEngine PRIVATE ${NOVA_VK_SOURCES})
    target_link_libraries(NovaEngine PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
    target_compile_definitions(NovaEngine PUBLIC NOVA_RHI_VULKAN=1)
endif()

# Runtime shader compiler (optional). The Vulkan SDK ships shaderc_combined (static, glslang + SPIRV-Tools).
if(NOVA_SHADER_COMPILER)
    find_package(Vulkan COMPONENTS shaderc_combined)
    # Vulkan headers may come from vcpkg while shaderc only exists in the SDK: find its headers
    # separately and give them to the one file that uses them (so they never shadow vcpkg's).
    find_path(NOVA_SHADERC_INCLUDE_DIR shaderc/shaderc.hpp
        HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include" ${Vulkan_INCLUDE_DIRS})
    if(TARGET Vulkan::shaderc_combined AND NOVA_SHADERC_INCLUDE_DIR)
        target_link_libraries(NovaEngine PRIVATE Vulkan::shaderc_combined)
        set_source_files_properties(${SRC}/renderer/ShaderLibrary.cpp PROPERTIES INCLUDE_DIRECTORIES ${NOVA_SHADERC_INCLUDE_DIR})
        target_compile_definitions(NovaEngine PUBLIC NOVA_SHADER_COMPILER=1)
    else()
        message(WARNING "shaderc_combined not found in the Vulkan SDK: runtime shader compilation disabled")
        target_compile_definitions(NovaEngine PUBLIC NOVA_SHADER_COMPILER=0)
    endif()
else()
    target_compile_definitions(NovaEngine PUBLIC NOVA_SHADER_COMPILER=0)
endif()

if(NOVA_PLATFORM_GLFW)
    file(GLOB NOVA_GLFW_SOURCES CONFIGURE_DEPENDS ${SRC}/platform/glfw/*.cpp ${SRC}/platform/glfw/*.h)
    target_sources(NovaEngine PRIVATE ${NOVA_GLFW_SOURCES})
    # Vulkan headers are needed for the WSI bridge (glfwCreateWindowSurface).
    target_link_libraries(NovaEngine PRIVATE glfw Vulkan::Headers)
    target_compile_definitions(NovaEngine PUBLIC NOVA_PLATFORM_GLFW=1)
endif()

if(NOVA_PHYSICS_JOLT)
    find_package(Jolt CONFIG REQUIRED)
    # The simulation must give the same bits everywhere: Jolt comes from the overlay port in
    # cmake/vcpkg-ports (CROSS_PLATFORM_DETERMINISTIC=ON), which exports the define to users.
    get_target_property(NOVA_JOLT_DEFINES Jolt::Jolt INTERFACE_COMPILE_DEFINITIONS)
    if(NOT "JPH_CROSS_PLATFORM_DETERMINISTIC" IN_LIST NOVA_JOLT_DEFINES)
        message(FATAL_ERROR "Jolt was built without CROSS_PLATFORM_DETERMINISTIC. Use the overlay port "
                            "(vcpkg-configuration.json → cmake/vcpkg-ports/joltphysics) and reconfigure.")
    endif()
    file(GLOB NOVA_JOLT_SOURCES CONFIGURE_DEPENDS ${SRC}/physics/backend/jolt/*.cpp ${SRC}/physics/backend/jolt/*.h)
    target_sources(NovaEngine PRIVATE ${NOVA_JOLT_SOURCES})
    target_link_libraries(NovaEngine PRIVATE Jolt::Jolt)
    target_compile_definitions(NovaEngine PUBLIC NOVA_PHYSICS_JOLT=1)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${NOVA_CORE_SOURCES} ${NOVA_VK_SOURCES} ${NOVA_GLFW_SOURCES} ${NOVA_JOLT_SOURCES})

# ----------------------------------------------------------------- shaders --
set(SH ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
file(GLOB NOVA_SHADER_INCLUDES CONFIGURE_DEPENDS ${SH}/nova/*.glsl)
set(NOVA_SHADER_INCLUDES ${NOVA_SHADER_INCLUDES} CACHE INTERNAL "Public GLSL headers (nova/*.glsl)")

add_custom_target(NovaShaders SOURCES ${NOVA_SHADER_INCLUDES})
set_target_properties(NovaShaders PROPERTIES FOLDER "Engine")
# Public headers are deployed to <exe>/shaders/include/nova for runtime-compiled game shaders.
foreach(_inc ${NOVA_SHADER_INCLUDES})
    get_filename_component(_inc_name ${_inc} NAME)
    set(_inc_out ${NOVA_SHADER_OUTPUT_DIR}/include/nova/${_inc_name})
    add_custom_command(OUTPUT ${_inc_out}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_inc} ${_inc_out}
        DEPENDS ${_inc} VERBATIM)
    target_sources(NovaShaders PRIVATE ${_inc_out})
    set_source_files_properties(${_inc_out} PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
endforeach()
nova_add_shader(NovaShaders ${SH}/fullscreen.vert DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/sky.frag        DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/tonemap.frag    DEPENDS ${NOVA_SHADER_INCLUDES})
if(NOVA_WRAP_RENDER_MODES)
    nova_add_shader(NovaShaders ${SH}/forward.vert DEFINES NOVA_WRAP_RENDER_MODES=1 DEPENDS ${NOVA_SHADER_INCLUDES})
else()
    nova_add_shader(NovaShaders ${SH}/forward.vert DEPENDS ${NOVA_SHADER_INCLUDES})
endif()
nova_add_shader(NovaShaders ${SH}/forward.frag    DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/debug_line.vert DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/debug_line.frag DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/cull.comp       DEPENDS ${NOVA_SHADER_INCLUDES}) # GPU frustum culling
nova_add_shader(NovaShaders ${SH}/shadow.frag     DEPENDS ${NOVA_SHADER_INCLUDES}) # cut-out shadow casters
nova_add_shader(NovaShaders ${SH}/skin.comp       DEPENDS ${NOVA_SHADER_INCLUDES}) # skinned meshes for ray tracing
# Hybrid variant: same shader, shadows traced with VK_KHR_ray_query against the TLAS.
nova_add_shader(NovaShaders ${SH}/forward.frag OUTPUT forward_rq.frag.spv DEFINES NOVA_RAY_QUERY=1 DEPENDS ${NOVA_SHADER_INCLUDES})
add_dependencies(NovaEngine NovaShaders)
target_include_directories(NovaEngine PRIVATE ${MINIAUDIO_INCLUDE_DIR})
if(NOVA_WRAP_RENDER_MODES)
    target_compile_definitions(NovaEngine PUBLIC NOVA_WRAP_RENDER_MODES=1)
else()
    target_compile_definitions(NovaEngine PUBLIC NOVA_WRAP_RENDER_MODES=0)
endif()
set_target_properties(NovaEngine PROPERTIES FOLDER "Engine")
