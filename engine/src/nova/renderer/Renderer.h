#pragma once

#include "nova/renderer/Mesh.h"
#include "nova/rhi/Device.h"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova {

// ------------------------------------------------------------- frame input --
struct CameraData {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f}; // Vulkan-style clip space (Y down, depth 0..1)
    glm::vec3 position{0.0f};

    static CameraData perspective(const glm::vec3& position, const glm::vec3& target, f32 fovY, f32 aspect,
                                  f32 nearClip = 0.05f, f32 farClip = 500.0f);
    static CameraData fromTransform(const glm::mat4& cameraToWorld, f32 fovY, f32 aspect, f32 nearClip,
                                    f32 farClip);
};

struct DirectionalLightData {
    glm::vec3 direction{-0.35f, -1.0f, -0.45f}; // direction the light travels
    glm::vec3 color{1.0f, 0.96f, 0.9f};
    f32 intensity = 3.0f;
};

struct PointLightData {
    glm::vec3 position{0.0f};
    f32 radius = 8.0f;
    glm::vec3 color{1.0f};
    f32 intensity = 10.0f;
};

struct DrawItem {
    MeshHandle mesh;
    glm::mat4 transform{1.0f};
    Material material;
    bool castShadows = true;
};

// Everything needed to draw one frame of a scene. Built by Scene::buildRenderScene()
// or by hand if you don't use the ECS.
struct RenderScene {
    CameraData camera;
    DirectionalLightData sun;
    std::vector<PointLightData> pointLights;
    std::vector<DrawItem> draws;
    f32 time = 0.0f;

    void clear() {
        pointLights.clear();
        draws.clear();
    }
};

// ---------------------------------------------------------------- settings --
enum class DebugView : u32 { Lit = 0, Normals, Shadows, Albedo };
enum class Tonemapper : u32 { ACES = 0, Reinhard, None };

struct RenderSettings {
    bool rayTracedShadows = true; // used when the GPU supports ray queries
    bool wireframe = false;
    DebugView debugView = DebugView::Lit;
    Tonemapper tonemapper = Tonemapper::ACES;
    f32 exposure = 1.0f;
    f32 shadowBias = 0.02f;
    f32 ambientIntensity = 0.3f;
    glm::vec3 skyZenith{0.22f, 0.42f, 0.85f};
    glm::vec3 skyHorizon{0.78f, 0.84f, 0.92f};
    glm::vec3 groundColor{0.28f, 0.25f, 0.22f};
};

struct RenderStats {
    u32 drawCalls = 0;
    u32 instances = 0;
    u64 triangles = 0;
    u32 tlasInstances = 0;
};

class Renderer;

// Offscreen render targets for one view (editor viewport, minimap, ...).
class RenderView : NonCopyable {
public:
    ~RenderView();
    void resize(Extent2D size);
    Extent2D size() const { return m_size; }
    // Tonemapped result (sampled-ready after Renderer::render). Use with ImGui::Image via textureId().
    rhi::TextureHandle output() const { return m_output; }

private:
    friend class Renderer;
    RenderView(Renderer& renderer, Extent2D size, bool withOutput);
    void create();
    void destroy();

    Renderer& m_renderer;
    Extent2D m_size;
    bool m_withOutput;
    rhi::TextureHandle m_hdr;
    rhi::TextureHandle m_depth;
    rhi::TextureHandle m_output;
};

// Hybrid forward renderer: raster G-buffer-free forward shading + ray-queried shadows.
//
// Frame graph:  [TLAS build] -> sky -> opaque forward (HDR) -> tonemap (LDR output)
class Renderer : NonCopyable {
public:
    explicit Renderer(rhi::Device& device);
    ~Renderer();

    // -- meshes
    MeshHandle createMesh(const MeshData& data, const std::string& name = "Mesh");
    void destroyMesh(MeshHandle mesh);
    MeshHandle cube() const { return m_cube; }
    MeshHandle sphere() const { return m_sphere; }
    MeshHandle plane() const { return m_plane; }
    const std::string& meshName(MeshHandle mesh) const;
    std::vector<MeshHandle> meshes() const;

    // -- textures (RGBA8, sRGB)
    rhi::TextureHandle createTexture2D(u32 width, u32 height, const void* rgba8, const char* name = nullptr);

    // -- views
    Scope<RenderView> createView(Extent2D size);

    // -- frame
    void beginFrame(); // called by Application each frame
    // Renders into the view; its output ends in ShaderRead state.
    void render(rhi::CommandList& cmd, const RenderScene& scene, RenderView& view);
    // Renders straight to the swapchain image (games without an editor).
    void renderToBackbuffer(rhi::CommandList& cmd, const RenderScene& scene);

    RenderSettings& settings() { return m_settings; }
    const RenderStats& stats() const { return m_lastStats; } // previous completed frame
    bool rayTracingAvailable() const { return m_device.caps().rayQuery; }
    rhi::Device& device() { return m_device; }

    static constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16_FLOAT;
    static constexpr rhi::Format kDepthFormat = rhi::Format::D32_FLOAT;

private:
    struct GpuMesh {
        std::string name;
        rhi::BufferHandle vertexBuffer;
        rhi::BufferHandle indexBuffer;
        rhi::AccelStructHandle blas;
        u32 vertexCount = 0;
        u32 indexCount = 0;
        bool alive = false;
    };

    struct FrameAllocation {
        void* cpu = nullptr;
        u64 gpu = 0;
    };

    void createPipelines();
    rhi::PipelineHandle tonemapPipeline(rhi::Format format);
    FrameAllocation allocate(u64 size, u64 alignment = 256);
    void renderInternal(rhi::CommandList& cmd, const RenderScene& scene, rhi::TextureHandle hdr,
                        rhi::TextureHandle depth, rhi::TextureHandle output);

    rhi::Device& m_device;
    RenderSettings m_settings;
    RenderStats m_stats;
    RenderStats m_lastStats;

    std::vector<GpuMesh> m_meshes;
    std::vector<u32> m_freeMeshSlots;
    MeshHandle m_cube, m_sphere, m_plane;
    std::vector<rhi::TextureHandle> m_textures;

    rhi::PipelineHandle m_skyPipeline;
    rhi::PipelineHandle m_forwardPipeline;
    rhi::PipelineHandle m_forwardRtPipeline;
    rhi::PipelineHandle m_wireframePipeline;
    std::unordered_map<rhi::Format, rhi::PipelineHandle> m_tonemapPipelines;
    rhi::ShaderDesc m_fullscreenVS;
    rhi::ShaderDesc m_tonemapFS;

    static constexpr u64 kFrameBufferSize = 16ull << 20;
    static constexpr u32 kMaxInstances = 16384;
    std::vector<rhi::BufferHandle> m_frameBuffers; // one linear upload buffer per frame in flight
    u64 m_frameOffset = 0;
    std::vector<rhi::AccelStructHandle> m_tlas;   // one per frame in flight

    Scope<RenderView> m_backbufferView;
};

} // namespace nova
