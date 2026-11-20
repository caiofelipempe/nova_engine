#pragma once

#include "nova/rhi/RHITypes.h"

#include <span>

namespace nova::platform { class Window; }

namespace nova::rhi {

// Records GPU commands for the current frame. Obtained from Device::commandList().
class CommandList {
public:
    virtual ~CommandList() = default;

    // Transitions attachments automatically, sets a full-target viewport and scissor.
    virtual void beginRendering(const RenderingDesc& desc) = 0;
    virtual void endRendering() = 0;

    virtual void bindPipeline(PipelineHandle pipeline) = 0;
    // Push constants are visible to every stage; max size is DeviceCaps::maxPushConstantSize.
    virtual void pushConstants(const void* data, u32 size) = 0;
    template <typename T>
    void pushConstants(const T& data) { pushConstants(&data, sizeof(T)); }

    virtual void setViewport(f32 x, f32 y, f32 width, f32 height) = 0;
    virtual void setScissor(i32 x, i32 y, u32 width, u32 height) = 0;

    virtual void bindIndexBuffer(BufferHandle buffer, u64 offset = 0) = 0; // 32-bit indices
    virtual void draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) = 0;
    virtual void drawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0, i32 vertexOffset = 0,
                             u32 firstInstance = 0) = 0;
    virtual void dispatch(u32 x, u32 y = 1, u32 z = 1) = 0;

    virtual void transition(TextureHandle texture, ResourceState state) = 0;
    // Full execution + memory barrier between all shader / transfer work (coarse, for buffers).
    virtual void globalBarrier() = 0;
    virtual void copyBuffer(BufferHandle src, BufferHandle dst, u64 size, u64 srcOffset = 0, u64 dstOffset = 0) = 0;

    // (Re)builds a TLAS from `instanceCount` AccelStructInstance records at `instanceAddress`
    // (a device address, typically inside a per-frame upload buffer). Inserts the barriers
    // needed so any later shader stage in this command list can trace against it.
    virtual void buildTLAS(AccelStructHandle tlas, u64 instanceAddress, u32 instanceCount) = 0;

    virtual void beginDebugLabel(const char* name) = 0;
    virtual void endDebugLabel() = 0;
};

// Renders Dear ImGui draw data with the active graphics API.
class ImGuiRenderer {
public:
    virtual ~ImGuiRenderer() = default;
    virtual void newFrame() = 0;
    // Draws ImGui::GetDrawData() into the render pass currently open on `cmd`.
    virtual void render(CommandList& cmd) = 0;
    // An ImTextureID for a sampled texture (must be in ResourceState::ShaderRead when drawn).
    virtual u64 textureId(TextureHandle texture) = 0;
};

// Owns the GPU, the swapchain, resources and frame pacing.
class Device : NonCopyable {
public:
    static Scope<Device> create(Backend backend, const DeviceDesc& desc, platform::Window& window);
    virtual ~Device() = default;

    virtual const DeviceCaps& caps() const = 0;

    // -- buffers
    virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
    virtual void destroyBuffer(BufferHandle buffer) = 0; // deferred until the GPU is done with it
    virtual void* mappedPointer(BufferHandle buffer) = 0; // CpuToGpu / GpuToCpu only
    virtual u64 deviceAddress(BufferHandle buffer) = 0;
    virtual u64 bufferSize(BufferHandle buffer) = 0;
    // Synchronous upload (staging + wait). Fine for loading; use mapped buffers per frame.
    virtual void uploadBuffer(BufferHandle dst, const void* data, u64 size, u64 dstOffset = 0) = 0;

    // -- textures
    virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
    virtual void destroyTexture(TextureHandle texture) = 0;
    virtual void uploadTexture(TextureHandle texture, const void* data, u64 size) = 0; // mip 0, ends in ShaderRead
    virtual Extent2D textureExtent(TextureHandle texture) = 0;
    virtual ResourceState textureState(TextureHandle texture) = 0;
    virtual u32 bindlessIndex(TextureHandle texture) = 0; // slot in the global sampled-texture array

    // -- pipelines
    virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual void destroyPipeline(PipelineHandle pipeline) = 0;

    // -- ray tracing (valid only if caps().accelerationStructures)
    // Builds synchronously; the geometry buffers must already contain their data.
    virtual AccelStructHandle createBLAS(const BLASDesc& desc) = 0;
    virtual AccelStructHandle createTLAS(const TLASDesc& desc) = 0;
    virtual void destroyAccelStruct(AccelStructHandle as) = 0;
    virtual u64 accelStructAddress(AccelStructHandle as) = 0;

    // -- frame
    // Returns false when no frame can be rendered (minimized / swapchain being recreated).
    virtual bool beginFrame() = 0;
    virtual CommandList& commandList() = 0;
    virtual void endFrame() = 0; // submits and presents; backbuffer is transitioned to Present
    virtual TextureHandle backbuffer() = 0;
    virtual Format backbufferFormat() const = 0;
    virtual Extent2D backbufferExtent() const = 0;
    virtual u32 frameIndex() const = 0;   // [0, framesInFlight)
    virtual u64 frameCounter() const = 0; // monotonically increasing
    virtual void setVSync(bool enabled) = 0;
    virtual bool vsync() const = 0;
    virtual void waitIdle() = 0;

    // -- tooling
    // Requires Window::imguiInit() to have been called.
    virtual Scope<ImGuiRenderer> createImGuiRenderer() = 0;
};

} // namespace nova::rhi
