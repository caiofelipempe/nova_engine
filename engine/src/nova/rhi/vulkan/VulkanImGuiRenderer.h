#pragma once

#include "nova/rhi/vulkan/VulkanDevice.h"

#include <unordered_map>

namespace nova::rhi::vk {

// Dear ImGui renderer backend (imgui_impl_vulkan) using dynamic rendering.
class VulkanImGuiRenderer final : public ImGuiRenderer {
public:
    explicit VulkanImGuiRenderer(VulkanDevice& device);
    ~VulkanImGuiRenderer() override;

    void newFrame() override;
    void render(CommandList& cmd) override;
    u64 textureId(TextureHandle texture) override;

    void onTextureDestroyed(TextureHandle texture);

private:
    VulkanDevice& m_device;
    std::unordered_map<u64, VkDescriptorSet> m_textures;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
};

} // namespace nova::rhi::vk
