#pragma once

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

constexpr u32 kInvalidIndex = ~0u;
constexpr u32 kFramesInFlight = 2;
constexpr u32 kPushConstantSize = 128;

// Bindless descriptor set (set = 0) layout, mirrored in shaders/bindless.glsl.
constexpr u32 kBindingSampledTextures = 0;
constexpr u32 kBindingStorageImages = 1;

const char* resultString(VkResult result);

VkFormat toVk(Format format);
Format fromVk(VkFormat format);
VkCompareOp toVk(CompareOp op);
VkCullModeFlags toVk(CullMode mode);
VkPrimitiveTopology toVk(PrimitiveTopology topology);
VkAttachmentLoadOp toVk(LoadOp op);
VkAttachmentStoreOp toVk(StoreOp op);

struct StateInfo {
    VkImageLayout layout;
    VkPipelineStageFlags2 stages;
    VkAccessFlags2 access;
};
StateInfo stateInfo(ResourceState state, bool depth);

// Extension entry points that are not exported by the loader library.
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
    // VK_KHR_ray_tracing_pipeline (loaded for the upcoming RT pipeline path)
    PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
};

} // namespace nova::rhi::vk
