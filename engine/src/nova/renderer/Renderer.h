#pragma once

/// @file
/// @brief High-level renderer: frame inputs, settings, render views and the Renderer itself.

#include "nova/renderer/DebugDraw.h"
#include "nova/renderer/Mesh.h"
#include "nova/renderer/RenderGraph.h"
#include "nova/renderer/ShaderLibrary.h"
#include "nova/rhi/Device.h"

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova {

// ------------------------------------------------------------- frame input --

/// @brief View and projection of the camera a frame is rendered from.
///
/// Everything the GPU sees is measured from `origin` (camera-relative rendering): with
/// relative() / relativeLookAt() the origin is the eye itself, so view, position and every
/// transform of the RenderScene stay small floats however far from the world origin the camera is.
struct CameraData {
    glm::mat4 view{1.0f};       ///< (World − origin) → view.
    glm::mat4 projection{1.0f}; ///< View → clip, Vulkan convention (Y down, depth 0..1).
    glm::vec3 position{0.0f};   ///< Camera position relative to `origin` (zero for relative cameras).
    GlobalPosition origin;      ///< Point every position of the frame is measured from.

    /// @brief Camera at `eye` (which becomes the origin) with the given orientation (looks down −Z).
    /// @param eye Exact world position of the camera.
    /// @param orientation World rotation of the camera.
    /// @param fovY Vertical field of view in radians.
    /// @param aspect Width / height of the target.
    /// @param nearClip Near plane distance.
    /// @param farClip Far plane distance.
    static CameraData relative(const GlobalPosition& eye, const glm::quat& orientation, f32 fovY, f32 aspect,
                               f32 nearClip, f32 farClip);
    /// @brief Camera at `eye` (which becomes the origin) looking along `direction` (Y up).
    /// @param eye Exact world position of the camera.
    /// @param direction View direction (need not be normalized).
    /// @param fovY Vertical field of view in radians.
    /// @param aspect Width / height of the target.
    /// @param nearClip Near plane distance.
    /// @param farClip Far plane distance.
    static CameraData relativeLookAt(const GlobalPosition& eye, const glm::vec3& direction, f32 fovY, f32 aspect,
                                     f32 nearClip, f32 farClip);

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
    glm::vec3 position{0.0f}; ///< Position measured from CameraData::origin.
    f32 radius = 8.0f;        ///< Distance at which the light reaches zero.
    glm::vec3 color{1.0f};    ///< Linear RGB color.
    f32 intensity = 10.0f;    ///< Radiance multiplier.
    bool castShadows = true;  ///< Things block it (ray traced, or cube shadow maps without ray tracing).
};

/// @brief One mesh instance to draw.
struct DrawItem {
    MeshHandle mesh;           ///< Mesh created with Renderer::createMesh.
    glm::mat4 transform{1.0f}; ///< Object → world, translation measured from CameraData::origin.
    Material material;         ///< Surface parameters.
    bool castShadows = true;   ///< Include in the ray tracing scene (ray-traced shadows).
    /// @brief Skinned meshes: this draw's joint matrices in RenderScene::joints (count 0: drawn
    ///        rigid, in the bind pose). Each maps the bind pose to the pose, in mesh space.
    u32 jointOffset = 0, jointCount = 0;
    /// @brief Skinned draws: a key that stays the same from frame to frame for the same object
    ///        (Scene uses the entity's UUID). With it, the deformed shape casts ray-traced shadows
    ///        (a BLAS of its own, refitted every frame); 0: none (the draw casts no ray-traced shadow).
    u64 skinKey = 0;
};

/// @brief Everything needed to draw one frame of a scene.
///
/// Built by Scene::buildRenderScene(), or filled by hand when not using the ECS. Positions and
/// transforms are measured from `camera.origin` (the camera itself for scenes built from the ECS).
/// Reuse the same object every frame to avoid reallocations (see clear()).
struct RenderScene {
    CameraData camera;                       ///< Point of view.
    DirectionalLightData sun;                ///< Directional light.
    std::vector<PointLightData> pointLights; ///< Point lights (the first 16 are used).
    std::vector<DrawItem> draws;             ///< Mesh instances.
    std::vector<glm::mat4> joints;           ///< Joint matrices of skinned draws (DrawItem::jointOffset).
    f32 time = 0.0f;                         ///< Scene time in seconds within a 1-hour cycle (shaders: `nova_time()`).
    /// @brief Size of the circular world per axis (m), 0 = not circular. Scene::buildRenderScene sets
    ///        it; it matters to the NOVA_WRAP_RENDER_MODES options (RenderSettings::wrapShift).
    glm::vec3 wrapSize{0.0f};
    /// @brief Where the world's center (0, 0, 0) is, from the camera, the shortest way (WrapCrop::World).
    glm::vec3 worldCenter{0.0f};

    /// @brief Empties the lists, keeping their capacity.
    void clear() {
        pointLights.clear();
        draws.clear();
        joints.clear();
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

/// @brief How an object across a seam of the circular world is drawn (RenderSettings::wrapShift,
///        only in builds with the CMake option NOVA_WRAP_RENDER_MODES). Every object is first placed
///        at its copy nearest the camera, by its origin.
enum class WrapShift : u32 {
    Triangle = 0, ///< Each triangle goes to the lap its centroid is in: the object shows on both sides of the seam (the cut follows its edges).
    Object,       ///< The whole object stays at the copy nearest the camera (its origin decides).
    Split,        ///< Drawn twice (or more, at corners), each copy clipped exactly at the seam (needs DeviceCaps::clipDistance).
    Crop,         ///< Drawn once, clipped at the edges of one lap; what is beyond is not drawn (RenderSettings::wrapCrop; needs DeviceCaps::clipDistance).
};

/// @brief Which lap WrapShift::Crop keeps.
enum class WrapCrop : u32 {
    World,  ///< The world's own lap, fixed (from −L/2 to +L/2 on each circular axis): a diorama, cut at the seams.
    Camera, ///< The lap around the camera: the cut is half a lap from it and moves with it.
};

/// @brief Runtime-tweakable rendering options (Renderer::settings()).
struct RenderSettings {
    bool rayTracedShadows = true;                ///< Ray-query shadows; ignored when the GPU lacks ray queries.
    /// @brief Without ray-traced shadows (off, or no ray queries), the sun casts shadows from
    ///        cascaded shadow maps (4 cascades up to shadowDistance; point lights cast none).
    bool shadowMaps = true;
    f32 shadowDistance = 120.0f;                 ///< How far from the camera shadow maps reach (m).
    u32 shadowMapSize = 2048;                    ///< Texels per side of each cascade.
    /// @brief Without ray-traced shadows, the point lights nearest the camera cast shadows from cube
    ///        shadow maps (six faces each, drawing what is within the light's radius).
    bool pointShadowMaps = true;
    u32 maxPointShadows = 4;                     ///< Point lights with shadow maps (the nearest; at most 16).
    u32 pointShadowMapSize = 512;                ///< Texels per side of each cube face (64..1024).
    /// @brief Frustum culling on the GPU: plain opaque draws (not skinned, not split per triangle)
    ///        are tested by a compute shader and drawn in groups of the same pipeline and mesh, one
    ///        indirect draw each (needs DeviceCaps::drawIndirectFirstInstance; otherwise one draw each,
    ///        not culled).
    bool gpuCulling = true;
    /// @brief With gpuCulling, also skip what other geometry hides: tested against a depth pyramid
    ///        (Hi-Z) in two phases, last frame's then this frame's, so nothing pops in.
    bool occlusionCulling = true;
    bool wireframe = false;                      ///< Draw meshes as lines (needs DeviceCaps::wireframe).
    DebugView debugView = DebugView::Lit;        ///< Debug visualization.
    Tonemapper tonemapper = Tonemapper::ACES;    ///< Tonemapping operator.
    f32 exposure = 1.0f;                         ///< Linear exposure multiplier applied before tonemapping.
    /// @brief Bloom: light bleeding around bright things, from a chain of ever smaller blurred
    ///        copies of the HDR image, mixed in before the tonemap.
    bool bloom = true;
    f32 bloomStrength = 0.04f;   ///< How much of the blurred image is mixed in (0..1).
    f32 bloomThreshold = 0.0f;   ///< Brightness where blooming starts (0: everything blooms, physically).
    f32 bloomRadius = 1.0f;      ///< Size of the blur at each level (texels).
    u32 bloomLevels = 6;         ///< Levels of the chain (each half the size of the previous).
    f32 shadowBias = 0.02f;                      ///< Shadow ray origin offset along the normal (world units).
    f32 ambientIntensity = 0.3f;                 ///< Strength of the hemisphere ambient light.
    glm::vec3 skyZenith{0.22f, 0.42f, 0.85f};    ///< Sky color straight up (also the ambient's upper color).
    glm::vec3 skyHorizon{0.78f, 0.84f, 0.92f};   ///< Sky color at the horizon.
    glm::vec3 groundColor{0.28f, 0.25f, 0.22f};  ///< Color below the horizon (also the ambient's lower color).
    /// @brief Post-process passes, run in order on the HDR image before exposure and tonemapping.
    std::vector<PostProcessDesc> postProcess;
#if NOVA_WRAP_RENDER_MODES
    /// @brief How objects across a seam of the circular world are drawn.
    WrapShift wrapShift = WrapShift::Triangle;
    /// @brief Laps of the world drawn on each side of the camera's, on the circular axes smaller
    ///        than the view (0 = only the camera's lap). Each lap is a copy of every object (frustum
    ///        culled): small worlds repeat up to the horizon.
    u32 wrapRepeat = 0;
    /// @brief The axes wrapRepeat repeats on (X and Z by default: a world circular on Y too is not
    ///        stacked floor over floor, which would also shade the floor with the one above).
    glm::bvec3 wrapRepeatAxes{true, false, true};
    /// @brief WrapShift::Crop: the lap it keeps (no further laps are drawn when cropping).
    WrapCrop wrapCrop = WrapCrop::World;
#endif
};

struct WrapInstance; // WrapInstances.h
namespace gpu {
struct FrameData; // GpuTypes.h
}

/// @brief Counters of the last completed frame.
struct RenderStats {
    u32 drawCalls = 0;     ///< Draw calls issued.
    u32 instances = 0;     ///< Mesh instances submitted.
    u64 triangles = 0;     ///< Triangles drawn.
    u32 tlasInstances = 0; ///< Instances in the ray tracing scene.
    u32 wrapCopies = 0;    ///< Extra instances drawn for the circular world (seams, further laps).
    u32 postProcessPasses = 0; ///< Post-process passes run.
    u32 graphPasses = 0;       ///< Render graph passes run (all views).
    u32 graphCulled = 0;       ///< Render graph passes dropped: nothing needed them.
    u32 transientTextures = 0; ///< Transient textures of the graphs (shadow atlas, post-process images...).
    u32 pooledTextures = 0;    ///< Textures held behind them (transients that do not overlap share one).
    u32 cullGroups = 0;        ///< Indirect draws of GPU-culled groups (pipeline and mesh).
    u32 pointShadowMaps = 0;   ///< Point lights drawn into shadow maps.
    /// @brief Instances tested and found visible by GPU culling, as counted by the GPU a few frames
    ///        ago (read back without waiting).
    u32 gpuTested = 0, gpuVisible = 0;
    u32 gpuOccluded = 0; ///< Of the tested, hidden by other geometry (occlusion culling).
};

/// @brief Where in the frame Renderer::addRenderPasses adds passes.
enum class RenderStage : u32 {
    AfterOpaque,   ///< After the forward pass (sky, opaque and see-through), before post-processing (HDR).
    BeforeTonemap, ///< After RenderSettings::postProcess, before the tonemap (HDR).
    AfterTonemap,  ///< After the tonemap, on the output (display format): overlays. Draw with LoadOp::Load.
};

/// @brief What a render pass callback gets: the frame's graph and its textures.
struct RenderGraphFrame {
    RenderGraph& graph;       ///< Add passes and transient textures to it.
    const RenderScene& scene; ///< The scene being drawn.
    Extent2D extent;          ///< Size of the view.
    /// @brief The image so far: HDR before the tonemap, the output after. Set it to a texture your
    ///        passes wrote to hand that one on (to the next passes and the tonemap).
    RGTexture color;
    RGTexture depth;          ///< The forward pass's depth (Renderer::kDepthFormat).
    u64 frameData = 0;        ///< Device address of the frame's FrameData (`nova/common.glsl`).
};

/// @brief Adds passes to each frame's graph (Renderer::addRenderPasses).
using RenderGraphCallback = std::function<void(RenderGraphFrame&)>;

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
    rhi::TextureHandle m_depth;
    rhi::TextureHandle m_output;
    // Occlusion culling: the view's depth pyramid (float levels one after another) and the view it was made from.
    rhi::BufferHandle m_hiz;
    Extent2D m_hizSize{};
    u32 m_hizLevelCount = 0;
    std::array<glm::uvec4, 16> m_hizLevels{};
    bool m_hizValid = false;
    glm::mat4 m_prevViewProj{1.0f};
    GlobalPosition m_prevOrigin;
};

/// @brief Hybrid forward renderer: rasterized PBR shading with ray-queried shadows.
///
/// Each frame is a RenderGraph: `[skinned shapes] → [TLAS build] → [shadow maps] → forward (sky,
/// opaque, see-through; HDR) → post-process passes (HDR) → tonemap (output) → [debug lines]`, with
/// the game's passes (addRenderPasses) at three stages. The graph moves textures between states,
/// drops passes nothing needs and makes the transient textures (shadow atlas, post-process images).
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
    /// @brief Same, choosing the color space: sRGB for colors (base color, emissive), linear for
    ///        data (normal, metallic-roughness and occlusion maps).
    rhi::TextureHandle createTexture2D(u32 width, u32 height, const void* rgba8, const char* name, bool srgb);

    /// @name Shaders
    /// @{
    /// @brief Game shaders: runtime compiled or precompiled, hot reloaded by beginFrame().
    ShaderLibrary& shaders() { return m_shaders; }
    /// @brief Compute pipeline for a compute shader, cached and rebuilt after the shader reloads.
    /// @return Invalid handle while the shader is not ready (e.g. its first compile failed).
    /// @note Handles change after a reload: fetch it every time you dispatch.
    rhi::PipelineHandle computePipeline(ShaderHandle shader);
    /// @brief Fullscreen pipeline (HDR target) for a `nova/postprocess.glsl` fragment shader, cached
    ///        and rebuilt after the shader reloads: for passes added with addRenderPasses().
    /// @return Invalid handle while the shader is not ready. Fetch it every frame.
    rhi::PipelineHandle postProcessPipeline(ShaderHandle shader);
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

    /// @name Render graph
    /// @{
    /// @brief Adds passes to every frame's graph at `stage`: `callback` declares them (and any
    ///        transient textures) each frame. Passes whose results nothing uses are dropped.
    /// @return An id for removeRenderPasses().
    u32 addRenderPasses(RenderStage stage, RenderGraphCallback callback);
    /// @brief Stops adding the passes of addRenderPasses(id).
    void removeRenderPasses(u32 id);
    /// @brief The last frame's graph, one line per pass (RenderGraph::describe()).
    const std::string& lastGraph() const { return m_lastGraph; }
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
    /// @brief Origin of the last frame rendered (DebugDraw records lines relative to it).
    const GlobalPosition& lastOrigin() const { return m_lastOrigin; }

    static constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16_FLOAT; ///< Format of the HDR color target.
    static constexpr rhi::Format kDepthFormat = rhi::Format::D32_FLOAT;  ///< Format of the depth target.

private:
    struct GpuMesh {
        std::string name;
        rhi::BufferHandle vertexBuffer;
        rhi::BufferHandle indexBuffer;
        rhi::BufferHandle skinBuffer; // skinned meshes only
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
    // Pipeline states of a material (index into the variants): 1 = double sided (no culling),
    // 2 = see-through (alpha blending, no depth writes).
    static u32 pipelineState(const Material& m) {
        return (m.doubleSided ? 1u : 0u) | (m.alphaMode == AlphaMode::Blend ? 2u : 0u);
    }
    static rhi::GraphicsPipelineDesc withState(rhi::GraphicsPipelineDesc desc, u32 state);
    struct MaterialPipelines {
        VersionedPipeline fill[4]; // raster-only variants, by pipeline state
        VersionedPipeline rq[4];   // ray query shadows variants
        ShaderHandle rqShader;
        bool rqRequested = false;
    };

    struct FrameAllocation {
        void* cpu = nullptr;
        u64 gpu = 0;
        u64 offset = 0; // in this frame's upload buffer (m_frameBuffers[frameIndex])
    };

    void createPipelines();
    rhi::PipelineHandle tonemapPipeline(rhi::Format format);
    rhi::PipelineHandle linePipeline(rhi::Format format, bool onTop);
    void drawDebugLines(rhi::CommandList& cmd, u64 frameAddress, const GlobalPosition& origin);
    FrameAllocation allocate(u64 size, u64 alignment = 256);
    void renderInternal(rhi::CommandList& cmd, const RenderScene& scene, RenderView& view, rhi::TextureHandle output,
                        std::optional<rhi::ResourceState> outputState);
    rhi::PipelineHandle materialPipeline(ShaderHandle shader, bool rayQuery, u32 state = 0);
    rhi::PipelineHandle defaultPipeline(bool rayQuery, u32 state);
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
    rhi::PipelineHandle m_skinPipeline; // skin.comp: deformed vertices for ray tracing
    rhi::PipelineHandle m_shadowPipeline;     // depth only, for the shadow maps
    rhi::PipelineHandle m_shadowMaskPipeline; // the same, alpha-tested (cut-out casters)
    rhi::PipelineHandle m_cullPipeline;       // cull.comp: GPU frustum and occlusion culling
    rhi::PipelineHandle m_hizPipeline;        // hiz.comp: the depth pyramid
    rhi::PipelineHandle m_bloomPipeline;      // bloom.frag (HDR targets)
    std::vector<rhi::BufferHandle> m_cullCounters; // per frame in flight: GPU culling statistics, read back later
    // Per frame in flight: what GPU culling writes (draw commands, visible lists, states, counters),
    // in the GPU's own memory: its atomics and the vertex shader's reads stay off the bus.
    struct CullBuffer {
        rhi::BufferHandle buffer;
        u64 size = 0;
    };
    std::vector<CullBuffer> m_cullBuffers;
    bool m_cullCountersZeroed = false; // this frame's culling counters were reset (first culling of the frame)
    bool m_cullCountersRead = false;           // this frame's were read (first render of the frame)
    // Adds the shadow map pass (the cascades, 2 × 2 in a transient atlas) and returns the atlas.
    RGTexture addShadowMapPass(RenderGraph& graph, const RenderScene& scene, gpu::FrameData& fd, u64 instances, u64 joints,
                               const glm::vec4& wrap);
    // Adds the point shadow map pass (`lights`: indices into scene.pointLights, one atlas row each).
    RGTexture addPointShadowPass(RenderGraph& graph, const RenderScene& scene, const std::vector<u32>& lights,
                                 gpu::FrameData& fd, u64 instances, u64 joints, const glm::vec4& wrap);
    // Draws shadow casters (instance indices; all of them when `list` is null) with the depth-only pipelines.
    void drawCasters(rhi::CommandList& cmd, const RenderScene& scene, const std::vector<u32>* list, u64 frame,
                     u64 instances, u64 joints);
    RenderGraph m_graph;               // rebuilt every render call
    Scope<RenderGraphPool> m_graphPool; // the textures behind transients
    std::string m_lastGraph;
    struct GraphCallback {
        u32 id;
        RenderStage stage;
        RenderGraphCallback callback;
    };
    std::vector<GraphCallback> m_graphCallbacks;
    u32 m_nextGraphCallback = 1;
    void runGraphCallbacks(RenderStage stage, RenderGraphFrame& frame);
    // Skinned draws in the ray tracing scene: their deformed vertices and a BLAS refitted to them.
    struct SkinnedShape {
        rhi::BufferHandle vertices;
        rhi::AccelStructHandle blas;
        u32 mesh = ~0u;
        u64 lastFrame = 0;
    };
    std::unordered_map<u64, SkinnedShape> m_skinnedShapes; // by DrawItem::skinKey
    u64 m_frameNumber = 0;
    void releaseSkinnedShape(SkinnedShape& shape);
    std::unordered_map<rhi::Format, rhi::PipelineHandle> m_tonemapPipelines;
    rhi::ShaderDesc m_fullscreenVS;
    rhi::ShaderDesc m_tonemapFS;
    rhi::ShaderDesc m_lineVS;
    rhi::ShaderDesc m_lineFS;
    std::unordered_map<u32, rhi::PipelineHandle> m_linePipelines; // key: format | onTop << 8
    rhi::GraphicsPipelineDesc m_forwardDesc;                      // template of material pipelines
    rhi::GraphicsPipelineDesc m_defaultDescs[2];                  // the default material: raster, ray query
    rhi::PipelineHandle m_defaultVariants[2][4];                  // ... by pipeline state (0: the ones above)
    std::unordered_map<u32, MaterialPipelines> m_materialPipelines; // key: ShaderHandle::id
    std::unordered_map<u32, VersionedPipeline> m_postPipelines;     // key: ShaderHandle::id
    std::unordered_map<u32, VersionedPipeline> m_computePipelines;  // key: ShaderHandle::id
    void expandDraws(const RenderScene& scene, const glm::mat4& viewProj);
#if NOVA_WRAP_RENDER_MODES
    WrapShift wrapMode() const;
#endif
    std::vector<WrapInstance> m_instances;                          // this frame's instances (WrapInstances.h), reused
    std::vector<std::pair<u64, u32>> m_drawOrder;                   // (pipeline key, instance index), reused
    DebugDraw m_debugDraw;

    static constexpr u64 kFrameBufferSize = 16ull << 20;
    static constexpr u32 kMaxInstances = 16384;
    std::vector<rhi::BufferHandle> m_frameBuffers; // one linear upload buffer per frame in flight
    u64 m_frameOffset = 0;
    std::vector<rhi::AccelStructHandle> m_tlas;   // one per frame in flight

    Scope<RenderView> m_backbufferView;
    GlobalPosition m_lastOrigin;
};

} // namespace nova
