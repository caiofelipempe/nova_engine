#pragma once

#include "nova/core/Base.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// Graphics-API-agnostic resource descriptions.
//
// Binding model (all backends): "bindless".
//   * Buffers are accessed in shaders through 64-bit device addresses passed in push constants.
//   * Sampled textures live in one global descriptor array; Device::bindlessIndex() gives the slot.
//   * Acceleration structures are referenced by device address (rayQuery / traceRay).
// This maps 1:1 to Vulkan (BDA + descriptor indexing) and D3D12 (SM6.6 ResourceDescriptorHeap).
namespace nova::rhi {

enum class Backend { Vulkan };

// ------------------------------------------------------------------ handles --
template <typename Tag>
struct Handle {
    u32 index = 0;
    u32 generation = 0; // 0 == invalid
    bool valid() const { return generation != 0; }
    explicit operator bool() const { return valid(); }
    bool operator==(const Handle&) const = default;
    u64 key() const { return (u64(generation) << 32) | index; }
};

using BufferHandle = Handle<struct BufferTag>;
using TextureHandle = Handle<struct TextureTag>;
using PipelineHandle = Handle<struct PipelineTag>;
using AccelStructHandle = Handle<struct AccelStructTag>;

// ------------------------------------------------------------------ formats --
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
    RGB32_FLOAT, // vertex / acceleration-structure input only
    RGBA32_FLOAT,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
};

constexpr bool isDepthFormat(Format f) { return f == Format::D32_FLOAT || f == Format::D24_UNORM_S8_UINT; }

// ------------------------------------------------------------------ buffers --
enum class BufferUsage : u32 {
    None = 0,
    Vertex = 1 << 0,
    Index = 1 << 1,
    Uniform = 1 << 2,
    Storage = 1 << 3,
    Indirect = 1 << 4,
    TransferSrc = 1 << 5,
    TransferDst = 1 << 6,
    AccelStructInput = 1 << 7,   // vertex/index/instance data read by AS builds
    AccelStructStorage = 1 << 8, // backing memory of an acceleration structure
    ShaderBindingTable = 1 << 9,
};
NOVA_ENUM_FLAGS(BufferUsage)

enum class MemoryUsage { GpuOnly, CpuToGpu, GpuToCpu };

struct BufferDesc {
    u64 size = 0;
    BufferUsage usage = BufferUsage::Storage;
    MemoryUsage memory = MemoryUsage::GpuOnly;
    const char* debugName = nullptr;
};

// ----------------------------------------------------------------- textures --
enum class TextureUsage : u32 {
    None = 0,
    Sampled = 1 << 0,
    Storage = 1 << 1,
    RenderTarget = 1 << 2,
    DepthStencil = 1 << 3,
    TransferSrc = 1 << 4,
    TransferDst = 1 << 5,
};
NOVA_ENUM_FLAGS(TextureUsage)

struct TextureDesc {
    u32 width = 1;
    u32 height = 1;
    u32 mipLevels = 1;
    Format format = Format::RGBA8_UNORM;
    TextureUsage usage = TextureUsage::Sampled;
    const char* debugName = nullptr;
};

// Logical state of a texture. The backend tracks the current state and emits barriers.
enum class ResourceState : u8 {
    Undefined,
    RenderTarget,
    DepthWrite,
    DepthRead,
    ShaderRead,
    UnorderedAccess,
    CopySrc,
    CopyDst,
    Present,
};

// ---------------------------------------------------------------- pipelines --
struct ShaderDesc {
    std::vector<u32> spirv; // backends that do not consume SPIR-V cross-compile (e.g. via spirv-cross / DXC)
    std::string entryPoint = "main";
    bool valid() const { return !spirv.empty(); }
};

enum class CullMode : u8 { None, Front, Back };
enum class PolygonMode : u8 { Fill, Line };
enum class CompareOp : u8 { Never, Less, LessEqual, Greater, GreaterEqual, Equal, Always };
enum class PrimitiveTopology : u8 { TriangleList, LineList, PointList };
enum class BlendMode : u8 { Opaque, Alpha, Additive };

struct GraphicsPipelineDesc {
    ShaderDesc vertex;
    ShaderDesc fragment;
    std::vector<Format> colorFormats;
    Format depthFormat = Format::Undefined;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    CullMode cullMode = CullMode::Back;
    PolygonMode polygonMode = PolygonMode::Fill;
    bool depthTest = true;
    bool depthWrite = true;
    CompareOp depthCompare = CompareOp::Less;
    BlendMode blend = BlendMode::Opaque;
    const char* debugName = nullptr;
};

struct ComputePipelineDesc {
    ShaderDesc compute;
    const char* debugName = nullptr;
};

// ---------------------------------------------------------------- rendering --
enum class LoadOp : u8 { Load, Clear, DontCare };
enum class StoreOp : u8 { Store, DontCare };

struct ColorAttachment {
    TextureHandle texture;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    std::array<f32, 4> clear{0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthAttachment {
    TextureHandle texture;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    f32 clearDepth = 1.0f;
};

struct RenderingDesc {
    std::vector<ColorAttachment> colors;
    std::optional<DepthAttachment> depth;
    Extent2D extent{}; // 0 = derive from the first attachment
};

// ----------------------------------------------------------- ray tracing ----
struct BLASDesc {
    BufferHandle vertexBuffer;
    u64 vertexOffset = 0;
    u32 vertexStride = 0;
    u32 vertexCount = 0;
    Format vertexFormat = Format::RGB32_FLOAT;
    BufferHandle indexBuffer; // 32-bit indices
    u64 indexOffset = 0;
    u32 indexCount = 0;
    bool opaque = true;
    const char* debugName = nullptr;
};

struct TLASDesc {
    u32 maxInstances = 1024;
    const char* debugName = nullptr;
};

enum AccelInstanceFlags : u8 {
    AccelInstanceTriangleCullDisable = 1 << 0,
    AccelInstanceTriangleFlipFacing = 1 << 1,
    AccelInstanceForceOpaque = 1 << 2,
    AccelInstanceForceNoOpaque = 1 << 3,
};

// Binary layout of one TLAS instance. Identical to VkAccelerationStructureInstanceKHR and
// D3D12_RAYTRACING_INSTANCE_DESC, so it can be written straight into GPU memory.
struct AccelStructInstance {
    f32 transform[3][4];              // row-major 3x4 object-to-world
    u32 instanceCustomIndex : 24;     // gl_InstanceCustomIndexEXT / InstanceID()
    u32 mask : 8;
    u32 sbtRecordOffset : 24;         // hit group offset (ray tracing pipelines)
    u32 flags : 8;                    // AccelInstanceFlags
    u64 blasAddress;                  // Device::accelStructAddress(blas)
};
static_assert(sizeof(AccelStructInstance) == 64, "AccelStructInstance must be 64 bytes");

// --------------------------------------------------------------------- misc --
struct DeviceDesc {
    bool enableValidation = false;
    bool vsync = true;
    bool enableRayTracing = true; // request RT extensions when the GPU has them
};

struct DeviceCaps {
    std::string deviceName;
    std::string apiName;
    bool accelerationStructures = false;
    bool rayQuery = false;            // inline RT from any shader stage (hybrid rendering)
    bool rayTracingPipeline = false;  // dedicated RT pipelines (raygen/miss/hit)
    bool wireframe = false;
    u32 framesInFlight = 2;
    u32 maxBindlessTextures = 0;
    u32 maxPushConstantSize = 128;
};

} // namespace nova::rhi
