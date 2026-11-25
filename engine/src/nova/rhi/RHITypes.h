#pragma once

/// @file
/// @brief Graphics-API-agnostic handles, formats and resource descriptions.

#include "nova/core/Base.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

/// @brief Render Hardware Interface: the engine's graphics API abstraction.
///
/// **Binding model (all backends): "bindless".**
/// - Buffers are accessed in shaders through 64-bit device addresses passed in push constants.
/// - Sampled textures live in one global descriptor array; Device::bindlessIndex() gives the slot.
/// - Acceleration structures are referenced by device address (rayQuery / traceRay).
///
/// This maps 1:1 to Vulkan (buffer device address + descriptor indexing) and to D3D12
/// (SM 6.6 `ResourceDescriptorHeap`).
namespace nova::rhi {

/// @brief Available graphics API backends.
enum class Backend {
    Vulkan ///< Vulkan 1.3 (dynamic rendering, synchronization2, bindless).
};

// ------------------------------------------------------------------ handles --

/// @brief Typed, generational resource handle.
///
/// A handle is an index into a backend pool plus a generation counter: after the resource is
/// destroyed, old handles stop resolving instead of silently aliasing a newer resource.
/// A default-constructed handle is invalid.
/// @tparam Tag Empty tag type that makes each handle kind a distinct type.
template <typename Tag>
struct Handle {
    u32 index = 0;      ///< Slot in the backend pool.
    u32 generation = 0; ///< Slot generation; 0 means "invalid".
    /// @brief True if the handle was ever assigned (it may still be stale).
    bool valid() const { return generation != 0; }
    explicit operator bool() const { return valid(); }
    bool operator==(const Handle&) const = default;
    /// @brief Unique 64-bit key, suitable for hash maps.
    u64 key() const { return (u64(generation) << 32) | index; }
};

using BufferHandle = Handle<struct BufferTag>;           ///< GPU buffer.
using TextureHandle = Handle<struct TextureTag>;         ///< 2D texture / render target / swapchain image.
using PipelineHandle = Handle<struct PipelineTag>;       ///< Graphics or compute pipeline.
using AccelStructHandle = Handle<struct AccelStructTag>; ///< Ray tracing acceleration structure (BLAS/TLAS).

// ------------------------------------------------------------------ formats --

/// @brief Pixel and vertex formats.
enum class Format : u8 {
    Undefined,
    R8_UNORM,
    RGBA8_UNORM,
    RGBA8_SRGB,
    BGRA8_UNORM,
    BGRA8_SRGB,
    R16_FLOAT,
    RG16_FLOAT,
    RGBA16_FLOAT,
    R32_FLOAT,
    R32_UINT,
    RG32_FLOAT,
    RGB32_FLOAT, ///< Vertex / acceleration-structure input only.
    RGBA32_FLOAT,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
};

/// @brief True for depth (and depth-stencil) formats.
constexpr bool isDepthFormat(Format f) { return f == Format::D32_FLOAT || f == Format::D24_UNORM_S8_UINT; }

// ------------------------------------------------------------------ buffers --

/// @brief How a buffer may be used (bit set; combine with `|`).
enum class BufferUsage : u32 {
    None = 0,
    Vertex = 1 << 0,             ///< Fixed-function vertex input (the engine itself uses vertex pulling).
    Index = 1 << 1,              ///< Index buffer for CommandList::drawIndexed.
    Uniform = 1 << 2,            ///< Uniform/constant buffer.
    Storage = 1 << 3,            ///< Read/write from shaders (by device address).
    Indirect = 1 << 4,           ///< Indirect draw/dispatch arguments.
    TransferSrc = 1 << 5,        ///< Source of copies.
    TransferDst = 1 << 6,        ///< Destination of copies / uploads.
    AccelStructInput = 1 << 7,   ///< Vertex/index/instance data read by acceleration structure builds.
    AccelStructStorage = 1 << 8, ///< Backing memory of an acceleration structure.
    ShaderBindingTable = 1 << 9, ///< Ray tracing shader binding table.
};
NOVA_ENUM_FLAGS(BufferUsage)

/// @brief Where buffer memory lives.
enum class MemoryUsage {
    GpuOnly,  ///< Device-local; filled with Device::uploadBuffer or GPU copies.
    CpuToGpu, ///< Host-visible, persistently mapped, write-combined (per-frame data, staging).
    GpuToCpu, ///< Host-visible, persistently mapped, cached (readback).
};

/// @brief Buffer creation parameters.
struct BufferDesc {
    u64 size = 0;                              ///< Size in bytes (> 0).
    BufferUsage usage = BufferUsage::Storage;  ///< Allowed usages.
    MemoryUsage memory = MemoryUsage::GpuOnly; ///< Memory placement.
    const char* debugName = nullptr;           ///< Name shown in RenderDoc / Nsight / validation messages.
};

// ----------------------------------------------------------------- textures --

/// @brief How a texture may be used (bit set; combine with `|`).
enum class TextureUsage : u32 {
    None = 0,
    Sampled = 1 << 0,      ///< Sampled in shaders; gets a slot in the bindless texture array.
    Storage = 1 << 1,      ///< Read/write image in shaders; gets a bindless storage-image slot.
    RenderTarget = 1 << 2, ///< Color attachment.
    DepthStencil = 1 << 3, ///< Depth/stencil attachment.
    TransferSrc = 1 << 4,  ///< Source of copies.
    TransferDst = 1 << 5,  ///< Destination of copies / uploads.
};
NOVA_ENUM_FLAGS(TextureUsage)

/// @brief 2D texture creation parameters.
struct TextureDesc {
    u32 width = 1;                               ///< Width in pixels.
    u32 height = 1;                              ///< Height in pixels.
    u32 mipLevels = 1;                           ///< Number of mip levels.
    Format format = Format::RGBA8_UNORM;         ///< Pixel format.
    TextureUsage usage = TextureUsage::Sampled;  ///< Allowed usages.
    const char* debugName = nullptr;             ///< Name for graphics debuggers.
};

/// @brief Logical state of a texture.
///
/// The backend tracks each texture's current state and emits the required barriers/layout
/// transitions when CommandList::transition (or beginRendering) asks for a new one.
enum class ResourceState : u8 {
    Undefined,       ///< Contents may be discarded.
    RenderTarget,    ///< Color attachment.
    DepthWrite,      ///< Depth attachment, read/write.
    DepthRead,       ///< Depth, read-only (test or sampling).
    ShaderRead,      ///< Sampled in shaders.
    UnorderedAccess, ///< Storage image (shader read/write).
    CopySrc,         ///< Source of a copy.
    CopyDst,         ///< Destination of a copy.
    Present,         ///< Ready to be presented (swapchain images).
};

// ---------------------------------------------------------------- pipelines --

/// @brief One shader stage.
struct ShaderDesc {
    /// @brief SPIR-V words. Backends that do not consume SPIR-V would cross-compile it (spirv-cross / DXC).
    std::vector<u32> spirv;
    std::string entryPoint = "main"; ///< Entry point name.
    /// @brief True if bytecode was provided.
    bool valid() const { return !spirv.empty(); }
};

/// @brief Triangle face culling.
enum class CullMode : u8 { None, Front, Back };
/// @brief Rasterization fill mode (Line needs DeviceCaps::wireframe).
enum class PolygonMode : u8 { Fill, Line };
/// @brief Depth comparison function.
enum class CompareOp : u8 { Never, Less, LessEqual, Greater, GreaterEqual, Equal, Always };
/// @brief Primitive assembly.
enum class PrimitiveTopology : u8 { TriangleList, LineList, PointList };
/// @brief Color blending preset (applied to every color attachment).
enum class BlendMode : u8 {
    Opaque,   ///< No blending.
    Alpha,    ///< Straight (non-premultiplied) alpha: `src * a + dst * (1 - a)`.
    Additive, ///< `src + dst`.
};

/// @brief Graphics pipeline description.
///
/// There is no vertex input state: vertices are fetched in shaders by device address.
/// Viewport and scissor are dynamic. Front faces are counter-clockwise.
struct GraphicsPipelineDesc {
    ShaderDesc vertex;                                            ///< Vertex shader (required).
    ShaderDesc fragment;                                          ///< Fragment shader (optional, e.g. depth-only).
    std::vector<Format> colorFormats;                             ///< Formats of the color attachments it renders to.
    Format depthFormat = Format::Undefined;                       ///< Depth attachment format, or Undefined.
    PrimitiveTopology topology = PrimitiveTopology::TriangleList; ///< Primitive type.
    CullMode cullMode = CullMode::Back;                           ///< Face culling.
    PolygonMode polygonMode = PolygonMode::Fill;                  ///< Fill or wireframe.
    bool depthTest = true;                                        ///< Enable the depth test.
    bool depthWrite = true;                                       ///< Write depth.
    CompareOp depthCompare = CompareOp::Less;                     ///< Depth test function.
    BlendMode blend = BlendMode::Opaque;                          ///< Blending preset.
    const char* debugName = nullptr;                              ///< Name for graphics debuggers.
};

/// @brief Compute pipeline description.
struct ComputePipelineDesc {
    ShaderDesc compute;              ///< Compute shader.
    const char* debugName = nullptr; ///< Name for graphics debuggers.
};

// ---------------------------------------------------------------- rendering --

/// @brief What happens to an attachment's contents at the start of a pass.
enum class LoadOp : u8 { Load, Clear, DontCare };
/// @brief What happens to an attachment's contents at the end of a pass.
enum class StoreOp : u8 { Store, DontCare };

/// @brief A color attachment of a render pass.
struct ColorAttachment {
    TextureHandle texture;                       ///< Target (needs TextureUsage::RenderTarget); invalid: an unused slot, writes to it discarded (needs DeviceCaps::unusedAttachments).
    LoadOp load = LoadOp::Clear;                 ///< Load operation.
    StoreOp store = StoreOp::Store;              ///< Store operation.
    std::array<f32, 4> clear{0.0f, 0.0f, 0.0f, 1.0f}; ///< Clear color when `load == Clear`.
};

/// @brief The depth attachment of a render pass.
struct DepthAttachment {
    TextureHandle texture;          ///< Target (needs TextureUsage::DepthStencil).
    LoadOp load = LoadOp::Clear;    ///< Load operation.
    StoreOp store = StoreOp::Store; ///< Store operation.
    f32 clearDepth = 1.0f;          ///< Clear value when `load == Clear`.
};

/// @brief Parameters of CommandList::beginRendering (dynamic rendering, no render pass objects).
struct RenderingDesc {
    std::vector<ColorAttachment> colors;  ///< Color attachments (may be empty for depth-only passes).
    std::optional<DepthAttachment> depth; ///< Optional depth attachment.
    Extent2D extent{};                    ///< Render area; zero = size of the first attachment.
};

// ----------------------------------------------------------- ray tracing ----

/// @brief Bottom-level acceleration structure: one triangle mesh.
struct BLASDesc {
    BufferHandle vertexBuffer;                 ///< Buffer with vertex positions (needs BufferUsage::AccelStructInput).
    u64 vertexOffset = 0;                      ///< Byte offset of the first position.
    u32 vertexStride = 0;                      ///< Bytes between consecutive positions.
    u32 vertexCount = 0;                       ///< Number of vertices.
    Format vertexFormat = Format::RGB32_FLOAT; ///< Position format.
    BufferHandle indexBuffer;                  ///< 32-bit indices (needs BufferUsage::AccelStructInput).
    u64 indexOffset = 0;                       ///< Byte offset of the first index.
    u32 indexCount = 0;                        ///< Number of indices (3 per triangle).
    bool opaque = true;                        ///< Skip any-hit shaders.
    /// @brief Can be refitted with CommandList::updateBLAS after its vertices move (same buffers,
    ///        same triangles): it keeps its geometry and build scratch memory.
    bool allowUpdate = false;
    const char* debugName = nullptr;           ///< Name for graphics debuggers.
};

/// @brief Top-level acceleration structure: instances of BLASes, rebuilt with CommandList::buildTLAS.
struct TLASDesc {
    u32 maxInstances = 1024;         ///< Capacity; builds may use up to this many instances.
    const char* debugName = nullptr; ///< Name for graphics debuggers.
};

/// @brief Per-instance flags of an AccelStructInstance.
enum AccelInstanceFlags : u8 {
    AccelInstanceTriangleCullDisable = 1 << 0, ///< Ignore face culling for this instance.
    AccelInstanceTriangleFlipFacing = 1 << 1,  ///< Invert the front-face winding.
    AccelInstanceForceOpaque = 1 << 2,         ///< Treat all geometry as opaque.
    AccelInstanceForceNoOpaque = 1 << 3,       ///< Treat all geometry as non-opaque.
};

/// @brief Binary layout of one TLAS instance.
///
/// Identical to `VkAccelerationStructureInstanceKHR` and `D3D12_RAYTRACING_INSTANCE_DESC`, so an
/// array of these can be written straight into GPU memory and passed to CommandList::buildTLAS.
struct AccelStructInstance {
    f32 transform[3][4];          ///< Row-major 3x4 object-to-world matrix.
    u32 instanceCustomIndex : 24; ///< Value of `gl_InstanceCustomIndexEXT` / `InstanceID()`.
    u32 mask : 8;                 ///< Visibility mask tested against the ray's cull mask.
    u32 sbtRecordOffset : 24;     ///< Hit group offset (ray tracing pipelines).
    u32 flags : 8;                ///< AccelInstanceFlags.
    u64 blasAddress;              ///< Device::accelStructAddress of the instanced BLAS.
};
static_assert(sizeof(AccelStructInstance) == 64, "AccelStructInstance must be 64 bytes");

// --------------------------------------------------------------------- misc --

/// @brief Device creation parameters.
struct DeviceDesc {
    bool enableValidation = false; ///< Enable API validation layers (debug messages go to the log).
    bool vsync = true;             ///< Present synchronized to the display.
    bool enableRayTracing = true;  ///< Request ray tracing extensions when the GPU supports them.
};

/// @brief What the selected GPU and backend support.
struct DeviceCaps {
    std::string deviceName;              ///< GPU name.
    std::string apiName;                 ///< API and version, e.g. "Vulkan 1.4.315".
    bool accelerationStructures = false; ///< BLAS/TLAS available.
    bool rayQuery = false;               ///< Inline ray tracing from any shader stage (hybrid rendering).
    bool rayTracingPipeline = false;     ///< Dedicated ray tracing pipelines (raygen/miss/hit).
    bool wireframe = false;              ///< PolygonMode::Line supported.
    bool clipDistance = false;           ///< Vertex shaders can write gl_ClipDistance.
    bool drawIndirectFirstInstance = false; ///< Indirect draws may start at an instance other than 0.
    bool multiDrawIndirect = false;      ///< Indirect draws of more than one record per call.
    /// @brief A ColorAttachment may be left without a texture (writes to it discarded) while the
    ///        pipeline still has a format for that slot.
    bool unusedAttachments = false;
    u32 framesInFlight = 2;              ///< Frames the CPU may record ahead of the GPU.
    u32 maxBindlessTextures = 0;         ///< Size of the bindless sampled-texture array.
    u32 maxPushConstantSize = 128;       ///< Push constant bytes available to every stage.
};

} // namespace nova::rhi
