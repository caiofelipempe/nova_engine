#pragma once

/// @file
/// @brief High-level renderer: frame inputs, settings, render views and the Renderer itself.

#include "nova/renderer/DebugDraw.h"
#include "nova/renderer/Mesh.h"
#include "nova/renderer/ShaderLibrary.h"
#include "nova/rhi/Device.h"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova {

// ------------------------------------------------------------- frame input --

/// @brief View and projection of the camera a frame is rendered from.
struct CameraData {
    glm::mat4 view{1.0f};       ///< World → view.
    glm::mat4 projection{1.0f}; ///< View → clip, Vulkan convention (Y down, depth 0..1).
    glm::vec3 position{0.0f};   ///< Camera position in world space.

    /// @brief Perspective camera looking from `position` at `target` (Y up).
    /// @param fovY Vertical field of view in radians.
    /// @param aspect Width / height of the target.
    /// @param nearClip Near plane distance.
    /// @param farClip Far plane distance.
    static CameraData perspective(const glm::vec3& position, const glm::vec3& target, f32 fovY, f32 aspect,
                                  f32 nearClip = 0.05f, f32 farClip = 500.0f);
    /// @brief Perspective camera from a camera-to-world matrix (camera looks down its local -Z).
    /// @param cameraToWorld Rigid transform of the camera (no scale).
    /// @param fovY Vertical field of view in radians.
    /// @param aspect Width / height of the target.
    /// @param nearClip Near plane distance.
    /// @param farClip Far plane distance.
    static CameraData fromTransform(const glm::mat4& cameraToWorld, f32 fovY, f32 aspect, f32 nearClip,
                                    f32 farClip);
};

/// @brief The scene's single directional light ("sun").
struct DirectionalLightData {
    glm::vec3 direction{-0.35f, -1.0f, -0.45f}; ///< Direction the light travels (need not be normalized).
    glm::vec3 color{1.0f, 0.96f, 0.9f};         ///< Linear RGB color.
    f32 intensity = 3.0f;                       ///< Radiance multiplier.
};

/// @brief A point light with a finite radius (smooth windowed inverse-square falloff).
struct PointLightData {
    glm::vec3 position{0.0f}; ///< World position.
    f32 radius = 8.0f;        ///< Distance at which the light reaches zero.
    glm::vec3 color{1.0f};    ///< Linear RGB color.
    f32 intensity = 10.0f;    ///< Radiance multiplier.
};

/// @brief One mesh instance to draw.
struct DrawItem {
    MeshHandle mesh;           ///< Mesh created with Renderer::createMesh.
    glm::mat4 transform{1.0f}; ///< Object → world.
    Material material;         ///< Surface parameters.
    bool castShadows = true;   ///< Include in the ray tracing scene (ray-traced shadows).
};

/// @brief Everything needed to draw one frame of a scene.
///
/// Built by Scene::buildRenderScene(), or filled by hand when not using the ECS.
/// Reuse the same object every frame to avoid reallocations (see clear()).
struct RenderScene {
    CameraData camera;                       ///< Point of view.
    DirectionalLightData sun;                ///< Directional light.
    std::vector<PointLightData> pointLights; ///< Point lights (the first 16 are used).
    std::vector<DrawItem> draws;             ///< Mesh instances.
    f32 time = 0.0f;                         ///< Scene time in seconds within a 1-hour cycle (shaders: `nova_time()`).

    /// @brief Empties the lists, keeping their capacity.
    void clear() {
        pointLights.clear();
        draws.clear();
    }
};

// ---------------------------------------------------------------- settings --

/// @brief Debug visualization of the forward pass.
enum class DebugView : u32 {
    Lit = 0, ///< Normal shading.
    Normals, ///< World-space normals as colors.
    Shadows, ///< Sun visibility (white = lit).
    Albedo,  ///< Base color only.
};

/// @brief HDR → display mapping.
enum class Tonemapper : u32 {
    ACES = 0, ///< ACES filmic (Narkowicz fit).
    Reinhard, ///< `c / (1 + c)`.
    None,     ///< Clamp only.
};

/// @brief A fullscreen pass over the HDR image, written against `nova/postprocess.glsl`.
///
/// @code
/// auto vignette = renderer().shaders().load("assets/shaders/vignette.frag");
/// renderer().settings().postProcess.push_back({.name = "Vignette", .shader = vignette, .params = {0.6f, 0, 0, 0}});
/// @endcode
struct PostProcessDesc {
    std::string name;        ///< Label (editor, debuggers).
    ShaderHandle shader;     ///< Fragment shader (passes whose shader is not ready are skipped).
    glm::vec4 params{0.0f};  ///< `nova_params()` in GLSL.
    bool enabled = true;     ///< Skip the pass when false.
};

/// @brief Runtime-tweakable rendering options (Renderer::settings()).
struct RenderSettings {
    bool rayTracedShadows = true;                ///< Ray-query shadows; ignored when the GPU lacks ray queries.
    bool wireframe = false;                      ///< Draw meshes as lines (needs DeviceCaps::wireframe).
    DebugView debugView = DebugView::Lit;        ///< Debug visualization.
    Tonemapper tonemapper = Tonemapper::ACES;    ///< Tonemapping operator.
    f32 exposure = 1.0f;                         ///< Linear exposure multiplier applied before tonemapping.
    f32 shadowBias = 0.02f;                      ///< Shadow ray origin offset along the normal (world units).
    f32 ambientIntensity = 0.3f;                 ///< Strength of the hemisphere ambient light.
    glm::vec3 skyZenith{0.22f, 0.42f, 0.85f};    ///< Sky color straight up (also the ambient's upper color).
    glm::vec3 skyHorizon{0.78f, 0.84f, 0.92f};   ///< Sky color at the horizon.
    glm::vec3 groundColor{0.28f, 0.25f, 0.22f};  ///< Color below the horizon (also the ambient's lower color).
    /// @brief Post-process passes, run in order on the HDR image before exposure and tonemapping.
    std::vector<PostProcessDesc> postProcess;
};

/// @brief Counters of the last completed frame.
struct RenderStats {
    u32 drawCalls = 0;     ///< Draw calls issued.
    u32 instances = 0;     ///< Mesh instances submitted.
    u64 triangles = 0;     ///< Triangles drawn.
    u32 tlasInstances = 0; ///< Instances in the ray tracing scene.
    u32 postProcessPasses = 0; ///< Post-process passes run.
};

class Renderer;

/// @brief Offscreen render targets for one view (editor viewport, minimap, ...).
///
/// Holds an HDR color target, a depth buffer and a tonemapped output in the swapchain format.
/// Created with Renderer::createView(); must be destroyed before the Renderer.
class RenderView : NonCopyable {
public:
    ~RenderView();
    /// @brief Recreates the targets if `size` differs (clamped to at least 1×1).
    /// @note Call outside of rendering (e.g. in Layer::onUpdate); old targets are released safely.
    void resize(Extent2D size);
    /// @brief Current size in pixels.
    Extent2D size() const { return m_size; }
    /// @brief Tonemapped result, in ResourceState::ShaderRead after Renderer::render.
    ///
    /// Display it with `ImGui::Image((ImTextureID)app.imgui()->textureId(view.output()), size)`.
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
    rhi::TextureHandle m_hdrPing; // second HDR target for post-process ping-pong
    rhi::TextureHandle m_depth;
    rhi::TextureHandle m_output;
};

/// @brief Hybrid forward renderer: rasterized PBR shading with ray-queried shadows.
///
/// Frame graph: `[TLAS build] → sky → opaque forward (HDR) → post-process passes (HDR) → tonemap (LDR output)`.
///
/// Game shaders plug in without engine changes: Material::shader selects a material fragment
/// shader (`nova/material.glsl`), RenderSettings::postProcess adds fullscreen passes
/// (`nova/postprocess.glsl`) and computePipeline() builds compute pipelines. Shaders come from
/// shaders() and are hot reloaded; pipelines are rebuilt when their shader changes.
///
/// - Meshes are stored once on the GPU (vertex pulling via device addresses) and, when ray
///   tracing is available, each gets a BLAS.
/// - Per-frame data (camera, lights, instances, TLAS instances) is written into a linear upload
///   buffer per frame in flight; a TLAS per frame in flight is rebuilt from the draw list.
/// - Without ray query support the same scene renders unshadowed, with no code changes.
class Renderer : NonCopyable {
public:
    /// @brief Loads shaders from `<exe dir>/shaders`, creates pipelines, per-frame buffers and the built-in meshes.
    explicit Renderer(rhi::Device& device);
    /// @brief Waits for the GPU and destroys every mesh, texture and pipeline it owns.
    ~Renderer();

    /// @name Meshes
    /// @{
    /// @brief Uploads a mesh (and builds its BLAS when ray tracing is available).
    /// @param data Geometry; must not be empty.
    /// @param name Display/debug name.
    /// @return Handle to use in DrawItem / MeshComponent.
    MeshHandle createMesh(const MeshData& data, const std::string& name = "Mesh");
    /// @brief Releases a mesh. Invalid handles are ignored.
    void destroyMesh(MeshHandle mesh);
    /// @brief Built-in unit cube (side 1, centered).
    MeshHandle cube() const { return m_cube; }
    /// @brief Built-in sphere (diameter 1).
    MeshHandle sphere() const { return m_sphere; }
    /// @brief Built-in 1×1 plane on XZ facing +Y.
    MeshHandle plane() const { return m_plane; }
    /// @brief Name given at creation, or "<none>" for invalid handles.
    const std::string& meshName(MeshHandle mesh) const;
    /// @brief First live mesh with this name (e.g. "Cube"), or an invalid handle.
    MeshHandle findMesh(const std::string& name) const;
    /// @brief CPU copy of a mesh's geometry (for picking/placement), or `nullptr`.
    const MeshData* meshData(MeshHandle mesh) const;
    /// @brief Object-space bounding box of a mesh (zero box for invalid handles).
    Bounds meshBounds(MeshHandle mesh) const;
    /// @brief Every live mesh.
    std::vector<MeshHandle> meshes() const;
    /// @}

    /// @brief Creates a sampled sRGB texture from RGBA8 pixels; owned by the renderer.
    /// @param width Width in pixels.
    /// @param height Height in pixels.
    /// @param rgba8 `width * height * 4` bytes, tightly packed.
    /// @param name Debug name.
    rhi::TextureHandle createTexture2D(u32 width, u32 height, const void* rgba8, const char* name = nullptr);

    /// @name Shaders
    /// @{
    /// @brief Game shaders: runtime compiled or precompiled, hot reloaded by beginFrame().
    ShaderLibrary& shaders() { return m_shaders; }
    /// @brief Compute pipeline for a compute shader, cached and rebuilt after the shader reloads.
    /// @return Invalid handle while the shader is not ready (e.g. its first compile failed).
    /// @note Handles change after a reload: fetch it every time you dispatch.
    rhi::PipelineHandle computePipeline(ShaderHandle shader);
    /// @brief True if `shader` currently has a working material pipeline (false: drawn with the default).
    bool materialReady(ShaderHandle shader) const { return m_shaders.ready(shader); }
    /// @}

    /// @brief Creates offscreen targets to render a scene into.
    Scope<RenderView> createView(Extent2D size);

    /// @name Frame
    /// @{
    /// @brief Resets per-frame allocations, statistics and debug lines. Called by Application at the
    ///        start of each frame, before layers update.
    void beginFrame();
    /// @brief Renders `scene` into `view`; RenderView::output() ends in ResourceState::ShaderRead.
    void render(rhi::CommandList& cmd, const RenderScene& scene, RenderView& view);
    /// @brief Renders `scene` straight into the swapchain image (games without an editor).
    void renderToBackbuffer(rhi::CommandList& cmd, const RenderScene& scene);
    /// @}

    /// @brief Debug lines for this frame (drawn by every render call, cleared by beginFrame()).
    DebugDraw& debugDraw() { return m_debugDraw; }

    /// @brief Mutable rendering options, read at render time.
    RenderSettings& settings() { return m_settings; }
    /// @brief Statistics of the previous completed frame (UI code runs before this frame renders).
    const RenderStats& stats() const { return m_lastStats; }
    /// @brief True if ray-traced shadows can be used on this GPU.
    bool rayTracingAvailable() const { return m_device.caps().rayQuery; }
    /// @brief The device the renderer draws with.
    rhi::Device& device() { return m_device; }

    static constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16_FLOAT; ///< Format of the HDR color target.
    static constexpr rhi::Format kDepthFormat = rhi::Format::D32_FLOAT;  ///< Format of the depth target.

private:
    struct GpuMesh {
        std::string name;
        rhi::BufferHandle vertexBuffer;
        rhi::BufferHandle indexBuffer;
        rhi::AccelStructHandle blas;
        u32 vertexCount = 0;
        u32 indexCount = 0;
        bool alive = false;
        MeshData cpu;  // kept for picking
        Bounds bounds;
    };

    struct VersionedPipeline {
        u32 version = 0;
        rhi::PipelineHandle pipeline;
    };
    struct MaterialPipelines {
        VersionedPipeline fill;   // raster-only variant
        VersionedPipeline rq;     // ray query shadows variant
        ShaderHandle rqShader;
        bool rqRequested = false;
    };

    struct FrameAllocation {
        void* cpu = nullptr;
        u64 gpu = 0;
    };

    void createPipelines();
    rhi::PipelineHandle tonemapPipeline(rhi::Format format);
    rhi::PipelineHandle linePipeline(rhi::Format format, bool onTop);
    void drawDebugLines(rhi::CommandList& cmd, u64 frameAddress, rhi::TextureHandle depth, rhi::TextureHandle output);
    FrameAllocation allocate(u64 size, u64 alignment = 256);
    void renderInternal(rhi::CommandList& cmd, const RenderScene& scene, RenderView& view, rhi::TextureHandle output);
    rhi::PipelineHandle materialPipeline(ShaderHandle shader, bool rayQuery);
    rhi::PipelineHandle postProcessPipeline(ShaderHandle shader);
    bool rebuild(VersionedPipeline& vp, ShaderHandle shader, const rhi::GraphicsPipelineDesc* graphics);

    rhi::Device& m_device;
    ShaderLibrary m_shaders;
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
    rhi::ShaderDesc m_lineVS;
    rhi::ShaderDesc m_lineFS;
    std::unordered_map<u32, rhi::PipelineHandle> m_linePipelines; // key: format | onTop << 8
    rhi::GraphicsPipelineDesc m_forwardDesc;                      // template of material pipelines
    std::unordered_map<u32, MaterialPipelines> m_materialPipelines; // key: ShaderHandle::id
    std::unordered_map<u32, VersionedPipeline> m_postPipelines;     // key: ShaderHandle::id
    std::unordered_map<u32, VersionedPipeline> m_computePipelines;  // key: ShaderHandle::id
    std::vector<std::pair<u64, u32>> m_drawOrder;                   // (pipeline key, draw index), reused
    DebugDraw m_debugDraw;

    static constexpr u64 kFrameBufferSize = 16ull << 20;
    static constexpr u32 kMaxInstances = 16384;
    std::vector<rhi::BufferHandle> m_frameBuffers; // one linear upload buffer per frame in flight
    u64 m_frameOffset = 0;
    std::vector<rhi::AccelStructHandle> m_tlas;   // one per frame in flight

    Scope<RenderView> m_backbufferView;
};

} // namespace nova
