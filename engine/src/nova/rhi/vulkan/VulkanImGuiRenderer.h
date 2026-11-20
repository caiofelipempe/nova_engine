#pragma once

/// @file
/// @brief Dear ImGui renderer for the Vulkan backend.

#include "nova/rhi/vulkan/VulkanDevice.h"

#include <unordered_map>

namespace nova::rhi::vk {

/// @brief rhi::ImGuiRenderer built on `imgui_impl_vulkan` with dynamic rendering.
///
/// Renders into the swapchain format. Texture IDs are `VkDescriptorSet`s created on demand by
/// textureId() and released (deferred) when the texture is destroyed.
class VulkanImGuiRenderer final : public ImGuiRenderer {
public:
    /// @brief Initializes `imgui_impl_vulkan` for `device`. @pre The ImGui platform backend is initialized.
    explicit VulkanImGuiRenderer(VulkanDevice& device);
    ~VulkanImGuiRenderer() override;

    void newFrame() override;
    void render(CommandList& cmd) override;
    u64 textureId(TextureHandle texture) override;

    /// @brief Called by VulkanDevice::destroyTexture to release the texture's ImGui descriptor (deferred).
    void onTextureDestroyed(TextureHandle texture);

private:
    VulkanDevice& m_device;
    std::unordered_map<u64, VkDescriptorSet> m_textures;
    VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;
};

} // namespace nova::rhi::vk
