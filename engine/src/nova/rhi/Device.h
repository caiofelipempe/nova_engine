#pragma once

/// @file
/// @brief Graphics device, command list and ImGui renderer interfaces.

#include "nova/rhi/RHITypes.h"

#include <span>

namespace nova::platform { class Window; }

namespace nova::rhi {

/// @brief Records GPU commands for the current frame.
///
/// Obtained from Device::commandList() between Device::beginFrame() and Device::endFrame().
/// Every pipeline uses the same layout: the bindless descriptor set plus up to
/// DeviceCaps::maxPushConstantSize bytes of push constants visible to all stages.
class CommandList {
public:
    virtual ~CommandList() = default;

    /// @brief Starts a render pass (dynamic rendering).
    ///
    /// Transitions the attachments to RenderTarget / DepthWrite automatically and sets a
    /// viewport and scissor covering the render area.
    virtual void beginRendering(const RenderingDesc& desc) = 0;
    /// @brief Ends the current render pass.
    virtual void endRendering() = 0;

    /// @brief Binds a graphics or compute pipeline (and the bindless descriptor set).
    virtual void bindPipeline(PipelineHandle pipeline) = 0;
    /// @brief Sets push constants for all stages, starting at offset 0.
    /// @param data Bytes to copy.
    /// @param size Byte count (at most DeviceCaps::maxPushConstantSize).
    virtual void pushConstants(const void* data, u32 size) = 0;
    /// @brief Sets push constants from a struct.
    /// @tparam T Trivially copyable struct mirroring the shader's `push_constant` block.
    template <typename T>
    void pushConstants(const T& data) { pushConstants(&data, sizeof(T)); }

    /// @brief Sets the viewport (pixels, origin top-left).
    virtual void setViewport(f32 x, f32 y, f32 width, f32 height) = 0;
    /// @brief Sets the scissor rectangle (pixels, origin top-left).
    virtual void setScissor(i32 x, i32 y, u32 width, u32 height) = 0;

    /// @brief Binds a buffer of 32-bit indices for drawIndexed().
    virtual void bindIndexBuffer(BufferHandle buffer, u64 offset = 0) = 0;
    /// @brief Non-indexed draw.
    virtual void draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) = 0;
    /// @brief Indexed draw using the bound index buffer.
    virtual void drawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0, i32 vertexOffset = 0,
                             u32 firstInstance = 0) = 0;
    /// @brief Indexed draws whose parameters are in a buffer (e.g. written by a compute shader): each
    ///        a VkDrawIndexedIndirectCommand-like record of 5 × 32 bits (indexCount, instanceCount,
    ///        firstIndex, vertexOffset, firstInstance), using the bound index buffer. A firstInstance
    ///        other than 0 needs DeviceCaps::drawIndirectFirstInstance.
    /// @param args Buffer created with BufferUsage::Indirect.
    /// @param offset Byte offset of the first record (multiple of 4).
    /// @param drawCount Records to draw (more than 1 needs DeviceCaps::multiDrawIndirect).
    /// @param stride Bytes between records (at least 20).
    virtual void drawIndexedIndirect(BufferHandle args, u64 offset, u32 drawCount = 1, u32 stride = 20) = 0;
    /// @brief Compute dispatch, in work groups.
    virtual void dispatch(u32 x, u32 y = 1, u32 z = 1) = 0;

    /// @brief Moves a texture to a new logical state, emitting the needed barrier (no-op if already there).
    virtual void transition(TextureHandle texture, ResourceState state) = 0;
    /// @brief Full execution + memory barrier between all prior and later work (coarse; use for buffers).
    virtual void globalBarrier() = 0;
    /// @brief Copies `size` bytes between buffers.
    virtual void copyBuffer(BufferHandle src, BufferHandle dst, u64 size, u64 srcOffset = 0, u64 dstOffset = 0) = 0;
    /// @brief Copies a texture's first mip into a buffer, rows tightly packed (the texture needs
    ///        TextureUsage::TransferSrc and ends in ResourceState::CopySrc).
    virtual void copyTextureToBuffer(TextureHandle src, BufferHandle dst, u64 dstOffset = 0) = 0;

    /// @brief (Re)builds a TLAS from an array of AccelStructInstance records in GPU memory.
    ///
    /// Inserts the barriers needed so any later shader stage in this command list can trace
    /// against the TLAS, and so earlier traces of the same TLAS finish before it is overwritten.
    /// @param tlas Target, created with Device::createTLAS.
    /// @param instanceAddress Device address of the instance array (16-byte aligned), typically inside a per-frame upload buffer.
    /// @param instanceCount Number of instances (at most TLASDesc::maxInstances).
    virtual void buildTLAS(AccelStructHandle tlas, u64 instanceAddress, u32 instanceCount) = 0;
    /// @brief Refits a BLAS created with BLASDesc::allowUpdate to the current contents of its
    ///        vertex buffer (e.g. written by a compute shader earlier in this command list).
    ///
    /// Inserts the barriers needed: earlier shader writes are visible to the build, earlier traces
    /// of it finish first, and later TLAS builds and traces see the refitted structure.
    virtual void updateBLAS(AccelStructHandle blas) = 0;

    /// @brief Opens a named region shown in graphics debuggers (RenderDoc, Nsight).
    virtual void beginDebugLabel(const char* name) = 0;
    /// @brief Closes the innermost debug region.
    virtual void endDebugLabel() = 0;
};

/// @brief Renders Dear ImGui draw data with the active graphics API.
class ImGuiRenderer {
public:
    virtual ~ImGuiRenderer() = default;
    /// @brief Prepares backend state for a new ImGui frame (call before `ImGui::NewFrame()`).
    virtual void newFrame() = 0;
    /// @brief Draws `ImGui::GetDrawData()` into the render pass currently open on `cmd`.
    virtual void render(CommandList& cmd) = 0;
    /// @brief An `ImTextureID` to display a texture with `ImGui::Image`.
    ///
    /// The texture must be in ResourceState::ShaderRead when the UI is drawn. IDs are cached and
    /// released automatically when the texture is destroyed.
    virtual u64 textureId(TextureHandle texture) = 0;
};

/// @brief Owns the GPU, the swapchain, resources and frame pacing.
///
/// Resource destruction is deferred until the GPU has finished every frame that may still use
/// the resource, so destroying something that was used in the current frame is safe.
class Device : NonCopyable {
public:
    /// @brief Creates a device with the given backend, presenting to `window`.
    /// @note Aborts with a log message if no GPU meets the minimum requirements.
    static Scope<Device> create(Backend backend, const DeviceDesc& desc, platform::Window& window);
    virtual ~Device() = default;

    /// @brief Capabilities of the selected GPU.
    virtual const DeviceCaps& caps() const = 0;

    /// @name Buffers
    /// @{
    /// @brief Creates a buffer. Every buffer has a device address (see deviceAddress()).
    virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    /// @brief Destroys a buffer once the GPU no longer uses it. Invalid handles are ignored.
    virtual void destroyBuffer(BufferHandle buffer) = 0;
    /// @brief Persistent CPU pointer for MemoryUsage::CpuToGpu / GpuToCpu buffers, `nullptr` otherwise.
    virtual void* mappedPointer(BufferHandle buffer) = 0;
    /// @brief 64-bit GPU address for shader access (`buffer_reference` in GLSL).
    virtual u64 deviceAddress(BufferHandle buffer) = 0;
    /// @brief Size in bytes, 0 for invalid handles.
    virtual u64 bufferSize(BufferHandle buffer) = 0;
    /// @brief Copies CPU data into a buffer and waits for completion.
    ///
    /// Mapped buffers are written directly; GPU-only buffers go through a staging buffer.
    /// Fine for loading; use mapped CpuToGpu buffers for per-frame data.
    virtual void uploadBuffer(BufferHandle dst, const void* data, u64 size, u64 dstOffset = 0) = 0;
    /// @}

    /// @name Textures
    /// @{
    /// @brief Creates a 2D texture. Sampled/Storage textures get bindless slots.
    virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
    /// @brief Destroys a texture (deferred) and frees its bindless slots. Swapchain images are ignored.
    virtual void destroyTexture(TextureHandle texture) = 0;
    /// @brief Uploads mip 0 and waits; the texture ends in ResourceState::ShaderRead.
    /// @param data Tightly packed pixels.
    /// @param size Byte count of `data`.
    virtual void uploadTexture(TextureHandle texture, const void* data, u64 size) = 0;
    /// @brief Size in pixels, zero for invalid handles.
    virtual Extent2D textureExtent(TextureHandle texture) = 0;
    /// @brief Current tracked state (Undefined for invalid handles).
    virtual ResourceState textureState(TextureHandle texture) = 0;
    /// @brief Slot in the global sampled-texture array (`g_textures[]` in shaders).
    virtual u32 bindlessIndex(TextureHandle texture) = 0;
    /// @}

    /// @name Pipelines
    /// @{
    /// @brief Creates a graphics pipeline.
    virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    /// @brief Creates a compute pipeline.
    virtual PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;
    /// @brief Destroys a pipeline (deferred).
    virtual void destroyPipeline(PipelineHandle pipeline) = 0;
    /// @}

    /// @name Ray tracing
    /// Valid only when DeviceCaps::accelerationStructures is true.
    /// @{
    /// @brief Creates and builds a BLAS synchronously. The geometry buffers must already contain their data.
    virtual AccelStructHandle createBLAS(const BLASDesc& desc) = 0;
    /// @brief Creates an empty TLAS; fill it with CommandList::buildTLAS.
    virtual AccelStructHandle createTLAS(const TLASDesc& desc) = 0;
    /// @brief Destroys an acceleration structure and its memory (deferred).
    virtual void destroyAccelStruct(AccelStructHandle as) = 0;
    /// @brief Device address of an acceleration structure (for TLAS instances and `accelerationStructureEXT(addr)`).
    virtual u64 accelStructAddress(AccelStructHandle as) = 0;
    /// @}

    /// @name Frame
    /// @{
    /// @brief Waits for a free frame slot and acquires the next swapchain image.
    /// @return False when no frame can be rendered (minimized, or swapchain being recreated); skip the frame.
    virtual bool beginFrame() = 0;
    /// @brief Command list of the current frame. @pre beginFrame() returned true.
    virtual CommandList& commandList() = 0;
    /// @brief Submits the frame and presents. The backbuffer is transitioned to Present automatically.
    virtual void endFrame() = 0;
    /// @brief The swapchain image of the current frame (its previous contents are undefined).
    virtual TextureHandle backbuffer() = 0;
    /// @brief Format of the swapchain images.
    virtual Format backbufferFormat() const = 0;
    /// @brief Size of the swapchain images.
    virtual Extent2D backbufferExtent() const = 0;
    /// @brief Index of the current frame slot, in `[0, DeviceCaps::framesInFlight)`.
    virtual u32 frameIndex() const = 0;
    /// @brief Number of frames submitted so far.
    virtual u64 frameCounter() const = 0;
    /// @brief Enables/disables vsync; the swapchain is recreated at the next beginFrame().
    virtual void setVSync(bool enabled) = 0;
    /// @brief Whether vsync is enabled.
    virtual bool vsync() const = 0;
    /// @brief Blocks until the GPU is idle and releases every deferred resource.
    virtual void waitIdle() = 0;
    /// @}

    /// @brief Creates the Dear ImGui renderer for this backend.
    /// @pre platform::Window::imguiInit() has been called.
    virtual Scope<ImGuiRenderer> createImGuiRenderer() = 0;
};

} // namespace nova::rhi
