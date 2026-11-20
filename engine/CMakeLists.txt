find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_path(CGLTF_INCLUDE_DIRS "cgltf.h" REQUIRED)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/nova)

# Backend-agnostic engine code.
file(GLOB_RECURSE NOVA_CORE_SOURCES CONFIGURE_DEPENDS
    ${SRC}/core/*.cpp      ${SRC}/core/*.h
    ${SRC}/platform/*.h    ${SRC}/platform/Window.cpp ${SRC}/platform/Input.cpp ${SRC}/platform/FileSystem.cpp
    ${SRC}/rhi/*.h         ${SRC}/rhi/Device.cpp
    ${SRC}/renderer/*.cpp  ${SRC}/renderer/*.h
    ${SRC}/scene/*.cpp     ${SRC}/scene/*.h
    ${SRC}/assets/*.cpp    ${SRC}/assets/*.h
    ${SRC}/ui/*.cpp        ${SRC}/ui/*.h
    ${SRC}/Nova.h)

add_library(NovaEngine STATIC ${NOVA_CORE_SOURCES})
add_library(Nova::Engine ALIAS NovaEngine)
target_include_directories(NovaEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src PRIVATE ${CGLTF_INCLUDE_DIRS})

target_link_libraries(NovaEngine
    PUBLIC  glm::glm EnTT::EnTT spdlog::spdlog imgui::imgui)

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

if(NOVA_PLATFORM_GLFW)
    file(GLOB NOVA_GLFW_SOURCES CONFIGURE_DEPENDS ${SRC}/platform/glfw/*.cpp ${SRC}/platform/glfw/*.h)
    target_sources(NovaEngine PRIVATE ${NOVA_GLFW_SOURCES})
    # Vulkan headers are needed for the WSI bridge (glfwCreateWindowSurface).
    target_link_libraries(NovaEngine PRIVATE glfw Vulkan::Headers)
    target_compile_definitions(NovaEngine PUBLIC NOVA_PLATFORM_GLFW=1)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${NOVA_CORE_SOURCES} ${NOVA_VK_SOURCES} ${NOVA_GLFW_SOURCES})

# ----------------------------------------------------------------- shaders --
set(SH ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
file(GLOB NOVA_SHADER_INCLUDES CONFIGURE_DEPENDS ${SH}/*.glsl)

add_custom_target(NovaShaders SOURCES ${NOVA_SHADER_INCLUDES})
set_target_properties(NovaShaders PROPERTIES FOLDER "Engine")
nova_add_shader(NovaShaders ${SH}/fullscreen.vert DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/sky.frag        DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/tonemap.frag    DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/forward.vert    DEPENDS ${NOVA_SHADER_INCLUDES})
nova_add_shader(NovaShaders ${SH}/forward.frag    DEPENDS ${NOVA_SHADER_INCLUDES})
# Hybrid variant: same shader, shadows traced with VK_KHR_ray_query against the TLAS.
nova_add_shader(NovaShaders ${SH}/forward.frag OUTPUT forward_rq.frag.spv DEFINES NOVA_RAY_QUERY=1 DEPENDS ${NOVA_SHADER_INCLUDES})
add_dependencies(NovaEngine NovaShaders)
set_target_properties(NovaEngine PROPERTIES FOLDER "Engine")
