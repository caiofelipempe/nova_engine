#pragma once

/// @file
/// @brief Vulkan backend utilities: includes, error checking, enum conversions, extension entry points.

#include "nova/core/Log.h"
#include "nova/rhi/RHITypes.h"

#include <vulkan/vulkan.h>

#if defined(_MSC_VER)
    #pragma warning(push, 0)
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
    #pragma warning(pop)
#endif

#include <cstdlib>

/// @brief Evaluates a Vulkan call and aborts with a log message if it does not return `VK_SUCCESS`.
#define VK_CHECK(expr)                                                                             \
    do {                                                                                           \
        VkResult _vkr = (expr);                                                                    \
        if (_vkr != VK_SUCCESS) {                                                                  \
            NOVA_CORE_CRITICAL("Vulkan call failed ({}) at {}:{}: {}",                             \
                               ::nova::rhi::vk::resultString(_vkr), __FILE__, __LINE__, #expr);   \
            ::nova::Log::core().flush();                                                           \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)

namespace nova::rhi::vk {

constexpr u32 kInvalidIndex = ~0u;       ///< "No bindless slot".
constexpr u32 kFramesInFlight = 2;       ///< Frames the CPU records ahead of the GPU.
constexpr u32 kPushConstantSize = 128;   ///< Push constant bytes shared by every pipeline.

/// @name Bindless descriptor set (set = 0) bindings, mirrored in shaders/common.glsl
/// @{
constexpr u32 kBindingSampledTextures = 0; ///< `sampler2D g_textures[]`.
constexpr u32 kBindingStorageImages = 1;   ///< Storage images.
/// @}

/// @brief Name of a VkResult (e.g. "VK_ERROR_DEVICE_LOST").
const char* resultString(VkResult result);

/// @name RHI → Vulkan enum conversions
/// @{
VkFormat toVk(Format format);
Format fromVk(VkFormat format); ///< Inverse of toVk(Format) for swapchain/color formats; Undefined otherwise.
VkCompareOp toVk(CompareOp op);
VkCullModeFlags toVk(CullMode mode);
VkPrimitiveTopology toVk(PrimitiveTopology topology);
VkAttachmentLoadOp toVk(LoadOp op);
VkAttachmentStoreOp toVk(StoreOp op);
/// @}

/// @brief Image layout + pipeline stages + access mask that implement a ResourceState.
struct StateInfo {
    VkImageLayout layout;         ///< Image layout.
    VkPipelineStageFlags2 stages; ///< Stages that access the image in this state.
    VkAccessFlags2 access;        ///< Accesses performed in this state.
};
/// @brief Maps a ResourceState to its synchronization2 description.
StateInfo stateInfo(ResourceState state, bool depth);
/// @brief Makes stateInfo's shader stages include ray tracing shaders (set by the device when it enables them).
void setRayTracingShaderStages(bool enabled);

/// @brief Extension entry points that are not exported by the loader library (loaded with vkGet*ProcAddr).
struct ExtFunctions {
    // VK_EXT_debug_utils
    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXT = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabelEXT = nullptr;
    // VK_KHR_acceleration_structure
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR vkCmdWriteAccelerationStructuresPropertiesKHR = nullptr; // compaction
    PFN_vkCmdCopyAccelerationStructureKHR vkCmdCopyAccelerationStructureKHR = nullptr;
    // VK_KHR_ray_tracing_pipeline (loaded for the upcoming RT pipeline path)
    PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
};

} // namespace nova::rhi::vk
