#include "nova/rhi/vulkan/VulkanImGuiRenderer.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace nova::rhi::vk {

namespace {
void checkResult(VkResult r) {
    if (r != VK_SUCCESS) NOVA_CORE_ERROR("imgui_impl_vulkan: {}", resultString(r));
}
} // namespace

VulkanImGuiRenderer::VulkanImGuiRenderer(VulkanDevice& device) : m_device(device) {
    m_colorFormat = toVk(device.backbufferFormat());

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = device.instance();
    info.PhysicalDevice = device.physicalDevice();
    info.Device = device.vkDevice();
    info.QueueFamily = device.queueFamily();
    info.Queue = device.queue();
    info.DescriptorPoolSize = 1024; // backend-owned pool for fonts + user textures
    info.MinImageCount = 2;
    info.ImageCount = std::max(2u, device.swapchainImageCount());
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_colorFormat;
    info.CheckVkResultFn = checkResult;
    NOVA_VERIFY(ImGui_ImplVulkan_Init(&info), "ImGui_ImplVulkan_Init failed");

    device.setImGuiRenderer(this);
}

VulkanImGuiRenderer::~VulkanImGuiRenderer() {
    m_device.waitIdle();
    for (auto& [key, set] : m_textures) ImGui_ImplVulkan_RemoveTexture(set);
    m_textures.clear();
    ImGui_ImplVulkan_Shutdown();
    m_device.setImGuiRenderer(nullptr);
}

void VulkanImGuiRenderer::newFrame() { ImGui_ImplVulkan_NewFrame(); }

void VulkanImGuiRenderer::render(CommandList& cmd) {
    ImDrawData* data = ImGui::GetDrawData();
    if (!data) return;
    ImGui_ImplVulkan_RenderDrawData(data, static_cast<VulkanCommandList&>(cmd).handle());
}

u64 VulkanImGuiRenderer::textureId(TextureHandle texture) {
    auto it = m_textures.find(texture.key());
    if (it != m_textures.end()) return reinterpret_cast<u64>(it->second);
    VulkanTexture* t = m_device.texture(texture);
    if (!t) return 0;
    VkDescriptorSet set =
        ImGui_ImplVulkan_AddTexture(m_device.defaultSampler(), t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_textures.emplace(texture.key(), set);
    return reinterpret_cast<u64>(set);
}

void VulkanImGuiRenderer::onTextureDestroyed(TextureHandle texture) {
    auto it = m_textures.find(texture.key());
    if (it == m_textures.end()) return;
    VkDescriptorSet set = it->second;
    m_textures.erase(it);
    m_device.deferRelease([set]() { ImGui_ImplVulkan_RemoveTexture(set); });
}

} // namespace nova::rhi::vk
