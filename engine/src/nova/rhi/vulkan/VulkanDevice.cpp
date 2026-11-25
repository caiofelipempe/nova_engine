#include "nova/rhi/vulkan/VulkanDevice.h"

#include "nova/platform/Window.h"
#include "nova/rhi/vulkan/VulkanImGuiRenderer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace nova::rhi::vk {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* id = data->pMessageIdName ? data->pMessageIdName : "";
    // Color slots left empty on purpose (ColorAttachment without a texture): writes to them are dropped.
    if (std::strcmp(id, "Undefined-Value-ShaderOutputNotConsumed-DynamicRendering") == 0) return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        NOVA_CORE_ERROR("[vulkan] {} {}", id, data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        NOVA_CORE_WARN("[vulkan] {} {}", id, data->pMessage);
    else
        NOVA_CORE_TRACE("[vulkan] {} {}", id, data->pMessage);
    return VK_FALSE;
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice pd) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());
    return exts;
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

constexpr u64 alignUp(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

struct CoreFeatures {
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    void chain() {
        f2.pNext = &f12;
        f12.pNext = &f13;
        f13.pNext = nullptr;
    }
};

bool supportsRequiredFeatures(VkPhysicalDevice pd) {
    CoreFeatures f;
    f.chain();
    vkGetPhysicalDeviceFeatures2(pd, &f.f2);
    return f.f2.features.shaderInt64 && f.f12.bufferDeviceAddress && f.f12.descriptorIndexing &&
           f.f12.runtimeDescriptorArray && f.f12.descriptorBindingPartiallyBound &&
           f.f12.descriptorBindingSampledImageUpdateAfterBind && f.f12.descriptorBindingStorageImageUpdateAfterBind &&
           f.f12.shaderSampledImageArrayNonUniformIndexing && f.f12.scalarBlockLayout && f.f13.dynamicRendering &&
           f.f13.synchronization2;
}

VkBufferUsageFlags toVkUsage(BufferUsage u, bool rt) {
    VkBufferUsageFlags r = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (any(u & BufferUsage::Vertex)) r |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (any(u & BufferUsage::Index)) r |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (any(u & BufferUsage::Uniform)) r |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (any(u & BufferUsage::Storage)) r |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (any(u & BufferUsage::Indirect)) r |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (any(u & BufferUsage::TransferSrc)) r |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (any(u & BufferUsage::TransferDst)) r |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (rt) {
        if (any(u & BufferUsage::AccelStructInput))
            r |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        if (any(u & BufferUsage::AccelStructStorage)) r |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
        if (any(u & BufferUsage::ShaderBindingTable)) r |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
    }
    return r;
}

} // namespace

// ------------------------------------------------------------- transitions --
void recordTransition(VkCommandBuffer cmd, VulkanTexture& tex, ResourceState state) {
    if (tex.state == state && state != ResourceState::UnorderedAccess) return;
    const bool depth = (tex.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    StateInfo src = stateInfo(tex.state, depth);
    StateInfo dst = stateInfo(state, depth);
    // Coming from "undefined" there is nothing to wait on except whatever produced the image
    // (e.g. the swapchain acquire semaphore, which waits at ALL_COMMANDS).
    if (tex.state == ResourceState::Undefined || tex.state == ResourceState::Present)
        src.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    if (state == ResourceState::Present) dst.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src.stages;
    b.srcAccessMask = src.access;
    b.dstStageMask = dst.stages;
    b.dstAccessMask = dst.access;
    b.oldLayout = src.layout;
    b.newLayout = dst.layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex.image;
    b.subresourceRange = {tex.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    tex.state = state;
}

// ---------------------------------------------------------------- lifetime --
VulkanDevice::VulkanDevice(const DeviceDesc& desc, platform::Window& window)
    : m_vsync(desc.vsync), m_validation(desc.enableValidation), m_rayTracingRequested(desc.enableRayTracing),
      m_window(window) {
    createInstance(window);
    NOVA_VERIFY(window.createVulkanSurface(m_instance, &m_surface), "Failed to create the Vulkan surface");
    pickPhysicalDevice();
    createLogicalDevice();
    createAllocator();
    createSamplers();
    createBindlessLayout();
    createFrames();
    createSwapchain();
    createPipelineCache(desc.pipelineCacheFile);

    NOVA_CORE_INFO("RHI: {} on '{}'", m_caps.apiName, m_caps.deviceName);
    NOVA_CORE_INFO("RHI: ray query {} | RT pipeline {} | bindless textures {}",
                   m_caps.rayQuery ? "yes" : "no", m_caps.rayTracingPipeline ? "yes" : "no",
                   m_caps.maxBindlessTextures);
}

VulkanDevice::~VulkanDevice() {
    vkDeviceWaitIdle(m_device);
    flushReleases(true);

    for (Frame& f : m_frames) {
        vkDestroyCommandPool(m_device, f.pool, nullptr);
        vkDestroyFence(m_device, f.fence, nullptr);
        vkDestroySemaphore(m_device, f.imageAvailable, nullptr);
    }
    destroySwapchainResources();
    if (m_swapchain) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);

    // Anything still alive at this point leaked from higher layers; clean it up and report.
    u32 leaks = m_accelStructs.size() + m_pipelines.size() + m_textures.size();
    m_accelStructs.forEach([&](AccelStructHandle, VulkanAccelStruct& as) {
        m_ext.vkDestroyAccelerationStructureKHR(m_device, as.handle, nullptr);
    });
    m_pipelines.forEach([&](PipelineHandle, VulkanPipeline& p) { vkDestroyPipeline(m_device, p.pipeline, nullptr); });
    m_textures.forEach([&](TextureHandle, VulkanTexture& t) {
        vkDestroyImageView(m_device, t.view, nullptr);
        if (t.ownsImage) vmaDestroyImage(m_allocator, t.image, t.allocation);
    });
    m_buffers.forEach([&](BufferHandle, VulkanBuffer& b) { destroyBufferNow(b); });
    if (leaks) NOVA_CORE_WARN("RHI: {} GPU resources were not destroyed before device shutdown", leaks);

    savePipelineCache();
    if (m_blasBytesBuilt)
        NOVA_CORE_INFO("RHI: BLAS compaction kept {:.1f} of {:.1f} MB", m_blasBytesKept / 1048576.0, m_blasBytesBuilt / 1048576.0);
    vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
    vkDestroyCommandPool(m_device, m_uploadPool, nullptr);
    vkDestroyFence(m_device, m_uploadFence, nullptr);
    vkDestroySampler(m_device, m_defaultSampler, nullptr);
    vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
    vkDestroyDescriptorPool(m_device, m_bindlessPool, nullptr);
    vkDestroyDescriptorSetLayout(m_device, m_bindlessLayout, nullptr);
    vmaDestroyAllocator(m_allocator);
    vkDestroyDevice(m_device, nullptr);
    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_messenger) m_ext.vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    vkDestroyInstance(m_instance, nullptr);
}

void VulkanDevice::createInstance(platform::Window& window) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Nova Application";
    app.pEngineName = "Nova Engine";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = window.vulkanInstanceExtensions();

    u32 count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    if (hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        m_debugUtils = true; // object names + labels for RenderDoc / Nsight even without validation
    }

    std::vector<const char*> layers;
    if (m_validation) {
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> availableLayers(count);
        vkEnumerateInstanceLayerProperties(&count, availableLayers.data());
        bool found = std::any_of(availableLayers.begin(), availableLayers.end(), [](const VkLayerProperties& l) {
            return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (found) layers.push_back("VK_LAYER_KHRONOS_validation");
        else {
            NOVA_CORE_WARN("Validation requested but VK_LAYER_KHRONOS_validation is not installed");
            m_validation = false;
        }
    }

    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<u32>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<u32>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (m_validation && m_debugUtils) ci.pNext = &dbg;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &m_instance));

    if (m_debugUtils) {
#define NOVA_LOAD_INSTANCE(fn) m_ext.fn = reinterpret_cast<PFN_##fn>(vkGetInstanceProcAddr(m_instance, #fn))
        NOVA_LOAD_INSTANCE(vkCreateDebugUtilsMessengerEXT);
        NOVA_LOAD_INSTANCE(vkDestroyDebugUtilsMessengerEXT);
        NOVA_LOAD_INSTANCE(vkSetDebugUtilsObjectNameEXT);
        NOVA_LOAD_INSTANCE(vkCmdBeginDebugUtilsLabelEXT);
        NOVA_LOAD_INSTANCE(vkCmdEndDebugUtilsLabelEXT);
#undef NOVA_LOAD_INSTANCE
        if (m_validation) VK_CHECK(m_ext.vkCreateDebugUtilsMessengerEXT(m_instance, &dbg, nullptr, &m_messenger));
    }
    NOVA_CORE_INFO("Vulkan instance created (validation: {})", m_validation ? "on" : "off");
}

void VulkanDevice::pickPhysicalDevice() {
    u32 count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    int bestScore = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        if (!hasExtension(deviceExtensions(pd), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        if (!supportsRequiredFeatures(pd)) continue;

        u32 qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, families.data());
        int family = -1;
        for (u32 i = 0; i < qCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, m_surface, &present);
            constexpr VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((families[i].queueFlags & need) == need && present) { family = static_cast<int>(i); break; }
        }
        if (family < 0) continue;

        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 1000
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 100
                                                                                  : 10;
        if (hasExtension(deviceExtensions(pd), VK_KHR_RAY_QUERY_EXTENSION_NAME)) score += 50;
        if (score > bestScore) {
            bestScore = score;
            m_physicalDevice = pd;
            m_queueFamily = static_cast<u32>(family);
        }
    }
    NOVA_VERIFY(m_physicalDevice != VK_NULL_HANDLE,
                "No GPU supports Vulkan 1.3 + dynamic rendering + bindless (descriptor indexing, BDA)");
    vkGetPhysicalDeviceProperties(m_physicalDevice, &m_properties);
}

void VulkanDevice::createLogicalDevice() {
    auto exts = deviceExtensions(m_physicalDevice);
    const bool hasAS = hasExtension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                       hasExtension(exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const bool hasRQ = hasExtension(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool hasRTP = hasExtension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);

    // Query what is supported.
    CoreFeatures supported;
    supported.chain();
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asSupported{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqSupported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtpSupported{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    if (hasAS && hasRQ) {
        supported.f13.pNext = &asSupported;
        asSupported.pNext = &rqSupported;
        if (hasRTP) rqSupported.pNext = &rtpSupported;
    }
    const bool hasUnused = hasExtension(exts, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT unusedSupported{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT};
    if (hasUnused) {
        unusedSupported.pNext = supported.f13.pNext;
        supported.f13.pNext = &unusedSupported;
    }
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &supported.f2);
    const bool unusedAttachments = hasUnused && unusedSupported.dynamicRenderingUnusedAttachments;

    const bool rt = m_rayTracingRequested && hasAS && hasRQ && asSupported.accelerationStructure && rqSupported.rayQuery;
    const bool rtPipeline = rt && hasRTP && rtpSupported.rayTracingPipeline;

    // Enable exactly what we use.
    CoreFeatures enable;
    enable.chain();
    enable.f2.features.shaderInt64 = VK_TRUE;
    enable.f2.features.samplerAnisotropy = supported.f2.features.samplerAnisotropy;
    enable.f2.features.fillModeNonSolid = supported.f2.features.fillModeNonSolid;
    enable.f2.features.shaderClipDistance = supported.f2.features.shaderClipDistance;
    enable.f2.features.drawIndirectFirstInstance = supported.f2.features.drawIndirectFirstInstance;
    enable.f2.features.multiDrawIndirect = supported.f2.features.multiDrawIndirect;
    enable.f12.bufferDeviceAddress = VK_TRUE;
    enable.f12.descriptorIndexing = VK_TRUE;
    enable.f12.runtimeDescriptorArray = VK_TRUE;
    enable.f12.descriptorBindingPartiallyBound = VK_TRUE;
    enable.f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    enable.f12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    enable.f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    enable.f12.shaderStorageImageArrayNonUniformIndexing = supported.f12.shaderStorageImageArrayNonUniformIndexing;
    enable.f12.scalarBlockLayout = VK_TRUE;
    enable.f12.timelineSemaphore = supported.f12.timelineSemaphore;
    enable.f13.dynamicRendering = VK_TRUE;
    enable.f13.synchronization2 = VK_TRUE;
    enable.f13.maintenance4 = supported.f13.maintenance4;
    enable.f13.shaderDemoteToHelperInvocation = supported.f13.shaderDemoteToHelperInvocation; // `discard` (alpha cut-outs) in SPIR-V 1.6

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asEnable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqEnable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtpEnable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};

    std::vector<const char*> enabledExts{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (rt) {
        enabledExts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        enabledExts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        enabledExts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        asEnable.accelerationStructure = VK_TRUE;
        rqEnable.rayQuery = VK_TRUE;
        enable.f13.pNext = &asEnable;
        asEnable.pNext = &rqEnable;
        if (rtPipeline) {
            enabledExts.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            rtpEnable.rayTracingPipeline = VK_TRUE;
            rqEnable.pNext = &rtpEnable;
        }
    }
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT unusedEnable{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT};
    if (unusedAttachments) { // color slots left empty (a pipeline output not wanted this frame)
        enabledExts.push_back(VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
        unusedEnable.dynamicRenderingUnusedAttachments = VK_TRUE;
        unusedEnable.pNext = enable.f13.pNext;
        enable.f13.pNext = &unusedEnable;
    }

    f32 priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &enable.f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<u32>(enabledExts.size());
    ci.ppEnabledExtensionNames = enabledExts.data();
    VK_CHECK(vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

#define NOVA_LOAD_DEVICE(fn) m_ext.fn = reinterpret_cast<PFN_##fn>(vkGetDeviceProcAddr(m_device, #fn))
    if (rt) {
        NOVA_LOAD_DEVICE(vkCreateAccelerationStructureKHR);
        NOVA_LOAD_DEVICE(vkDestroyAccelerationStructureKHR);
        NOVA_LOAD_DEVICE(vkGetAccelerationStructureBuildSizesKHR);
        NOVA_LOAD_DEVICE(vkGetAccelerationStructureDeviceAddressKHR);
        NOVA_LOAD_DEVICE(vkCmdBuildAccelerationStructuresKHR);
        NOVA_LOAD_DEVICE(vkCmdWriteAccelerationStructuresPropertiesKHR);
        NOVA_LOAD_DEVICE(vkCmdCopyAccelerationStructureKHR);

        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &asProps;
        vkGetPhysicalDeviceProperties2(m_physicalDevice, &p2);
        m_asScratchAlignment = std::max<u64>(asProps.minAccelerationStructureScratchOffsetAlignment, 1);
    }
    if (rtPipeline) {
        NOVA_LOAD_DEVICE(vkCreateRayTracingPipelinesKHR);
        NOVA_LOAD_DEVICE(vkGetRayTracingShaderGroupHandlesKHR);
        NOVA_LOAD_DEVICE(vkCmdTraceRaysKHR);

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &rtProps;
        vkGetPhysicalDeviceProperties2(m_physicalDevice, &p2);
        m_sbtHandleSize = rtProps.shaderGroupHandleSize;
        m_sbtHandleAlignment = std::max(rtProps.shaderGroupHandleAlignment, 1u);
        m_sbtBaseAlignment = std::max(rtProps.shaderGroupBaseAlignment, 1u);
        m_maxRayRecursion = std::max(rtProps.maxRayRecursionDepth, 1u);
    }
#undef NOVA_LOAD_DEVICE

    m_caps.deviceName = m_properties.deviceName;
    m_caps.apiName = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(m_properties.apiVersion)) + "." +
                     std::to_string(VK_API_VERSION_MINOR(m_properties.apiVersion)) + "." +
                     std::to_string(VK_API_VERSION_PATCH(m_properties.apiVersion));
    m_caps.accelerationStructures = rt;
    m_caps.rayQuery = rt;
    m_caps.rayTracingPipeline = rtPipeline;
    setRayTracingShaderStages(rtPipeline);
    m_caps.wireframe = enable.f2.features.fillModeNonSolid == VK_TRUE;
    m_caps.clipDistance = enable.f2.features.shaderClipDistance == VK_TRUE;
    m_caps.drawIndirectFirstInstance = enable.f2.features.drawIndirectFirstInstance == VK_TRUE;
    m_caps.multiDrawIndirect = enable.f2.features.multiDrawIndirect == VK_TRUE;
    m_caps.unusedAttachments = unusedAttachments;
    m_caps.framesInFlight = kFramesInFlight;
    m_caps.maxPushConstantSize = std::min(kPushConstantSize, m_properties.limits.maxPushConstantsSize);
}

// Compiled pipelines kept between runs: the file's data seeds the cache (the driver ignores data from
// another GPU or driver version), and the cache is written back at shutdown.
void VulkanDevice::createPipelineCache(const std::string& file) {
    m_pipelineCacheFile = file;
    std::vector<char> data;
    if (!file.empty()) {
        std::ifstream in(file, std::ios::binary);
        if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache) != VK_SUCCESS) { // unusable data: start empty
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        VK_CHECK(vkCreatePipelineCache(m_device, &ci, nullptr, &m_pipelineCache));
    }
    if (!data.empty()) NOVA_CORE_INFO("RHI: pipeline cache loaded ({} KB)", data.size() / 1024);
}

void VulkanDevice::savePipelineCache() {
    if (m_pipelineCacheFile.empty() || !m_pipelineCache) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_pipelineCacheFile).parent_path(), ec);
    std::ofstream out(m_pipelineCacheFile, std::ios::binary | std::ios::trunc);
    if (out) out.write(data.data(), static_cast<std::streamsize>(size));
}

void VulkanDevice::createAllocator() {
    VmaVulkanFunctions fns{};
    fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.physicalDevice = m_physicalDevice;
    ci.device = m_device;
    ci.instance = m_instance;
    ci.pVulkanFunctions = &fns;
    VK_CHECK(vmaCreateAllocator(&ci, &m_allocator));
}

void VulkanDevice::createSamplers() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &f);
    if (f.samplerAnisotropy) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(16.0f, m_properties.limits.maxSamplerAnisotropy);
    }
    VK_CHECK(vkCreateSampler(m_device, &si, nullptr, &m_defaultSampler));
}

void VulkanDevice::createBindlessLayout() {
    VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &p2);

    const u32 maxSampled = std::min({4096u, p12.maxDescriptorSetUpdateAfterBindSampledImages,
                                     p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                     p12.maxDescriptorSetUpdateAfterBindSamplers});
    const u32 maxStorage = std::min({1024u, p12.maxDescriptorSetUpdateAfterBindStorageImages,
                                     p12.maxPerStageDescriptorUpdateAfterBindStorageImages});
    m_sampledSlots.init(maxSampled);
    m_storageSlots.init(maxStorage);
    m_caps.maxBindlessTextures = maxSampled;

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {kBindingSampledTextures, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSampled, VK_SHADER_STAGE_ALL,
                   nullptr};
    bindings[1] = {kBindingStorageImages, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxStorage, VK_SHADER_STAGE_ALL, nullptr};
    VkDescriptorBindingFlags flags[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT};

    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 2;
    bf.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bf;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 2;
    li.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_bindlessLayout));

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxSampled},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxStorage}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(m_device, &pi, nullptr, &m_bindlessPool));

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_bindlessPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_bindlessLayout;
    VK_CHECK(vkAllocateDescriptorSets(m_device, &ai, &m_bindlessSet));

    VkPushConstantRange pcr{VK_SHADER_STAGE_ALL, 0, m_caps.maxPushConstantSize};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &m_bindlessLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(m_device, &pli, nullptr, &m_pipelineLayout));
}

void VulkanDevice::createFrames() {
    for (Frame& f : m_frames) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = m_queueFamily;
        VK_CHECK(vkCreateCommandPool(m_device, &pi, nullptr, &f.pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(m_device, &ai, &f.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(m_device, &fi, nullptr, &f.fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(m_device, &si, nullptr, &f.imageAvailable));
    }

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = m_queueFamily;
    VK_CHECK(vkCreateCommandPool(m_device, &pi, nullptr, &m_uploadPool));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(m_device, &fi, nullptr, &m_uploadFence));
}

// --------------------------------------------------------------- swapchain --
void VulkanDevice::createSwapchain() {
    Extent2D fb = m_window.framebufferSize();
    m_requestedExtent = fb;
    if (fb.isZero()) return;

    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &caps));

    u32 count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &count, formats.data());
    VkSurfaceFormatKHR chosen = formats[0];
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        auto it = std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
            return f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (it != formats.end()) { chosen = *it; break; }
    }

    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &count, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!m_vsync) {
        for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(modes.begin(), modes.end(), want) != modes.end()) { presentMode = want; break; }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(fb.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(fb.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return;

    u32 imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    VkSwapchainKHR old = m_swapchain;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const bool copyable = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0; // screenshots
    if (copyable) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VK_CHECK(vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain));
    if (old) vkDestroySwapchainKHR(m_device, old, nullptr);

    m_swapchainFormat = chosen.format;
    m_swapchainExtent = extent;

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, nullptr);
    m_swapchainImages.resize(count);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &count, m_swapchainImages.data());

    for (VkImage image : m_swapchainImages) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_swapchainFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VulkanTexture tex;
        tex.image = image;
        VK_CHECK(vkCreateImageView(m_device, &vi, nullptr, &tex.view));
        tex.format = m_swapchainFormat;
        tex.extent = extent;
        tex.usage = TextureUsage::RenderTarget | TextureUsage::TransferDst | (copyable ? TextureUsage::TransferSrc : TextureUsage::None);
        tex.ownsImage = false;
        m_swapchainTextures.push_back(m_textures.insert(tex));

        VkSemaphore sem;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(m_device, &si, nullptr, &sem));
        m_renderFinished.push_back(sem);
    }
    NOVA_CORE_INFO("Swapchain {}x{} ({} images, {})", extent.width, extent.height, count,
                   presentMode == VK_PRESENT_MODE_FIFO_KHR      ? "FIFO/vsync"
                   : presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                                                                : "immediate");
}

void VulkanDevice::destroySwapchainResources() {
    for (TextureHandle h : m_swapchainTextures) {
        if (VulkanTexture* t = m_textures.get(h)) vkDestroyImageView(m_device, t->view, nullptr);
        m_textures.remove(h);
    }
    m_swapchainTextures.clear();
    m_swapchainImages.clear();
    for (VkSemaphore s : m_renderFinished) vkDestroySemaphore(m_device, s, nullptr);
    m_renderFinished.clear();
}

void VulkanDevice::recreateSwapchain() {
    if (m_window.framebufferSize().isZero()) return;
    vkDeviceWaitIdle(m_device);
    destroySwapchainResources();
    createSwapchain();
    m_swapchainDirty = false;
}

void VulkanDevice::setVSync(bool enabled) {
    if (enabled == m_vsync) return;
    m_vsync = enabled;
    m_swapchainDirty = true;
}

// ------------------------------------------------------------------ frames --
bool VulkanDevice::beginFrame() {
    NOVA_ASSERT(!m_inFrame, "beginFrame called twice");
    Extent2D fb = m_window.framebufferSize();
    if (fb.isZero()) return false;
    if (m_swapchainDirty || !m_swapchain || fb != m_requestedExtent) {
        recreateSwapchain();
        if (!m_swapchain || m_swapchainTextures.empty()) return false;
    }

    Frame& f = m_frames[m_frameIndex];
    VK_CHECK(vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX));
    flushReleases(false);

    VkResult r = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE,
                                       &m_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m_swapchainDirty = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    else VK_CHECK(r);

    VK_CHECK(vkResetFences(m_device, 1, &f.fence));
    VK_CHECK(vkResetCommandPool(m_device, f.pool, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    m_commandList.reset(f.cmd);

    m_textures.get(backbuffer())->state = ResourceState::Undefined; // previous contents are discarded
    m_inFrame = true;
    return true;
}

void VulkanDevice::endFrame() {
    NOVA_ASSERT(m_inFrame, "endFrame without beginFrame");
    Frame& f = m_frames[m_frameIndex];
    recordTransition(f.cmd, *m_textures.get(backbuffer()), ResourceState::Present);
    VK_CHECK(vkEndCommandBuffer(f.cmd));

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = m_renderFinished[m_imageIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = f.cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(m_queue, 1, &submit, f.fence));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &m_renderFinished[m_imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &m_swapchain;
    pi.pImageIndices = &m_imageIndex;
    VkResult r = vkQueuePresentKHR(m_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    else VK_CHECK(r);

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
    ++m_frameCounter;
    m_inFrame = false;
}

void VulkanDevice::waitIdle() {
    vkDeviceWaitIdle(m_device);
    flushReleases(true);
}

void VulkanDevice::deferRelease(std::function<void()> fn) {
    // Tagging with the current counter is conservative: if we're between frames it's the next frame.
    m_releases.push_back({m_frameCounter, std::move(fn)});
}

void VulkanDevice::flushReleases(bool all) {
    while (!m_releases.empty() && (all || m_releases.front().frame + kFramesInFlight <= m_frameCounter)) {
        auto fn = std::move(m_releases.front().fn);
        m_releases.pop_front();
        fn();
    }
}

void VulkanDevice::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m_uploadPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    record(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    VK_CHECK(vkQueueSubmit2(m_queue, 1, &submit, m_uploadFence));
    VK_CHECK(vkWaitForFences(m_device, 1, &m_uploadFence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(m_device, 1, &m_uploadFence));
    VK_CHECK(vkResetCommandPool(m_device, m_uploadPool, 0));
}

void VulkanDevice::setDebugName(VkObjectType type, u64 handle, const char* name) {
    if (!name || !m_ext.vkSetDebugUtilsObjectNameEXT) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    m_ext.vkSetDebugUtilsObjectNameEXT(m_device, &info);
}

// ----------------------------------------------------------------- buffers --
BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
    NOVA_ASSERT(desc.size > 0, "Buffer size must be > 0");
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = desc.size;
    bi.usage = toVkUsage(desc.usage, m_caps.accelerationStructures);
    if (desc.memory == MemoryUsage::GpuOnly) bi.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo ai{};
    switch (desc.memory) {
        case MemoryUsage::GpuOnly: ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
        case MemoryUsage::CpuToGpu:
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case MemoryUsage::GpuToCpu:
            ai.usage = VMA_MEMORY_USAGE_AUTO;
            ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
    }

    VulkanBuffer b;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(m_allocator, &bi, &ai, &b.buffer, &b.allocation, &info));
    b.size = desc.size;
    b.mapped = info.pMappedData;
    VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addr.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(m_device, &addr);
    setDebugName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(b.buffer), desc.debugName);
    if (desc.debugName) vmaSetAllocationName(m_allocator, b.allocation, desc.debugName);
    return m_buffers.insert(b);
}

void VulkanDevice::destroyBufferNow(VulkanBuffer& b) {
    vmaDestroyBuffer(m_allocator, b.buffer, b.allocation);
    b = {};
}

void VulkanDevice::destroyBuffer(BufferHandle h) {
    VulkanBuffer* b = m_buffers.get(h);
    if (!b) return;
    VulkanBuffer copy = *b;
    m_buffers.remove(h);
    deferRelease([this, copy]() mutable { destroyBufferNow(copy); });
}

void* VulkanDevice::mappedPointer(BufferHandle h) {
    VulkanBuffer* b = m_buffers.get(h);
    return b ? b->mapped : nullptr;
}

u64 VulkanDevice::deviceAddress(BufferHandle h) {
    VulkanBuffer* b = m_buffers.get(h);
    return b ? b->address : 0;
}

u64 VulkanDevice::bufferSize(BufferHandle h) {
    VulkanBuffer* b = m_buffers.get(h);
    return b ? b->size : 0;
}

void VulkanDevice::uploadBuffer(BufferHandle dst, const void* data, u64 size, u64 dstOffset) {
    VulkanBuffer* b = m_buffers.get(dst);
    NOVA_VERIFY(b && dstOffset + size <= b->size, "uploadBuffer out of range");
    if (b->mapped) {
        std::memcpy(static_cast<u8*>(b->mapped) + dstOffset, data, size);
        return;
    }
    BufferHandle staging = createBuffer({size, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "staging"});
    VulkanBuffer* s = m_buffers.get(staging);
    std::memcpy(s->mapped, data, size);
    VkBuffer src = s->buffer, dstBuf = m_buffers.get(dst)->buffer;
    immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, dstOffset, size};
        vkCmdCopyBuffer(cmd, src, dstBuf, 1, &region);
    });
    VulkanBuffer copy = *s;
    m_buffers.remove(staging);
    destroyBufferNow(copy); // the GPU already finished (synchronous)
}

// ---------------------------------------------------------------- textures --
TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
    VulkanTexture t;
    t.format = toVk(desc.format);
    t.extent = {desc.width, desc.height};
    t.mipLevels = desc.mipLevels;
    t.usage = desc.usage;
    t.aspect = isDepthFormat(desc.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageUsageFlags usage = 0;
    if (any(desc.usage & TextureUsage::Sampled)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (any(desc.usage & TextureUsage::Storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(desc.usage & TextureUsage::RenderTarget)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (any(desc.usage & TextureUsage::DepthStencil)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (any(desc.usage & TextureUsage::TransferSrc)) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (any(desc.usage & TextureUsage::TransferDst)) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = t.format;
    ii.extent = {desc.width, desc.height, 1};
    ii.mipLevels = desc.mipLevels;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (any(desc.usage & (TextureUsage::RenderTarget | TextureUsage::DepthStencil)))
        ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VK_CHECK(vmaCreateImage(m_allocator, &ii, &ai, &t.image, &t.allocation, nullptr));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.format;
    vi.subresourceRange = {t.aspect, 0, desc.mipLevels, 0, 1};
    VK_CHECK(vkCreateImageView(m_device, &vi, nullptr, &t.view));
    setDebugName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(t.image), desc.debugName);
    if (desc.debugName) vmaSetAllocationName(m_allocator, t.allocation, desc.debugName);

    VkDescriptorImageInfo imgInfo{};
    VkWriteDescriptorSet writes[2]{};
    u32 writeCount = 0;
    if (any(desc.usage & TextureUsage::Sampled)) {
        t.sampledIndex = m_sampledSlots.allocate();
        NOVA_VERIFY(t.sampledIndex != kInvalidIndex, "Out of bindless texture slots");
        imgInfo = {m_defaultSampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet& w = writes[writeCount++];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = m_bindlessSet;
        w.dstBinding = kBindingSampledTextures;
        w.dstArrayElement = t.sampledIndex;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &imgInfo;
    }
    VkDescriptorImageInfo storageInfo{};
    if (any(desc.usage & TextureUsage::Storage)) {
        t.storageIndex = m_storageSlots.allocate();
        NOVA_VERIFY(t.storageIndex != kInvalidIndex, "Out of bindless storage image slots");
        storageInfo = {VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet& w = writes[writeCount++];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = m_bindlessSet;
        w.dstBinding = kBindingStorageImages;
        w.dstArrayElement = t.storageIndex;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &storageInfo;
    }
    if (writeCount) vkUpdateDescriptorSets(m_device, writeCount, writes, 0, nullptr);

    return m_textures.insert(t);
}

void VulkanDevice::destroyTexture(TextureHandle h) {
    VulkanTexture* t = m_textures.get(h);
    if (!t || !t->ownsImage) return;
    if (m_imguiRenderer) m_imguiRenderer->onTextureDestroyed(h);
    VulkanTexture copy = *t;
    m_textures.remove(h);
    deferRelease([this, copy]() {
        vkDestroyImageView(m_device, copy.view, nullptr);
        vmaDestroyImage(m_allocator, copy.image, copy.allocation);
        m_sampledSlots.release(copy.sampledIndex);
        m_storageSlots.release(copy.storageIndex);
    });
}

void VulkanDevice::uploadTexture(TextureHandle h, const void* data, u64 size) {
    VulkanTexture* t = m_textures.get(h);
    NOVA_VERIFY(t, "uploadTexture: invalid texture");
    BufferHandle staging = createBuffer({size, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "staging"});
    VulkanBuffer* s = m_buffers.get(staging);
    std::memcpy(s->mapped, data, size);
    VkBuffer src = s->buffer;
    immediateSubmit([&](VkCommandBuffer cmd) {
        recordTransition(cmd, *t, ResourceState::CopyDst);
        VkBufferImageCopy region{};
        region.imageSubresource = {t->aspect, 0, 0, 1};
        region.imageExtent = {t->extent.width, t->extent.height, 1};
        vkCmdCopyBufferToImage(cmd, src, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        recordTransition(cmd, *t, ResourceState::ShaderRead);
    });
    VulkanBuffer copy = *s;
    m_buffers.remove(staging);
    destroyBufferNow(copy);
}

Extent2D VulkanDevice::textureExtent(TextureHandle h) {
    VulkanTexture* t = m_textures.get(h);
    return t ? Extent2D{t->extent.width, t->extent.height} : Extent2D{};
}

ResourceState VulkanDevice::textureState(TextureHandle h) {
    VulkanTexture* t = m_textures.get(h);
    return t ? t->state : ResourceState::Undefined;
}

u32 VulkanDevice::bindlessIndex(TextureHandle h) {
    VulkanTexture* t = m_textures.get(h);
    return t ? t->sampledIndex : kInvalidIndex;
}

u32 VulkanDevice::storageIndex(TextureHandle h) {
    VulkanTexture* t = m_textures.get(h);
    return t ? t->storageIndex : kInvalidIndex;
}

// --------------------------------------------------------------- pipelines --
VkShaderModule VulkanDevice::createShaderModule(const ShaderDesc& desc) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = desc.spirv.size() * sizeof(u32);
    ci.pCode = desc.spirv.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(m_device, &ci, nullptr, &m));
    return m;
}

PipelineHandle VulkanDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    NOVA_VERIFY(desc.vertex.valid(), "Graphics pipeline needs a vertex shader");
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    VkShaderModule vs = createShaderModule(desc.vertex);
    stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs,
                      desc.vertex.entryPoint.c_str(), nullptr});
    VkShaderModule fs = VK_NULL_HANDLE;
    if (desc.fragment.valid()) {
        fs = createShaderModule(desc.fragment);
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                          VK_SHADER_STAGE_FRAGMENT_BIT, fs, desc.fragment.entryPoint.c_str(), nullptr});
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = toVk(desc.topology);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = (desc.polygonMode == PolygonMode::Line && m_caps.wireframe) ? VK_POLYGON_MODE_LINE
                                                                                  : VK_POLYGON_MODE_FILL;
    rs.cullMode = toVk(desc.cullMode);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // engine convention: CCW = front (like glTF)
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = desc.depthTest;
    ds.depthWriteEnable = desc.depthWrite;
    ds.depthCompareOp = toVk(desc.depthCompare);

    std::vector<VkPipelineColorBlendAttachmentState> blends(desc.colorFormats.size());
    for (auto& b : blends) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
        if (desc.blend == BlendMode::Alpha) {
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.colorBlendOp = VK_BLEND_OP_ADD;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.alphaBlendOp = VK_BLEND_OP_ADD;
        } else if (desc.blend == BlendMode::Additive) {
            b.blendEnable = VK_TRUE;
            b.srcColorBlendFactor = b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.srcAlphaBlendFactor = b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = static_cast<u32>(blends.size());
    cb.pAttachments = blends.data();

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamicStates;

    std::vector<VkFormat> colorFormats;
    for (Format f : desc.colorFormats) colorFormats.push_back(toVk(f));
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = static_cast<u32>(colorFormats.size());
    ri.pColorAttachmentFormats = colorFormats.data();
    ri.depthAttachmentFormat = toVk(desc.depthFormat);
    if (desc.depthFormat == Format::D24_UNORM_S8_UINT) ri.stencilAttachmentFormat = ri.depthAttachmentFormat;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &ri;
    pci.stageCount = static_cast<u32>(stages.size());
    pci.pStages = stages.data();
    pci.pVertexInputState = &vertexInput;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = m_pipelineLayout;

    VulkanPipeline p;
    p.bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VK_CHECK(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &pci, nullptr, &p.pipeline));
    vkDestroyShaderModule(m_device, vs, nullptr);
    if (fs) vkDestroyShaderModule(m_device, fs, nullptr);
    setDebugName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(p.pipeline), desc.debugName);
    return m_pipelines.insert(p);
}

PipelineHandle VulkanDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    VkShaderModule cs = createShaderModule(desc.compute);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs,
                desc.compute.entryPoint.c_str(), nullptr};
    ci.layout = m_pipelineLayout;
    VulkanPipeline p;
    p.bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    VK_CHECK(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &p.pipeline));
    vkDestroyShaderModule(m_device, cs, nullptr);
    setDebugName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(p.pipeline), desc.debugName);
    return m_pipelines.insert(p);
}

// Groups in order: ray generation, the misses, the hit groups. The shader binding table holds
// their handles in three regions, each starting on the base alignment.
PipelineHandle VulkanDevice::createRayTracingPipeline(const RayTracingPipelineDesc& desc) {
    NOVA_VERIFY(m_caps.rayTracingPipeline, "createRayTracingPipeline: ray tracing pipelines not supported/enabled");
    NOVA_VERIFY(desc.rayGen.valid(), "createRayTracingPipeline: no ray generation shader");
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    std::vector<VkShaderModule> modules;
    auto addStage = [&](const ShaderDesc& s, VkShaderStageFlagBits stage) -> u32 {
        if (!s.valid()) return VK_SHADER_UNUSED_KHR;
        VkShaderModule m = createShaderModule(s);
        modules.push_back(m);
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, stage, m, s.entryPoint.c_str(), nullptr});
        return static_cast<u32>(stages.size() - 1);
    };
    auto general = [&](u32 stage) {
        VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        g.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        g.generalShader = stage;
        g.closestHitShader = g.anyHitShader = g.intersectionShader = VK_SHADER_UNUSED_KHR;
        groups.push_back(g);
    };
    general(addStage(desc.rayGen, VK_SHADER_STAGE_RAYGEN_BIT_KHR));
    for (const ShaderDesc& m : desc.miss) general(addStage(m, VK_SHADER_STAGE_MISS_BIT_KHR));
    for (const HitGroupDesc& h : desc.hitGroups) {
        VkRayTracingShaderGroupCreateInfoKHR g{VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
        g.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        g.generalShader = g.intersectionShader = VK_SHADER_UNUSED_KHR;
        g.closestHitShader = addStage(h.closestHit, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR);
        g.anyHitShader = addStage(h.anyHit, VK_SHADER_STAGE_ANY_HIT_BIT_KHR);
        groups.push_back(g);
    }

    VkRayTracingPipelineCreateInfoKHR ci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    ci.stageCount = static_cast<u32>(stages.size());
    ci.pStages = stages.data();
    ci.groupCount = static_cast<u32>(groups.size());
    ci.pGroups = groups.data();
    ci.maxPipelineRayRecursionDepth = std::clamp(desc.maxRecursionDepth, 1u, m_maxRayRecursion);
    ci.layout = m_pipelineLayout;
    VulkanPipeline p;
    p.bindPoint = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
    VK_CHECK(m_ext.vkCreateRayTracingPipelinesKHR(m_device, VK_NULL_HANDLE, m_pipelineCache, 1, &ci, nullptr, &p.pipeline));
    for (VkShaderModule m : modules) vkDestroyShaderModule(m_device, m, nullptr);
    setDebugName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(p.pipeline), desc.debugName);

    // The shader binding table.
    const u32 groupCount = static_cast<u32>(groups.size());
    std::vector<u8> handles(size_t(groupCount) * m_sbtHandleSize);
    VK_CHECK(m_ext.vkGetRayTracingShaderGroupHandlesKHR(m_device, p.pipeline, 0, groupCount, handles.size(), handles.data()));
    auto alignUp = [](u64 v, u64 a) { return (v + a - 1) / a * a; };
    const u64 stride = alignUp(m_sbtHandleSize, m_sbtHandleAlignment);
    const u64 missCount = desc.miss.size(), hitCount = desc.hitGroups.size();
    const u64 rayGenSize = alignUp(stride, m_sbtBaseAlignment);
    const u64 missSize = alignUp(stride * std::max<u64>(missCount, 1), m_sbtBaseAlignment);
    const u64 hitSize = alignUp(stride * std::max<u64>(hitCount, 1), m_sbtBaseAlignment);
    const u64 total = rayGenSize + missSize + hitSize;
    // Room to align its start to the base alignment.
    p.sbt = createBuffer({total + m_sbtBaseAlignment, BufferUsage::ShaderBindingTable, MemoryUsage::CpuToGpu,
                          desc.debugName ? desc.debugName : "sbt"});
    VulkanBuffer* sbt = m_buffers.get(p.sbt);
    const VkDeviceAddress base = alignUp(sbt->address, m_sbtBaseAlignment);
    u8* dst = static_cast<u8*>(sbt->mapped) + (base - sbt->address);
    std::memset(dst, 0, total);
    std::memcpy(dst, handles.data(), m_sbtHandleSize);
    for (u64 k = 0; k < missCount; ++k)
        std::memcpy(dst + rayGenSize + k * stride, handles.data() + (1 + k) * m_sbtHandleSize, m_sbtHandleSize);
    for (u64 k = 0; k < hitCount; ++k)
        std::memcpy(dst + rayGenSize + missSize + k * stride, handles.data() + (1 + missCount + k) * m_sbtHandleSize,
                    m_sbtHandleSize);
    p.rayGen = {base, rayGenSize, rayGenSize}; // ray generation: stride = size
    p.miss = {missCount ? base + rayGenSize : 0, stride, missCount ? stride * missCount : 0};
    p.hit = {hitCount ? base + rayGenSize + missSize : 0, stride, hitCount ? stride * hitCount : 0};
    return m_pipelines.insert(p);
}

void VulkanDevice::destroyPipeline(PipelineHandle h) {
    VulkanPipeline* p = m_pipelines.get(h);
    if (!p) return;
    VkPipeline pipe = p->pipeline;
    if (p->sbt) destroyBuffer(p->sbt); // deferred as well
    m_pipelines.remove(h);
    deferRelease([this, pipe]() { vkDestroyPipeline(m_device, pipe, nullptr); });
}

// --------------------------------------------------- acceleration structures --
AccelStructHandle VulkanDevice::createBLAS(const BLASDesc& desc) {
    NOVA_VERIFY(m_caps.accelerationStructures, "createBLAS: ray tracing not supported/enabled");
    VulkanBuffer* vb = m_buffers.get(desc.vertexBuffer);
    VulkanBuffer* ib = m_buffers.get(desc.indexBuffer);
    NOVA_VERIFY(vb && ib, "createBLAS: invalid geometry buffers");

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geom.flags = desc.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    auto& tri = geom.geometry.triangles;
    tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    tri.vertexFormat = toVk(desc.vertexFormat);
    tri.vertexData.deviceAddress = vb->address + desc.vertexOffset;
    tri.vertexStride = desc.vertexStride;
    tri.maxVertex = desc.vertexCount - 1;
    tri.indexType = VK_INDEX_TYPE_UINT32;
    tri.indexData.deviceAddress = ib->address + desc.indexOffset;

    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    // Updatable BLAS are refitted in place; the others are compacted after their build.
    build.flags |= desc.allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                                    : VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    const u32 primitiveCount = desc.indexCount / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_ext.vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                                  &primitiveCount, &sizes);

    VulkanAccelStruct as;
    as.storage = createBuffer({sizes.accelerationStructureSize, BufferUsage::AccelStructStorage,
                               MemoryUsage::GpuOnly, desc.debugName});
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = m_buffers.get(as.storage)->buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(m_ext.vkCreateAccelerationStructureKHR(m_device, &ci, nullptr, &as.handle));
    setDebugName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<u64>(as.handle), desc.debugName);

    // Updatable: the scratch memory is kept for the refits (large enough for both).
    const VkDeviceSize scratchSize = desc.allowUpdate ? std::max(sizes.buildScratchSize, sizes.updateScratchSize) : sizes.buildScratchSize;
    BufferHandle scratch = createBuffer(
        {scratchSize + m_asScratchAlignment, BufferUsage::Storage, MemoryUsage::GpuOnly, "blas scratch"});
    build.dstAccelerationStructure = as.handle;
    build.scratchData.deviceAddress = alignUp(m_buffers.get(scratch)->address, m_asScratchAlignment);

    VkAccelerationStructureBuildRangeInfoKHR range{primitiveCount, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    // Static BLAS: the build also writes its compacted size (a query read once the build is done).
    VkQueryPool sizeQuery = VK_NULL_HANDLE;
    if (!desc.allowUpdate) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qi.queryCount = 1;
        if (vkCreateQueryPool(m_device, &qi, nullptr, &sizeQuery) != VK_SUCCESS) sizeQuery = VK_NULL_HANDLE;
    }
    immediateSubmit([&](VkCommandBuffer cmd) {
        if (sizeQuery) vkCmdResetQueryPool(cmd, sizeQuery, 0, 1);
        m_ext.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
        if (sizeQuery) {
            VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            b.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            b.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
            m_ext.vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &as.handle,
                                                                 VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                                 sizeQuery, 0);
        }
    });

    // Compaction: a copy into a structure of the compacted size, the original dropped (nothing has
    // used it yet: the GPU is idle after the immediate submit).
    if (sizeQuery) {
        VkDeviceSize compacted = 0;
        const VkResult got = vkGetQueryPoolResults(m_device, sizeQuery, 0, 1, sizeof(compacted), &compacted,
                                                   sizeof(compacted), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        vkDestroyQueryPool(m_device, sizeQuery, nullptr);
        if (got == VK_SUCCESS && compacted > 0 && compacted < sizes.accelerationStructureSize) {
            VulkanAccelStruct small;
            small.storage = createBuffer({compacted, BufferUsage::AccelStructStorage, MemoryUsage::GpuOnly, desc.debugName});
            VkAccelerationStructureCreateInfoKHR sci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            sci.buffer = m_buffers.get(small.storage)->buffer;
            sci.size = compacted;
            sci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            VK_CHECK(m_ext.vkCreateAccelerationStructureKHR(m_device, &sci, nullptr, &small.handle));
            setDebugName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<u64>(small.handle), desc.debugName);
            immediateSubmit([&](VkCommandBuffer cmd) {
                VkCopyAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR};
                copy.src = as.handle;
                copy.dst = small.handle;
                copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
                m_ext.vkCmdCopyAccelerationStructureKHR(cmd, &copy);
            });
            m_ext.vkDestroyAccelerationStructureKHR(m_device, as.handle, nullptr);
            VulkanBuffer original = *m_buffers.get(as.storage);
            m_buffers.remove(as.storage);
            destroyBufferNow(original);
            m_blasBytesBuilt += sizes.accelerationStructureSize;
            m_blasBytesKept += compacted;
            as.handle = small.handle;
            as.storage = small.storage;
        }
    }

    if (desc.allowUpdate) {
        as.allowUpdate = true;
        as.geometry = geom;
        as.primitiveCount = primitiveCount;
        as.scratch = scratch;
        as.scratchAddress = build.scratchData.deviceAddress;
    } else {
        VulkanBuffer scratchCopy = *m_buffers.get(scratch);
        m_buffers.remove(scratch);
        destroyBufferNow(scratchCopy);
    }

    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = as.handle;
    as.address = m_ext.vkGetAccelerationStructureDeviceAddressKHR(m_device, &ai);
    return m_accelStructs.insert(as);
}

AccelStructHandle VulkanDevice::createTLAS(const TLASDesc& desc) {
    NOVA_VERIFY(m_caps.accelerationStructures, "createTLAS: ray tracing not supported/enabled");

    VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    // Updatable: frames with the same instance count refit it (CommandList::buildTLAS).
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_ext.vkGetAccelerationStructureBuildSizesKHR(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                                  &desc.maxInstances, &sizes);

    VulkanAccelStruct as;
    as.topLevel = true;
    as.maxInstances = desc.maxInstances;
    as.storage = createBuffer({sizes.accelerationStructureSize, BufferUsage::AccelStructStorage,
                               MemoryUsage::GpuOnly, desc.debugName});
    as.scratch = createBuffer({std::max(sizes.buildScratchSize, sizes.updateScratchSize) + m_asScratchAlignment,
                               BufferUsage::Storage, MemoryUsage::GpuOnly, "tlas scratch"});
    as.scratchAddress = alignUp(m_buffers.get(as.scratch)->address, m_asScratchAlignment);

    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = m_buffers.get(as.storage)->buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VK_CHECK(m_ext.vkCreateAccelerationStructureKHR(m_device, &ci, nullptr, &as.handle));
    setDebugName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, reinterpret_cast<u64>(as.handle), desc.debugName);

    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = as.handle;
    as.address = m_ext.vkGetAccelerationStructureDeviceAddressKHR(m_device, &ai);
    return m_accelStructs.insert(as);
}

void VulkanDevice::destroyAccelStruct(AccelStructHandle h) {
    VulkanAccelStruct* as = m_accelStructs.get(h);
    if (!as) return;
    VkAccelerationStructureKHR handle = as->handle;
    BufferHandle storage = as->storage, scratch = as->scratch;
    m_accelStructs.remove(h);
    deferRelease([this, handle]() { m_ext.vkDestroyAccelerationStructureKHR(m_device, handle, nullptr); });
    destroyBuffer(storage);
    if (scratch) destroyBuffer(scratch);
}

u64 VulkanDevice::accelStructAddress(AccelStructHandle h) {
    VulkanAccelStruct* as = m_accelStructs.get(h);
    return as ? as->address : 0;
}

Scope<ImGuiRenderer> VulkanDevice::createImGuiRenderer() { return makeScope<VulkanImGuiRenderer>(*this); }

} // namespace nova::rhi::vk
