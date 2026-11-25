#include "nova/rhi/vulkan/VulkanCommon.h"

namespace nova::rhi::vk {

const char* resultString(VkResult r) {
    switch (r) {
#define NOVA_VK_CASE(x) case x: return #x;
        NOVA_VK_CASE(VK_SUCCESS)
        NOVA_VK_CASE(VK_NOT_READY)
        NOVA_VK_CASE(VK_TIMEOUT)
        NOVA_VK_CASE(VK_INCOMPLETE)
        NOVA_VK_CASE(VK_SUBOPTIMAL_KHR)
        NOVA_VK_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        NOVA_VK_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        NOVA_VK_CASE(VK_ERROR_INITIALIZATION_FAILED)
        NOVA_VK_CASE(VK_ERROR_DEVICE_LOST)
        NOVA_VK_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        NOVA_VK_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        NOVA_VK_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        NOVA_VK_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        NOVA_VK_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        NOVA_VK_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        NOVA_VK_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        NOVA_VK_CASE(VK_ERROR_SURFACE_LOST_KHR)
        NOVA_VK_CASE(VK_ERROR_OUT_OF_DATE_KHR)
        NOVA_VK_CASE(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
#undef NOVA_VK_CASE
        default: return "VK_<unknown>";
    }
}

VkFormat toVk(Format f) {
    switch (f) {
        case Format::Undefined: return VK_FORMAT_UNDEFINED;
        case Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
        case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
        case Format::RG16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
        case Format::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case Format::R32_UINT: return VK_FORMAT_R32_UINT;
        case Format::RG32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        case Format::RGB32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
        case Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

Format fromVk(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return Format::RGBA8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return Format::BGRA8_SRGB;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return Format::RGBA16_FLOAT;
        default: return Format::Undefined;
    }
}

VkCompareOp toVk(CompareOp op) {
    switch (op) {
        case CompareOp::Never: return VK_COMPARE_OP_NEVER;
        case CompareOp::Less: return VK_COMPARE_OP_LESS;
        case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
        case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkCullModeFlags toVk(CullMode m) {
    switch (m) {
        case CullMode::None: return VK_CULL_MODE_NONE;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

VkPrimitiveTopology toVk(PrimitiveTopology t) {
    switch (t) {
        case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkAttachmentLoadOp toVk(LoadOp op) {
    switch (op) {
        case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

VkAttachmentStoreOp toVk(StoreOp op) {
    return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

namespace {
VkPipelineStageFlags2 g_extraShaderStages = 0; // ray tracing shaders, when the device enables them
} // namespace

void setRayTracingShaderStages(bool enabled) {
    g_extraShaderStages = enabled ? VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR : 0;
}

StateInfo stateInfo(ResourceState state, bool depth) {
    const VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | g_extraShaderStages;

    switch (state) {
        case ResourceState::Undefined:
            return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
        case ResourceState::RenderTarget:
            return {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
        case ResourceState::DepthWrite:
            return {VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case ResourceState::DepthRead:
            return {VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                        kShaderStages,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case ResourceState::ShaderRead:
            return {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kShaderStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case ResourceState::UnorderedAccess:
            return {VK_IMAGE_LAYOUT_GENERAL, kShaderStages,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        case ResourceState::CopySrc:
            return {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT};
        case ResourceState::CopyDst:
            return {VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT};
        case ResourceState::Present:
            return {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    }
    (void)depth;
    return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
}

} // namespace nova::rhi::vk
