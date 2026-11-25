#pragma once

/// @file
/// @brief Vulkan 1.3 implementation of rhi::Device and rhi::CommandList (internal to the backend).

#include "nova/rhi/Device.h"
#include "nova/rhi/HandlePool.h"
#include "nova/rhi/vulkan/VulkanCommon.h"

#include <array>
#include <deque>
#include <functional>
#include <vector>

/// @brief Vulkan backend of the RHI. Not part of the public API: use rhi::Device.
namespace nova::rhi::vk {

class VulkanDevice;
class VulkanImGuiRenderer;

/// @brief Backend storage behind a BufferHandle.
struct VulkanBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;   ///< Vulkan buffer.
    VmaAllocation allocation = nullptr; ///< VMA allocation.
    u64 size = 0;                       ///< Size in bytes.
    void* mapped = nullptr;             ///< Persistent mapping (host-visible buffers only).
    VkDeviceAddress address = 0;        ///< Buffer device address.
};

/// @brief Backend storage behind a TextureHandle.
struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;                        ///< Vulkan image.
    VmaAllocation allocation = nullptr;                    ///< VMA allocation (null for swapchain images).
    VkImageView view = VK_NULL_HANDLE;                     ///< View over all mips.
    VkFormat format = VK_FORMAT_UNDEFINED;                 ///< Vulkan format.
    VkExtent2D extent{};                                   ///< Size in pixels.
    u32 mipLevels = 1;                                     ///< Mip count.
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT; ///< Color or depth aspect.
    TextureUsage usage = TextureUsage::None;               ///< RHI usage flags.
    ResourceState state = ResourceState::Undefined;        ///< Tracked state (see recordTransition).
    u32 sampledIndex = kInvalidIndex;                      ///< Bindless sampled-texture slot.
    u32 storageIndex = kInvalidIndex;                      ///< Bindless storage-image slot.
    bool ownsImage = true;                                 ///< False for swapchain images (owned by the swapchain).
};

/// @brief Backend storage behind a PipelineHandle.
struct VulkanPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;                            ///< Vulkan pipeline.
    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; ///< Graphics or compute.
};

/// @brief Backend storage behind an AccelStructHandle.
struct VulkanAccelStruct {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE; ///< Vulkan acceleration structure.
    BufferHandle storage;                               ///< Buffer holding the structure.
    BufferHandle scratch;                               ///< Build scratch memory, kept for TLAS rebuilds.
    VkDeviceAddress address = 0;                        ///< Acceleration structure device address.
    VkDeviceAddress scratchAddress = 0;                 ///< Scratch address, aligned as the driver requires.
    bool topLevel = false;                              ///< TLAS (true) or BLAS.
    u32 maxInstances = 0;                               ///< TLAS capacity.
    // Updatable BLAS: what it was built from (the buffers stay where they are).
    bool allowUpdate = false;
    VkAccelerationStructureGeometryKHR geometry{};
    u32 primitiveCount = 0;
};

/// @brief Emits an image barrier from the texture's tracked state to `state` and updates the tracking.
/// @note No-op when the state does not change (except UnorderedAccess, which always gets a barrier).
void recordTransition(VkCommandBuffer cmd, VulkanTexture& tex, ResourceState state);

/// @brief Free-list allocator for bindless descriptor slots.
class IndexAllocator {
public:
    /// @brief Resets the allocator to `capacity` free slots.
    void init(u32 capacity) { m_capacity = capacity; m_next = 0; m_free.clear(); }
    /// @brief Returns a free slot, or kInvalidIndex when full.
    u32 allocate() {
        if (!m_free.empty()) { u32 i = m_free.back(); m_free.pop_back(); return i; }
        return m_next < m_capacity ? m_next++ : kInvalidIndex;
    }
    /// @brief Returns a slot to the free list (kInvalidIndex is ignored).
    void release(u32 i) { if (i != kInvalidIndex) m_free.push_back(i); }

private:
    u32 m_capacity = 0;
    u32 m_next = 0;
    std::vector<u32> m_free;
};

/// @brief rhi::CommandList recording into the current frame's `VkCommandBuffer`.
class VulkanCommandList final : public CommandList {
public:
    /// @param device Owning device (resolves handles).
    explicit VulkanCommandList(VulkanDevice& device) : m_device(device) {}
    /// @brief Points the list at a new, already begun command buffer (once per frame).
    void reset(VkCommandBuffer cmd);
    /// @brief The underlying command buffer.
    VkCommandBuffer handle() const { return m_cmd; }

    void beginRendering(const RenderingDesc& desc) override;
    void endRendering() override;
    void bindPipeline(PipelineHandle pipeline) override;
    void pushConstants(const void* data, u32 size) override;
    void setViewport(f32 x, f32 y, f32 width, f32 height) override;
    void setScissor(i32 x, i32 y, u32 width, u32 height) override;
    void bindIndexBuffer(BufferHandle buffer, u64 offset) override;
    void draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
    void drawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance) override;
    void drawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount, u32 stride) override;
    void dispatch(u32 x, u32 y, u32 z) override;
    void transition(TextureHandle texture, ResourceState state) override;
    void globalBarrier() override;
    void copyBuffer(BufferHandle src, BufferHandle dst, u64 size, u64 srcOffset, u64 dstOffset) override;
    void copyTextureToBuffer(TextureHandle src, BufferHandle dst, u64 dstOffset) override;
    void buildTLAS(AccelStructHandle tlas, u64 instanceAddress, u32 instanceCount) override;
    void updateBLAS(AccelStructHandle blas) override;
    void beginDebugLabel(const char* name) override;
    void endDebugLabel() override;

private:
    VulkanDevice& m_device;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
};

/// @brief rhi::Device implemented with Vulkan 1.3.
///
/// Requirements: dynamic rendering, synchronization2, buffer device address, descriptor indexing,
/// scalar block layout and shaderInt64. Ray tracing (acceleration structures + ray query, and
/// ray tracing pipelines when present) is enabled when available and requested.
/// Memory is managed with VMA; resources live in generational HandlePools; destruction is
/// deferred until the frames that may use the resource have completed.
class VulkanDevice final : public Device {
public:
    /// @brief Creates instance, surface, device, allocator, bindless layout, frames and swapchain.
    VulkanDevice(const DeviceDesc& desc, platform::Window& window);
    ~VulkanDevice() override;

    const DeviceCaps& caps() const override { return m_caps; }

    BufferHandle createBuffer(const BufferDesc& desc) override;
    void destroyBuffer(BufferHandle buffer) override;
    void* mappedPointer(BufferHandle buffer) override;
    u64 deviceAddress(BufferHandle buffer) override;
    u64 bufferSize(BufferHandle buffer) override;
    void uploadBuffer(BufferHandle dst, const void* data, u64 size, u64 dstOffset) override;

    TextureHandle createTexture(const TextureDesc& desc) override;
    void destroyTexture(TextureHandle texture) override;
    void uploadTexture(TextureHandle texture, const void* data, u64 size) override;
    Extent2D textureExtent(TextureHandle texture) override;
    ResourceState textureState(TextureHandle texture) override;
    u32 bindlessIndex(TextureHandle texture) override;

    PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;
    void destroyPipeline(PipelineHandle pipeline) override;

    AccelStructHandle createBLAS(const BLASDesc& desc) override;
    AccelStructHandle createTLAS(const TLASDesc& desc) override;
    void destroyAccelStruct(AccelStructHandle as) override;
    u64 accelStructAddress(AccelStructHandle as) override;

    bool beginFrame() override;
    CommandList& commandList() override { return m_commandList; }
    void endFrame() override;
    TextureHandle backbuffer() override { return m_swapchainTextures[m_imageIndex]; }
    Format backbufferFormat() const override { return fromVk(m_swapchainFormat); }
    Extent2D backbufferExtent() const override { return {m_swapchainExtent.width, m_swapchainExtent.height}; }
    u32 frameIndex() const override { return m_frameIndex; }
    u64 frameCounter() const override { return m_frameCounter; }
    void setVSync(bool enabled) override;
    bool vsync() const override { return m_vsync; }
    void waitIdle() override;

    Scope<ImGuiRenderer> createImGuiRenderer() override;

    /// @name Backend internals (used by the command list and the ImGui renderer)
    /// @{
    /// @brief Resolves a handle (`nullptr` if invalid/stale).
    VulkanBuffer* buffer(BufferHandle h) { return m_buffers.get(h); }
    /// @brief Resolves a handle (`nullptr` if invalid/stale).
    VulkanTexture* texture(TextureHandle h) { return m_textures.get(h); }
    /// @brief Resolves a handle (`nullptr` if invalid/stale).
    VulkanPipeline* pipeline(PipelineHandle h) { return m_pipelines.get(h); }
    /// @brief Resolves a handle (`nullptr` if invalid/stale).
    VulkanAccelStruct* accelStruct(AccelStructHandle h) { return m_accelStructs.get(h); }

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice vkDevice() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    u32 queueFamily() const { return m_queueFamily; }
    u32 swapchainImageCount() const { return static_cast<u32>(m_swapchainImages.size()); }
    VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }
    VkDescriptorSet bindlessSet() const { return m_bindlessSet; }
    VkSampler defaultSampler() const { return m_defaultSampler; }
    /// @brief Extension entry points loaded at device creation.
    const ExtFunctions& ext() const { return m_ext; }

    /// @brief Runs `fn` once every frame that may still reference a resource has completed on the GPU.
    void deferRelease(std::function<void()> fn);
    /// @brief Registers the ImGui renderer so destroyed textures release their ImGui descriptors.
    void setImGuiRenderer(VulkanImGuiRenderer* r) { m_imguiRenderer = r; }
    /// @}

private:
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
    };
    struct PendingRelease {
        u64 frame; // last frame counter that may still reference the resource
        std::function<void()> fn;
    };

    void createInstance(platform::Window& window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    void createBindlessLayout();
    void createFrames();
    void createSamplers();
    void createSwapchain();
    void destroySwapchainResources();
    void recreateSwapchain();
    void flushReleases(bool all);
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);
    void setDebugName(VkObjectType type, u64 handle, const char* name);
    VkShaderModule createShaderModule(const ShaderDesc& desc);
    void destroyBufferNow(VulkanBuffer& b);

    DeviceCaps m_caps;
    bool m_vsync = true;
    bool m_validation = false;
    bool m_debugUtils = false;
    bool m_rayTracingRequested = true;
    platform::Window& m_window;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_properties{};
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    u32 m_queueFamily = 0;
    VmaAllocator m_allocator = nullptr;
    ExtFunctions m_ext;
    u64 m_asScratchAlignment = 256;

    VkDescriptorSetLayout m_bindlessLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_bindlessPool = VK_NULL_HANDLE;
    VkDescriptorSet m_bindlessSet = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    IndexAllocator m_sampledSlots;
    IndexAllocator m_storageSlots;
    VkSampler m_defaultSampler = VK_NULL_HANDLE;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D m_swapchainExtent{};
    std::vector<VkImage> m_swapchainImages;
    std::vector<TextureHandle> m_swapchainTextures;
    std::vector<VkSemaphore> m_renderFinished; // one per swapchain image
    u32 m_imageIndex = 0;
    bool m_swapchainDirty = false;
    Extent2D m_requestedExtent{};
    std::deque<PendingRelease> m_releases;

    std::array<Frame, kFramesInFlight> m_frames{};
    u32 m_frameIndex = 0;
    u64 m_frameCounter = 0;
    bool m_inFrame = false;

    VkCommandPool m_uploadPool = VK_NULL_HANDLE;
    VkFence m_uploadFence = VK_NULL_HANDLE;

    VulkanCommandList m_commandList{*this};
    VulkanImGuiRenderer* m_imguiRenderer = nullptr;

    HandlePool<VulkanBuffer, BufferHandle> m_buffers;
    HandlePool<VulkanTexture, TextureHandle> m_textures;
    HandlePool<VulkanPipeline, PipelineHandle> m_pipelines;
    HandlePool<VulkanAccelStruct, AccelStructHandle> m_accelStructs;
};

} // namespace nova::rhi::vk
