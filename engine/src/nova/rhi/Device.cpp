#include "nova/rhi/Device.h"

#include "nova/core/Log.h"

#if NOVA_RHI_VULKAN
    #include "nova/rhi/vulkan/VulkanDevice.h"
#endif

namespace nova::rhi {

Scope<Device> Device::create(Backend backend, const DeviceDesc& desc, platform::Window& window) {
    switch (backend) {
#if NOVA_RHI_VULKAN
        case Backend::Vulkan: return makeScope<vk::VulkanDevice>(desc, window);
#endif
        default: break;
    }
    NOVA_VERIFY(false, "Requested RHI backend is not compiled in");
    return nullptr;
}

} // namespace nova::rhi
