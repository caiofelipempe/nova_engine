#include "nova/renderer/Renderer.h"

#include "nova/core/Log.h"
#include "nova/platform/FileSystem.h"
#include "nova/renderer/GpuTypes.h"
#include "nova/renderer/WrapInstances.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_MSC_SECURE_CRT
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace nova {

using namespace rhi;

namespace {

ShaderDesc loadShader(const char* name) {
    auto path = fs::executableDir() / "shaders" / (std::string(name) + ".spv");
    auto bytes = fs::readBinary(path);
    NOVA_VERIFY(bytes && bytes->size() % 4 == 0, "Missing/invalid shader: " + path.string());
    ShaderDesc desc;
    desc.spirv.resize(bytes->size() / 4);
    std::memcpy(desc.spirv.data(), bytes->data(), bytes->size());
    return desc;
}

// FNV-1a over bytes: the path tracer's key of what its image depends on.
struct KeyHasher {
    u64 value = 1469598103934665603ull;
    void add(const void* data, size_t size) {
        const auto* bytes = static_cast<const u8*>(data);
        for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 1099511628211ull;
    }
    template <class T>
    void add(const T& v) {
        add(&v, sizeof(T));
    }
};

void toInstanceTransform(const glm::mat4& m, f32 out[3][4]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) out[r][c] = m[c][r]; // glm is column-major; AS wants row-major 3x4
}

} // namespace

// ------------------------------------------------------------------- camera --
CameraData CameraData::perspective(const glm::vec3& position, const glm::vec3& target, f32 fovY, f32 aspect,
                                   f32 nearClip, f32 farClip) {
    CameraData c;
    c.position = position;
    c.view = glm::lookAt(position, target, glm::vec3(0, 1, 0));
    c.projection = glm::perspective(fovY, aspect, nearClip, farClip);
    c.projection[1][1] *= -1.0f; // Vulkan clip space has Y pointing down
    return c;
}

CameraData CameraData::fromTransform(const glm::mat4& cameraToWorld, f32 fovY, f32 aspect, f32 nearClip,
                                     f32 farClip) {
    CameraData c;
    c.position = glm::vec3(cameraToWorld[3]);
    c.view = glm::inverse(cameraToWorld);
    c.projection = glm::perspective(fovY, aspect, nearClip, farClip);
    c.projection[1][1] *= -1.0f;
    return c;
}

CameraData CameraData::relative(const GlobalPosition& eye, const glm::quat& orientation, f32 fovY, f32 aspect,
                                f32 nearClip, f32 farClip) {
    CameraData c;
    c.origin = eye;
    c.position = glm::vec3(0.0f);
    c.view = glm::inverse(glm::toMat4(orientation)); // rotation only: the eye is the origin
    c.projection = glm::perspective(fovY, aspect, nearClip, farClip);
    c.projection[1][1] *= -1.0f;
    return c;
}

CameraData CameraData::relativeLookAt(const GlobalPosition& eye, const glm::vec3& direction, f32 fovY, f32 aspect,
                                      f32 nearClip, f32 farClip) {
    CameraData c;
    c.origin = eye;
    c.position = glm::vec3(0.0f);
    c.view = glm::lookAt(glm::vec3(0.0f), glm::normalize(direction), glm::vec3(0, 1, 0));
    c.projection = glm::perspective(fovY, aspect, nearClip, farClip);
    c.projection[1][1] *= -1.0f;
    return c;
}

namespace {
// The origin modulo the shaders' world tile: exact, the tile is a power of two of steps and the
// lap a whole number of tiles, so the pattern is seamless across the torus too.
glm::vec3 tiledOrigin(const GlobalPosition& origin) {
    constexpr u32 tileSteps = static_cast<u32>(gpu::kWorldTile / WorldSpace::kUnit);
    static_assert((tileSteps & (tileSteps - 1)) == 0, "the world tile must be a power of two of steps");
    glm::vec3 out;
    for (int i = 0; i < 3; ++i)
        out[i] = static_cast<f32>(static_cast<f64>(static_cast<u32>(origin.raw(i)) & (tileSteps - 1)) * WorldSpace::kUnit);
    return out;
}
} // namespace

// -------------------------------------------------------------- RenderView --
RenderView::RenderView(Renderer& renderer, Extent2D size, bool withOutput)
    : m_renderer(renderer), m_size(size), m_withOutput(withOutput) {
    create();
}

RenderView::~RenderView() { destroy(); }

void RenderView::resize(Extent2D size) {
    size.width = std::max(1u, size.width);
    size.height = std::max(1u, size.height);
    if (size == m_size) return;
    destroy();
    m_size = size;
    create();
}

void RenderView::create() {
    Device& d = m_renderer.device();
    m_size.width = std::max(1u, m_size.width);
    m_size.height = std::max(1u, m_size.height);
    m_hdr = d.createTexture({m_size.width, m_size.height, 1, Renderer::kHdrFormat,
                             TextureUsage::RenderTarget | TextureUsage::Sampled, "view.hdr"});
    m_depth = d.createTexture(
        {m_size.width, m_size.height, 1, Renderer::kDepthFormat, TextureUsage::DepthStencil | TextureUsage::Sampled,
         "view.depth"}); // sampled: the occlusion culling's pyramid
    if (m_withOutput)
        m_output = d.createTexture({m_size.width, m_size.height, 1, d.backbufferFormat(),
                                    TextureUsage::RenderTarget | TextureUsage::Sampled | TextureUsage::TransferSrc,
                                    "view.output"}); // a copy source: Renderer::captureNextFrame
}

void RenderView::destroy() {
    Device& d = m_renderer.device();
    d.destroyTexture(m_hdr);
    d.destroyTexture(m_depth);
    if (m_output) d.destroyTexture(m_output);
    if (m_hiz) d.destroyBuffer(m_hiz);
    m_hiz = {};
    m_hizValid = false;
    if (m_exposure) d.destroyBuffer(m_exposure);
    m_exposure = {};
    m_exposureValid = false;
    for (rhi::TextureHandle& h : m_history) {
        if (h) d.destroyTexture(h);
        h = {};
    }
    m_historyValid = false;
    if (m_accumulation) d.destroyTexture(m_accumulation);
    m_accumulation = {};
    m_pathSamples = 0;
    m_hdr = m_depth = m_output = {};
}

// ---------------------------------------------------------------- Renderer --
Renderer::Renderer(Device& device) : m_device(device), m_graphPool(makeScope<RenderGraphPool>(device)) {
    createPipelines();
    // Pipelines otherwise made on first use, a hitch in the first frames: the default material's
    // states (both variants), the tonemap and FXAA for the formats they write, the debug lines.
    for (int rq = 0; rq < (m_forwardRtPipeline ? 2 : 1); ++rq)
        for (u32 state = 1; state < 4; ++state) defaultPipeline(rq == 1, state);
    const Format output = m_device.backbufferFormat();
    tonemapPipeline(output);
    tonemapPipeline(Format::RGBA8_UNORM);
    fxaaPipeline(output);
    linePipeline(output, false);
    linePipeline(output, true);

    const u32 frames = device.caps().framesInFlight;
    BufferUsage usage = BufferUsage::Storage | BufferUsage::TransferSrc; // a source of copies: GPU culling's commands
    if (device.caps().accelerationStructures) usage |= BufferUsage::AccelStructInput;
    for (u32 i = 0; i < frames; ++i) {
        m_frameBuffers.push_back(
            device.createBuffer({kFrameBufferSize, usage, MemoryUsage::CpuToGpu, "renderer.frameUpload"}));
        m_cullCounters.push_back(device.createBuffer({16, BufferUsage::TransferDst, MemoryUsage::CpuToGpu, "renderer.cullCounters"}));
        std::memset(device.mappedPointer(m_cullCounters.back()), 0, 16);
        m_exposureReadback.push_back(
            device.createBuffer({16, BufferUsage::TransferDst, MemoryUsage::CpuToGpu, "renderer.exposureReadback"}));
        std::memset(device.mappedPointer(m_exposureReadback.back()), 0, 16);
        m_cullBuffers.push_back({});
        if (device.caps().accelerationStructures)
            m_tlas.push_back(device.createTLAS({kMaxInstances, "renderer.tlas"}));
    }

    m_cube = createMesh(MeshData::cube(), "Cube");
    m_sphere = createMesh(MeshData::sphere(), "Sphere");
    m_plane = createMesh(MeshData::plane(1.0f, 1), "Plane");
}

Renderer::~Renderer() {
    m_device.waitIdle();
    finishCapture();
    m_backbufferView.reset();
    m_graph.clear();
    m_graphPool.reset();
    for (u32 i = 0; i < m_meshes.size(); ++i)
        if (m_meshes[i].alive) destroyMesh({i});
    for (TextureHandle t : m_textures) m_device.destroyTexture(t);
    for (BufferHandle b : m_frameBuffers) m_device.destroyBuffer(b);
    for (BufferHandle b : m_cullCounters) m_device.destroyBuffer(b);
    for (BufferHandle b : m_exposureReadback) m_device.destroyBuffer(b);
    for (CullBuffer& b : m_cullBuffers)
        if (b.buffer) m_device.destroyBuffer(b.buffer);
    for (AccelStructHandle t : m_tlas) m_device.destroyAccelStruct(t);
    for (auto& [key, shape] : m_skinnedShapes) releaseSkinnedShape(shape);
    m_skinnedShapes.clear();
    for (PipelineHandle p : {m_skyPipeline, m_forwardPipeline, m_forwardRtPipeline, m_wireframePipeline, m_skinPipeline,
                             m_shadowPipeline, m_shadowMaskPipeline, m_prepassPipeline[0], m_prepassPipeline[1], m_cullPipeline, m_hizPipeline, m_bloomPipeline, m_exposurePipeline,
                             m_ssaoPipeline, m_ssaoApplyPipeline, m_taaPipeline, m_rtPipeline,
                             m_ptPipeline})
        if (p) m_device.destroyPipeline(p);
    for (auto& [fmt, p] : m_tonemapPipelines) m_device.destroyPipeline(p);
    for (auto& [fmt, p] : m_fxaaPipelines) m_device.destroyPipeline(p);
    for (auto& [key, p] : m_linePipelines) m_device.destroyPipeline(p);
    for (auto& [id, mp] : m_materialPipelines)
        for (int k = 0; k < 4; ++k)
            for (PipelineHandle p : {mp.fill[k].pipeline, mp.rq[k].pipeline})
                if (p) m_device.destroyPipeline(p);
    for (auto& variants : m_defaultVariants)
        for (int k = 1; k < 4; ++k) // [0] are the forward pipelines, destroyed above
            if (variants[k]) m_device.destroyPipeline(variants[k]);
    for (auto& [key, gp] : m_rtGamePipelines)
        if (gp.pipeline) m_device.destroyPipeline(gp.pipeline);
    for (auto* map : {&m_postPipelines, &m_computePipelines})
        for (auto& [id, vp] : *map)
            if (vp.pipeline) m_device.destroyPipeline(vp.pipeline);
    m_device.waitIdle();
}

void Renderer::createPipelines() {
    m_fullscreenVS = loadShader("fullscreen.vert");
    m_tonemapFS = loadShader("tonemap.frag");
    m_fxaaFS = loadShader("fxaa.frag");
    m_lineVS = loadShader("debug_line.vert");
    m_lineFS = loadShader("debug_line.frag");
    ShaderDesc forwardVS = loadShader("forward.vert");

    GraphicsPipelineDesc sky;
    sky.vertex = m_fullscreenVS;
    sky.fragment = loadShader("sky.frag");
    sky.colorFormats = {kHdrFormat, kHdrFormat, kMotionFormat}; // the forward pass: color, the ambient's share (SSAO), motion (TAA)
    sky.depthFormat = kDepthFormat;
    sky.cullMode = CullMode::None;
    sky.depthTest = false;
    sky.depthWrite = false;
    sky.debugName = "pipeline.sky";
    m_skyPipeline = m_device.createGraphicsPipeline(sky);

    GraphicsPipelineDesc fwd;
    fwd.vertex = forwardVS;
    fwd.fragment = loadShader("forward.frag");
    fwd.colorFormats = {kHdrFormat, kHdrFormat, kMotionFormat}; // color, the ambient's share, motion (material.glsl)
    fwd.depthCompare = CompareOp::LessEqual; // equal passes: the depth prepass wrote the same depth
    fwd.depthFormat = kDepthFormat;
    fwd.debugName = "pipeline.forward";
    m_forwardPipeline = m_device.createGraphicsPipeline(fwd);
    m_defaultDescs[0] = fwd;
    m_defaultVariants[0][0] = m_forwardPipeline;
    m_forwardDesc = fwd;
    m_forwardDesc.fragment = {};
    m_forwardDesc.debugName = "pipeline.material";

    if (m_device.caps().wireframe) {
        GraphicsPipelineDesc wire = fwd;
        wire.polygonMode = PolygonMode::Line;
        wire.cullMode = CullMode::None;
        wire.debugName = "pipeline.wireframe";
        m_wireframePipeline = m_device.createGraphicsPipeline(wire);
    }

    { // shadow maps: the forward vertex shader (skinning, seams) writing depth only
        GraphicsPipelineDesc shadow;
        shadow.vertex = forwardVS;
        shadow.depthFormat = kDepthFormat;
        shadow.cullMode = CullMode::None; // thin and single-sided things cast too
        shadow.debugName = "pipeline.shadow";
        m_shadowPipeline = m_device.createGraphicsPipeline(shadow);
        shadow.fragment = loadShader("shadow.frag"); // cut-out casters: discarded where transparent
        shadow.debugName = "pipeline.shadow.mask";
        m_shadowMaskPipeline = m_device.createGraphicsPipeline(shadow);

        GraphicsPipelineDesc prepass; // the camera's depth, before the forward pass (same vertex shader)
        prepass.vertex = forwardVS;
        prepass.depthFormat = kDepthFormat;
        prepass.debugName = "pipeline.prepass";
        m_prepassPipeline[0] = m_device.createGraphicsPipeline(prepass);
        prepass.cullMode = CullMode::None;
        prepass.debugName = "pipeline.prepass.doubleSided";
        m_prepassPipeline[1] = m_device.createGraphicsPipeline(prepass);
    }

    { // bloom: fullscreen passes between HDR images
        GraphicsPipelineDesc bloom;
        bloom.vertex = m_fullscreenVS;
        bloom.fragment = loadShader("bloom.frag");
        bloom.colorFormats = {kHdrFormat};
        bloom.cullMode = CullMode::None;
        bloom.depthTest = false;
        bloom.depthWrite = false;
        bloom.debugName = "pipeline.bloom";
        m_bloomPipeline = m_device.createGraphicsPipeline(bloom);

        GraphicsPipelineDesc ssao = bloom; // fullscreen too
        ssao.fragment = loadShader("ssao.frag");
        ssao.debugName = "pipeline.ssao.apply";
        m_ssaoApplyPipeline = m_device.createGraphicsPipeline(ssao);
        ssao.colorFormats = {Format::R8_UNORM};
        ssao.debugName = "pipeline.ssao";
        m_ssaoPipeline = m_device.createGraphicsPipeline(ssao);

        GraphicsPipelineDesc taa = bloom; // fullscreen, HDR
        taa.fragment = loadShader("taa.frag");
        taa.debugName = "pipeline.taa";
        m_taaPipeline = m_device.createGraphicsPipeline(taa);
    }

    { // GPU frustum culling
        ComputePipelineDesc cull;
        cull.compute = loadShader("cull.comp");
        cull.debugName = "pipeline.cull";
        m_cullPipeline = m_device.createComputePipeline(cull);
        ComputePipelineDesc hiz;
        hiz.compute = loadShader("hiz.comp");
        hiz.debugName = "pipeline.hiz";
        m_hizPipeline = m_device.createComputePipeline(hiz);
        ComputePipelineDesc exposure;
        exposure.compute = loadShader("exposure.comp");
        exposure.debugName = "pipeline.exposure";
        m_exposurePipeline = m_device.createComputePipeline(exposure);
    }

    if (m_device.caps().accelerationStructures) {
        ComputePipelineDesc skin;
        skin.compute = loadShader("skin.comp");
        skin.debugName = "pipeline.skin";
        m_skinPipeline = m_device.createComputePipeline(skin);
    }

    if (m_device.caps().rayTracingPipeline) { // the ray traced reference view
        RayTracingPipelineDesc rt;
        rt.rayGen = loadShader("rt_reference.rgen");
        rt.miss = {loadShader("rt_reference.rmiss"), loadShader("rt_shadow.rmiss")}; // by ray type
        const ShaderDesc alpha = loadShader("rt_alpha.rahit");
        rt.hitGroups = {{loadShader("rt_reference.rchit"), alpha}, {ShaderDesc{}, alpha}}; // primary, shadow
        rt.maxRecursionDepth = 2; // shadow rays from the closest hit
        rt.debugName = "pipeline.rt.reference";
        m_rtPipeline = m_device.createRayTracingPipeline(rt);

        RayTracingPipelineDesc pt; // the path tracer: every ray from its ray generation shader
        pt.rayGen = loadShader("pt_reference.rgen");
        pt.miss = {loadShader("pt_surface.rmiss"), loadShader("rt_shadow.rmiss")};
        pt.hitGroups = {{loadShader("pt_surface.rchit"), alpha}, {ShaderDesc{}, alpha}};
        pt.maxRecursionDepth = 1;
        pt.debugName = "pipeline.pt.reference";
        m_ptPipeline = m_device.createRayTracingPipeline(pt);
    }

    if (m_device.caps().rayQuery) {
        GraphicsPipelineDesc rt = fwd;
        rt.fragment = loadShader("forward_rq.frag");
        rt.debugName = "pipeline.forward.rayquery";
        m_forwardRtPipeline = m_device.createGraphicsPipeline(rt);
        m_defaultDescs[1] = rt;
        m_defaultVariants[1][0] = m_forwardRtPipeline;
    }
}

PipelineHandle Renderer::tonemapPipeline(Format format) {
    auto it = m_tonemapPipelines.find(format);
    if (it != m_tonemapPipelines.end()) return it->second;
    GraphicsPipelineDesc desc;
    desc.vertex = m_fullscreenVS;
    desc.fragment = m_tonemapFS;
    desc.colorFormats = {format};
    desc.cullMode = CullMode::None;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.debugName = "pipeline.tonemap";
    PipelineHandle p = m_device.createGraphicsPipeline(desc);
    m_tonemapPipelines.emplace(format, p);
    return p;
}

PipelineHandle Renderer::fxaaPipeline(Format format) {
    auto it = m_fxaaPipelines.find(format);
    if (it != m_fxaaPipelines.end()) return it->second;
    GraphicsPipelineDesc desc;
    desc.vertex = m_fullscreenVS;
    desc.fragment = m_fxaaFS;
    desc.colorFormats = {format};
    desc.cullMode = CullMode::None;
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.debugName = "pipeline.fxaa";
    PipelineHandle p = m_device.createGraphicsPipeline(desc);
    m_fxaaPipelines.emplace(format, p);
    return p;
}

MeshHandle Renderer::createMesh(const MeshData& data, const std::string& name) {
    NOVA_VERIFY(!data.vertices.empty() && !data.indices.empty(), "createMesh: empty mesh");
    GpuMesh mesh;
    mesh.name = name;
    mesh.vertexCount = static_cast<u32>(data.vertices.size());
    mesh.indexCount = static_cast<u32>(data.indices.size());

    const bool rt = m_device.caps().accelerationStructures;
    BufferUsage extra = rt ? BufferUsage::AccelStructInput : BufferUsage::None;
    const u64 vbSize = data.vertices.size() * sizeof(Vertex);
    const u64 ibSize = data.indices.size() * sizeof(u32);
    mesh.vertexBuffer = m_device.createBuffer({vbSize, BufferUsage::Storage | extra, MemoryUsage::GpuOnly, name.c_str()});
    mesh.indexBuffer = m_device.createBuffer(
        {ibSize, BufferUsage::Index | BufferUsage::Storage | extra, MemoryUsage::GpuOnly, name.c_str()});
    m_device.uploadBuffer(mesh.vertexBuffer, data.vertices.data(), vbSize, 0);
    m_device.uploadBuffer(mesh.indexBuffer, data.indices.data(), ibSize, 0);
    if (data.skin.size() == data.vertices.size()) { // skinned: the weights beside the vertices
        const u64 skinSize = data.skin.size() * sizeof(SkinVertex);
        mesh.skinBuffer = m_device.createBuffer({skinSize, BufferUsage::Storage, MemoryUsage::GpuOnly, name.c_str()});
        m_device.uploadBuffer(mesh.skinBuffer, data.skin.data(), skinSize, 0);
    }

    if (rt) {
        BLASDesc blas;
        blas.vertexBuffer = mesh.vertexBuffer;
        blas.vertexStride = sizeof(Vertex);
        blas.vertexCount = mesh.vertexCount;
        blas.indexBuffer = mesh.indexBuffer;
        blas.indexCount = mesh.indexCount;
        blas.debugName = name.c_str();
        mesh.blas = m_device.createBLAS(blas);
    }
    mesh.alive = true;
    mesh.cpu = data;
    mesh.bounds = data.bounds();

    u32 id;
    if (!m_freeMeshSlots.empty()) {
        id = m_freeMeshSlots.back();
        m_freeMeshSlots.pop_back();
        m_meshes[id] = std::move(mesh);
    } else {
        id = static_cast<u32>(m_meshes.size());
        m_meshes.push_back(std::move(mesh));
    }
    return {id};
}

void Renderer::releaseSkinnedShape(SkinnedShape& shape) {
    if (shape.blas) m_device.destroyAccelStruct(shape.blas);
    if (shape.vertices) m_device.destroyBuffer(shape.vertices);
    shape = {};
}

void Renderer::destroyMesh(MeshHandle h) {
    if (h.id >= m_meshes.size() || !m_meshes[h.id].alive) return;
    GpuMesh& m = m_meshes[h.id];
    m_device.destroyBuffer(m.vertexBuffer);
    m_device.destroyBuffer(m.indexBuffer);
    if (m.skinBuffer) m_device.destroyBuffer(m.skinBuffer);
    if (m.blas) m_device.destroyAccelStruct(m.blas);
    m = {};
    m_freeMeshSlots.push_back(h.id);
}

const std::string& Renderer::meshName(MeshHandle h) const {
    static const std::string none = "<none>";
    return (h.id < m_meshes.size() && m_meshes[h.id].alive) ? m_meshes[h.id].name : none;
}

MeshHandle Renderer::findMesh(const std::string& name) const {
    for (u32 i = 0; i < m_meshes.size(); ++i)
        if (m_meshes[i].alive && m_meshes[i].name == name) return {i};
    return {};
}

const MeshData* Renderer::meshData(MeshHandle h) const {
    return (h.id < m_meshes.size() && m_meshes[h.id].alive) ? &m_meshes[h.id].cpu : nullptr;
}

Bounds Renderer::meshBounds(MeshHandle h) const {
    return (h.id < m_meshes.size() && m_meshes[h.id].alive) ? m_meshes[h.id].bounds : Bounds{};
}

std::vector<MeshHandle> Renderer::meshes() const {
    std::vector<MeshHandle> out;
    for (u32 i = 0; i < m_meshes.size(); ++i)
        if (m_meshes[i].alive) out.push_back({i});
    return out;
}

TextureHandle Renderer::createTexture2D(u32 width, u32 height, const void* rgba8, const char* name) {
    return createTexture2D(width, height, rgba8, name, true);
}

TextureHandle Renderer::createTexture2D(u32 width, u32 height, const void* rgba8, const char* name, bool srgb) {
    // The full mip chain (generated by uploadTexture): textures seen far or at grazing angles are
    // filtered (trilinear, anisotropic) instead of shimmering.
    u32 levels = 1;
    for (u32 size = std::max(width, height); size > 1; size /= 2) ++levels;
    TextureHandle t = m_device.createTexture({width, height, levels, srgb ? Format::RGBA8_SRGB : Format::RGBA8_UNORM,
                                              TextureUsage::Sampled | TextureUsage::TransferDst | TextureUsage::TransferSrc,
                                              name});
    m_device.uploadTexture(t, rgba8, u64(width) * height * 4);
    m_textures.push_back(t);
    return t;
}

Scope<RenderView> Renderer::createView(Extent2D size) {
    return Scope<RenderView>(new RenderView(*this, size, true));
}

// ------------------------------------------------------------ game shaders --
bool Renderer::rebuild(VersionedPipeline& vp, ShaderHandle shader, const GraphicsPipelineDesc* graphics) {
    if (!m_shaders.ready(shader)) return false;
    const u32 version = m_shaders.version(shader);
    if (vp.pipeline && vp.version == version) return true;
    if (vp.pipeline) m_device.destroyPipeline(vp.pipeline); // deferred: frames in flight keep it
    const std::string name = m_shaders.path(shader).filename().string();
    if (graphics) {
        GraphicsPipelineDesc desc = *graphics;
        desc.fragment = m_shaders.desc(shader);
        desc.debugName = name.c_str();
        vp.pipeline = m_device.createGraphicsPipeline(desc);
    } else {
        ComputePipelineDesc desc;
        desc.compute = m_shaders.desc(shader);
        desc.debugName = name.c_str();
        vp.pipeline = m_device.createComputePipeline(desc);
    }
    vp.version = version;
    return static_cast<bool>(vp.pipeline);
}

GraphicsPipelineDesc Renderer::withState(GraphicsPipelineDesc desc, u32 state) {
    if (state & 1u) desc.cullMode = CullMode::None; // double sided (the shaders turn back faces' normals)
    if (state & 2u) {                               // see-through: blended over what is behind
        desc.blend = BlendMode::Alpha;
        desc.depthWrite = false;
    }
    return desc;
}

// The default material's pipeline for a state (the variants made on first use).
PipelineHandle Renderer::defaultPipeline(bool rayQuery, u32 state) {
    const int rq = rayQuery && m_forwardRtPipeline ? 1 : 0;
    PipelineHandle& p = m_defaultVariants[rq][state & 3u];
    if (!p) {
        GraphicsPipelineDesc desc = withState(m_defaultDescs[rq], state);
        desc.debugName = "pipeline.forward.variant";
        p = m_device.createGraphicsPipeline(desc);
    }
    return p;
}

PipelineHandle Renderer::materialPipeline(ShaderHandle shader, bool rayQuery, u32 state) {
    if (!m_shaders.ready(shader)) return {};
    state &= 3u;
    MaterialPipelines& mp = m_materialPipelines[shader.id];
    const GraphicsPipelineDesc desc = withState(m_forwardDesc, state);
    if (rayQuery) {
        if (!mp.rqRequested) { // compiled on first use: games without RT never pay for it
            mp.rqRequested = true;
            mp.rqShader = m_shaders.variant(shader, {"NOVA_RAY_QUERY=1"}, "rq");
            if (!mp.rqShader)
                NOVA_CORE_WARN("Renderer: material '{}' has no ray query variant; drawn without ray-traced shadows",
                               m_shaders.path(shader).filename().string());
        }
        if (mp.rqShader && rebuild(mp.rq[state], mp.rqShader, &desc)) return mp.rq[state].pipeline;
    }
    return rebuild(mp.fill[state], shader, &desc) ? mp.fill[state].pipeline : PipelineHandle{};
}

PipelineHandle Renderer::postProcessPipeline(ShaderHandle shader) {
    if (!m_shaders.ready(shader)) return {};
    GraphicsPipelineDesc desc;
    desc.vertex = m_fullscreenVS;
    desc.colorFormats = {kHdrFormat};
    desc.cullMode = CullMode::None;
    desc.depthTest = false;
    desc.depthWrite = false;
    VersionedPipeline& vp = m_postPipelines[shader.id];
    return rebuild(vp, shader, &desc) ? vp.pipeline : PipelineHandle{};
}

PipelineHandle Renderer::rayTracingPipeline(const RayTracingShaders& shaders) {
    if (!m_device.caps().rayTracingPipeline) return {};
    // Every shader in a fixed order (invalid optional ones as such), for the key and the versions.
    std::vector<ShaderHandle> all{shaders.rayGen};
    for (ShaderHandle m : shaders.miss) all.push_back(m);
    for (const RayTracingHitGroup& g : shaders.hitGroups) {
        all.push_back(g.closestHit);
        all.push_back(g.anyHit);
    }
    std::string key = std::to_string(shaders.maxRecursionDepth) + ":" + std::to_string(shaders.miss.size()) + ":";
    std::vector<u32> versions;
    for (ShaderHandle h : all) {
        key += std::to_string(h.id) + ",";
        if (!h) { // an optional shader left out
            versions.push_back(0);
            continue;
        }
        if (!m_shaders.ready(h)) return {}; // not compiled (yet)
        versions.push_back(m_shaders.version(h));
    }
    if (!m_shaders.ready(shaders.rayGen)) return {};
    GameRtPipeline& entry = m_rtGamePipelines[key];
    if (entry.pipeline && entry.versions == versions) return entry.pipeline;
    if (entry.pipeline) m_device.destroyPipeline(entry.pipeline); // deferred: frames in flight keep it
    auto descOf = [this](ShaderHandle h) { return h ? m_shaders.desc(h) : ShaderDesc{}; };
    RayTracingPipelineDesc desc;
    desc.rayGen = descOf(shaders.rayGen);
    for (ShaderHandle m : shaders.miss) desc.miss.push_back(descOf(m));
    for (const RayTracingHitGroup& g : shaders.hitGroups) desc.hitGroups.push_back({descOf(g.closestHit), descOf(g.anyHit)});
    desc.maxRecursionDepth = shaders.maxRecursionDepth;
    const std::string name = m_shaders.path(shaders.rayGen).filename().string();
    desc.debugName = name.c_str();
    entry.pipeline = m_device.createRayTracingPipeline(desc);
    entry.versions = std::move(versions);
    return entry.pipeline;
}

PipelineHandle Renderer::computePipeline(ShaderHandle shader) {
    if (!m_shaders.ready(shader) || m_shaders.stage(shader) != ShaderStage::Compute) return {};
    VersionedPipeline& vp = m_computePipelines[shader.id];
    return rebuild(vp, shader, nullptr) ? vp.pipeline : PipelineHandle{};
}

void Renderer::captureNextFrame(std::filesystem::path path) { m_capturePath = std::move(path); }

// Writes a capture made last frame (waits for the GPU: captures are for tests and bug reports).
void Renderer::finishCapture() {
    if (!m_pendingCapture) return;
    PendingCapture c = std::move(*m_pendingCapture);
    m_pendingCapture.reset();
    m_device.waitIdle();
    const u8* src = static_cast<const u8*>(m_device.mappedPointer(c.buffer));
    std::vector<u8> pixels(size_t(c.extent.width) * c.extent.height * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = src[i + (c.bgra ? 2 : 0)];
        pixels[i + 1] = src[i + 1];
        pixels[i + 2] = src[i + (c.bgra ? 0 : 2)];
        pixels[i + 3] = 255;
    }
    m_device.destroyBuffer(c.buffer);
    if (!c.path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(c.path.parent_path(), ec);
    }
    const bool ok = stbi_write_png(c.path.string().c_str(), static_cast<int>(c.extent.width), static_cast<int>(c.extent.height),
                                   4, pixels.data(), static_cast<int>(c.extent.width * 4)) != 0;
    if (ok) NOVA_CORE_INFO("Renderer: captured {}", c.path.string());
    else NOVA_CORE_ERROR("Renderer: could not write {}", c.path.string());
}

void Renderer::beginFrame() {
    finishCapture();
    m_shaders.update(); // hot reload: pipelines notice the new version when next used
    m_frameOffset = 0;
    m_cullCountersRead = false;
    m_cullCountersZeroed = false;
    m_graphPool->endFrame(); // transient textures no graph used for a while are released
    m_lastStats = m_stats; // UI code runs before rendering, so expose the previous frame's numbers
    m_stats = {};
    m_debugDraw.clear();
    m_debugDraw.setOrigin(m_lastOrigin);
}

Renderer::FrameAllocation Renderer::allocate(u64 size, u64 alignment) {
    u64 offset = (m_frameOffset + alignment - 1) & ~(alignment - 1);
    if (offset + size > kFrameBufferSize) return {};
    m_frameOffset = offset + size;
    BufferHandle buf = m_frameBuffers[m_device.frameIndex()];
    return {static_cast<u8*>(m_device.mappedPointer(buf)) + offset, m_device.deviceAddress(buf) + offset, offset};
}

void Renderer::render(CommandList& cmd, const RenderScene& scene, RenderView& view) {
    renderInternal(cmd, scene, view, view.m_output, ResourceState::ShaderRead);
}

void Renderer::renderToBackbuffer(CommandList& cmd, const RenderScene& scene) {
    Extent2D size = m_device.backbufferExtent();
    if (!m_backbufferView) m_backbufferView.reset(new RenderView(*this, size, false));
    else m_backbufferView->resize(size);
    renderInternal(cmd, scene, *m_backbufferView, m_device.backbuffer(), std::nullopt);
}

// ------------------------------------------------------------- instances --
#if NOVA_WRAP_RENDER_MODES
// The seam mode this GPU can draw: without clip distances, split falls back to per triangle and
// crop to per object.
WrapShift Renderer::wrapMode() const {
    const WrapShift m = m_settings.wrapShift;
    if (m_device.caps().clipDistance) return m;
    return m == WrapShift::Split ? WrapShift::Triangle : m == WrapShift::Crop ? WrapShift::Object : m;
}
#endif

// The draws as instances (WrapInstances.h). Builds with NOVA_WRAP_RENDER_MODES choose how objects
// across a seam of the circular world are drawn and how many laps around; otherwise each object
// is drawn once, at its copy nearest the camera.
void Renderer::expandDraws(const RenderScene& scene, const glm::mat4& viewProj) {
    WrapOptions options;
    options.size = scene.wrapSize;
    options.maxInstances = kMaxInstances;
#if NOVA_WRAP_RENDER_MODES
    options.mode = wrapMode();
    options.repeat = m_settings.wrapRepeat;
    options.repeatAxes = m_settings.wrapRepeatAxes;
    options.crop = m_settings.wrapCrop;
    options.worldCenter = scene.worldCenter;
#endif
    auto bounds = [this](MeshHandle h) -> const Bounds* {
        return h.id < m_meshes.size() && m_meshes[h.id].alive ? &m_meshes[h.id].bounds : nullptr;
    };
    expandWrapInstances(scene.draws, bounds, viewProj, options, m_instances);
}

// Cascaded shadow maps of the sun: the view up to shadowDistance in 4 slices, each fitted with a
// sphere and seen by an orthographic camera along the light, snapped to whole texels in the world
// (no shimmer as the camera moves), rendered into one quarter of the atlas.
RGTexture Renderer::addShadowMapPass(RenderGraph& graph, const RenderScene& scene, gpu::FrameData& fd, u64 instances,
                                     u64 joints, const glm::vec4& wrap) {
    std::array<glm::mat4, gpu::kShadowCascades> matrices; // kept here: the frame data is not read back
    const RenderSettings& s = m_settings;
    const u32 res = std::clamp(s.shadowMapSize, 256u, 4096u);
    const RGTexture atlas = graph.create({res * 2, res * 2, kDepthFormat, "shadow atlas"}); // a transient

    // The view's slices: practical split (log and linear mixed).
    const glm::mat4& proj = scene.camera.projection;
    const f32 tanX = 1.0f / std::abs(proj[0][0]), tanY = 1.0f / std::abs(proj[1][1]);
    const f32 nearClip = std::max(proj[3][2] / proj[2][2], 0.01f);
    const f32 farShadow = std::max(s.shadowDistance, nearClip * 2.0f);
    const glm::mat3 toWorld = glm::transpose(glm::mat3(scene.camera.view)); // the view is a rotation
    const glm::vec3 forward = toWorld * glm::vec3(0.0f, 0.0f, -1.0f);
    f32 splits[gpu::kShadowCascades];
    for (u32 c = 0; c < gpu::kShadowCascades; ++c) {
        const f32 t = static_cast<f32>(c + 1) / gpu::kShadowCascades;
        splits[c] = 0.75f * nearClip * std::pow(farShadow / nearClip, t) + 0.25f * (nearClip + (farShadow - nearClip) * t);
    }
    // The light's frame: it travels along `light`.
    const glm::vec3 light = glm::normalize(scene.sun.direction);
    const glm::vec3 helper = std::abs(light.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 right = glm::normalize(glm::cross(light, helper)), up = glm::cross(right, light);
    const glm::dvec3 camera = scene.camera.origin.toMeters(); // to snap to texels fixed in the world
    const f64 camRight = glm::dot(camera, glm::dvec3(right)), camUp = glm::dot(camera, glm::dvec3(up));
    constexpr f32 kCasterReach = 80.0f; // casters up to this far beyond a slice, toward the light

    for (u32 c = 0; c < gpu::kShadowCascades; ++c) {
        const f32 d0 = c == 0 ? nearClip : splits[c - 1], d1 = splits[c];
        glm::vec3 corners[8];
        glm::vec3 center(0.0f);
        for (int k = 0; k < 8; ++k) {
            const f32 d = (k & 4) ? d1 : d0;
            corners[k] = toWorld * glm::vec3(((k & 1) ? 1.0f : -1.0f) * d * tanX, ((k & 2) ? 1.0f : -1.0f) * d * tanY, -d);
            center += corners[k] / 8.0f;
        }
        f32 radius = 0.0f;
        for (const glm::vec3& p : corners) radius = std::max(radius, glm::length(p - center));
        radius = std::ceil(radius * 16.0f) / 16.0f; // a stable size
        const f64 texel = 2.0 * radius / res;
        // Snap the center to whole texels of a grid fixed in the world.
        const f64 x = std::floor((glm::dot(center, right) + camRight) / texel) * texel - camRight;
        const f64 y = std::floor((glm::dot(center, up) + camUp) / texel) * texel - camUp;
        const glm::vec3 snapped = right * static_cast<f32>(x) + up * static_cast<f32>(y) + light * glm::dot(center, light);
        const glm::vec3 eye = snapped - light * (radius + kCasterReach);
        const glm::mat4 lightView = glm::lookAt(eye, snapped, up);
        glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + kCasterReach);
        lightProj[1][1] *= -1.0f; // as the camera's: y down in Vulkan's clip space
        matrices[c] = lightProj * lightView;
        fd.shadowMatrices[c] = matrices[c];
        fd.shadowSplits[c] = d1;
        fd.shadowTexels[c] = static_cast<f32>(texel);
    }
    // Each cascade draws only the casters whose bounding sphere reaches its box (the ortho volume,
    // which already reaches kCasterReach toward the light): the near cascades see few objects.
    std::array<std::vector<u32>, gpu::kShadowCascades> casters;
    for (u32 i = 0; i < m_instances.size(); ++i) {
        const WrapInstance& wi = m_instances[i];
        const DrawItem& d = scene.draws[wi.draw];
        if (!d.castShadows || d.material.alphaMode == AlphaMode::Blend) continue;
        glm::mat4 model = d.transform;
        model[3] += glm::vec4(wi.offset, 0.0f);
        const Bounds& b = m_meshes[d.mesh.id].bounds;
        const f32 scale = std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                                    glm::length(glm::vec3(model[2]))});
        const glm::vec4 center = model * glm::vec4(b.center(), 1.0f);
        const f32 radius = glm::length(b.extents()) * scale;
        for (u32 c = 0; c < gpu::kShadowCascades; ++c) {
            const glm::vec4 clip = matrices[c] * center; // orthographic: w = 1
            // The sphere's extent in clip space on each axis (|row| · radius: conservative).
            const glm::mat4& m = matrices[c];
            const f32 rx = radius * (std::abs(m[0][0]) + std::abs(m[1][0]) + std::abs(m[2][0]));
            const f32 ry = radius * (std::abs(m[0][1]) + std::abs(m[1][1]) + std::abs(m[2][1]));
            const f32 rz = radius * (std::abs(m[0][2]) + std::abs(m[1][2]) + std::abs(m[2][2]));
            if (std::abs(clip.x) <= 1.0f + rx && std::abs(clip.y) <= 1.0f + ry && clip.z >= -rz && clip.z <= 1.0f + rz)
                casters[c].push_back(i);
        }
    }
    fd.shadowParams = glm::vec4(-1.0f, 0.0015f, static_cast<f32>(res), std::clamp(s.shadowCascadeBlend, 0.0f, 0.5f)); // x: the atlas, set when it exists
    fd.cameraForward = glm::vec4(forward, 0.0f);
    fd.flags |= gpu::kFrameFlagShadowMaps;

    // Each cascade: the scene's casters into its quarter, through frame data seen from the light
    // (the vertex shader reads only viewProj and wrapSize; the frame data in the upload buffer is
    // write-combined memory, never read back).
    gpu::FrameData* frame = &fd;
    graph.addPass("Shadow maps").depth(atlas).execute([this, &scene, frame, atlas, res, instances, joints, wrap, matrices,
                                                       casters = std::move(casters)](RenderPassContext& ctx) {
        CommandList& cmd = ctx.cmd;
        frame->shadowParams.x = static_cast<f32>(ctx.bindlessIndex(atlas)); // mapped memory, read when the GPU runs
        for (u32 c = 0; c < gpu::kShadowCascades; ++c) {
            FrameAllocation cascade = allocate(sizeof(gpu::FrameData));
            if (!cascade.cpu) break;
            auto* cf = static_cast<gpu::FrameData*>(cascade.cpu);
            cf->viewProj = matrices[c];
            cf->wrapSize = wrap;
            const f32 x = static_cast<f32>((c & 1u) * res), y = static_cast<f32>((c >> 1) * res);
            cmd.setViewport(x, y, static_cast<f32>(res), static_cast<f32>(res));
            cmd.setScissor(static_cast<i32>(x), static_cast<i32>(y), res, res);
            drawCasters(cmd, scene, &casters[c], cascade.gpu, instances, joints);
        }
    });
    return atlas;
}

void Renderer::drawCasters(CommandList& cmd, const RenderScene& scene, const std::vector<u32>* list, u64 frame,
                           u64 instances, u64 joints) {
    gpu::DrawPush push{};
    push.frame = frame;
    push.instances = instances;
    BufferHandle boundIndex{};
    PipelineHandle bound{};
    const u32 count = list ? static_cast<u32>(list->size()) : static_cast<u32>(m_instances.size());
    for (u32 k = 0; k < count; ++k) {
        const u32 i = list ? (*list)[k] : k;
        const WrapInstance& di = m_instances[i];
        const DrawItem& d = scene.draws[di.draw];
        if (!d.castShadows || d.material.alphaMode == AlphaMode::Blend) continue;
        const PipelineHandle want =
            d.material.alphaMode == AlphaMode::Mask && m_shadowMaskPipeline ? m_shadowMaskPipeline : m_shadowPipeline;
        if (!(want == bound)) {
            cmd.bindPipeline(want);
            bound = want;
        }
        const GpuMesh& mesh = m_meshes[d.mesh.id];
        if (!(mesh.indexBuffer == boundIndex)) {
            cmd.bindIndexBuffer(mesh.indexBuffer);
            boundIndex = mesh.indexBuffer;
        }
        push.vertices = m_device.deviceAddress(mesh.vertexBuffer);
        push.instanceIndex = i;
        push.indices = di.triangles ? m_device.deviceAddress(mesh.indexBuffer) : 0;
        const bool skinned = d.jointCount > 0 && mesh.skinBuffer && joints && d.jointOffset + d.jointCount <= scene.joints.size();
        push.skin = skinned ? m_device.deviceAddress(mesh.skinBuffer) : 0;
        push.joints = skinned ? joints + u64(d.jointOffset) * sizeof(glm::mat4) : 0;
        cmd.pushConstants(push);
        if (di.triangles) cmd.draw(mesh.indexCount);
        else cmd.drawIndexed(mesh.indexCount);
    }
}

// Cube shadow maps of point lights: each light a row of the atlas, six 90° faces (along ±X, ±Y,
// ±Z, as lighting.glsl's kNovaFaceDir / kNovaFaceUp), drawing the casters within its radius.
RGTexture Renderer::addPointShadowPass(RenderGraph& graph, const RenderScene& scene, const std::vector<u32>& lights,
                                       gpu::FrameData& fd, u64 instances, u64 joints, const glm::vec4& wrap) {
    const u32 res = std::clamp(m_settings.pointShadowMapSize, 64u, 1024u);
    const u32 count = static_cast<u32>(lights.size());
    const RGTexture atlas = graph.create({res * 6, res * count, kDepthFormat, "point shadow atlas"});
    constexpr f32 kNear = 0.05f;
    static const glm::vec3 kDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 kUps[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    struct Light {
        std::array<glm::mat4, 6> faces;
        std::vector<u32> casters; // instances whose bounding sphere reaches the light's
    };
    std::vector<Light> jobs(count);
    for (u32 k = 0; k < count; ++k) {
        const PointLightData& l = scene.pointLights[lights[k]];
        glm::mat4 proj = glm::perspective(glm::half_pi<f32>(), 1.0f, kNear, std::max(l.radius, kNear * 2.0f));
        proj[1][1] *= -1.0f; // as the camera's: y down in Vulkan's clip space
        for (int f = 0; f < 6; ++f) jobs[k].faces[f] = proj * glm::lookAt(l.position, l.position + kDirs[f], kUps[f]);
        for (u32 i = 0; i < m_instances.size(); ++i) {
            const WrapInstance& wi = m_instances[i];
            const DrawItem& d = scene.draws[wi.draw];
            if (!d.castShadows || d.material.alphaMode == AlphaMode::Blend) continue;
            glm::mat4 model = d.transform;
            model[3] += glm::vec4(wi.offset, 0.0f);
            const Bounds& b = m_meshes[d.mesh.id].bounds;
            const f32 scale = std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                                        glm::length(glm::vec3(model[2]))});
            const glm::vec3 center(model * glm::vec4(b.center(), 1.0f));
            if (glm::length(center - l.position) <= glm::length(b.extents()) * scale + l.radius) jobs[k].casters.push_back(i);
        }
    }
    fd.pointShadowParams = glm::vec4(-1.0f, static_cast<f32>(res), kNear, 0.0005f); // x: the atlas, set when it exists
    m_stats.pointShadowMaps += count;

    gpu::FrameData* frame = &fd;
    graph.addPass("Point shadow maps").depth(atlas).execute([this, &scene, frame, atlas, res, instances, joints, wrap,
                                                             jobs](RenderPassContext& ctx) {
        CommandList& cmd = ctx.cmd;
        frame->pointShadowParams.x = static_cast<f32>(ctx.bindlessIndex(atlas));
        for (u32 k = 0; k < jobs.size(); ++k)
            for (u32 f = 0; f < 6; ++f) {
                FrameAllocation face = allocate(sizeof(gpu::FrameData));
                if (!face.cpu) return;
                auto* cf = static_cast<gpu::FrameData*>(face.cpu);
                cf->viewProj = jobs[k].faces[f];
                cf->wrapSize = wrap;
                const f32 x = static_cast<f32>(f * res), y = static_cast<f32>(k * res);
                cmd.setViewport(x, y, static_cast<f32>(res), static_cast<f32>(res));
                cmd.setScissor(static_cast<i32>(x), static_cast<i32>(y), res, res);
                drawCasters(cmd, scene, &jobs[k].casters, face.gpu, instances, joints);
            }
    });
    return atlas;
}

void Renderer::renderInternal(CommandList& cmd, const RenderScene& scene, RenderView& view, TextureHandle output,
                              std::optional<ResourceState> outputState) {
    const auto cpuStart = std::chrono::steady_clock::now();
    const TextureHandle hdr = view.m_hdr;
    const TextureHandle depth = view.m_depth;
    const Extent2D extent = m_device.textureExtent(hdr);
    const RenderSettings& s = m_settings;

    // ---- per-frame constants
    // TAA: the projection moved by a sub-pixel amount, different every frame (Halton 2, 3; 8 steps).
    // (Not for the path tracer: it jitters its own paths and averages them itself.)
    const bool taa = s.antiAliasing == AntiAliasing::TAA && m_taaPipeline && s.debugView != DebugView::PathTraced;
    glm::vec2 jitter(0.0f); // NDC
    if (taa) {
        auto halton = [](u32 i, u32 base) {
            f32 f = 1.0f, r = 0.0f;
            for (; i > 0; i /= base) {
                f /= static_cast<f32>(base);
                r += f * static_cast<f32>(i % base);
            }
            return r;
        };
        const u32 i = view.m_jitterIndex++ % 8u + 1u;
        jitter = glm::vec2((halton(i, 2) - 0.5f) * 2.0f / f32(extent.width), (halton(i, 3) - 0.5f) * 2.0f / f32(extent.height));
    }
    const glm::mat4 projection =
        glm::translate(glm::mat4(1.0f), glm::vec3(jitter, 0.0f)) * scene.camera.projection; // NDC moved by `jitter`
    const glm::mat4 viewProjNoJitter = scene.camera.projection * scene.camera.view;
    const glm::mat4 viewProj = projection * scene.camera.view;
    // Motion vectors: last frame's view and transforms, if this view was drawn then.
    const u64 frameCounter = m_device.frameCounter();
    const bool motionKnown = view.m_motionFrame + 1 == frameCounter;
    const glm::vec3 originMoved = motionKnown ? glm::vec3((scene.camera.origin - view.m_prevOrigin).toMeters()) : glm::vec3(0.0f);
    expandDraws(scene, viewProj);
    const u32 drawCount = static_cast<u32>(m_instances.size());
    auto frameAlloc = allocate(sizeof(gpu::FrameData));
    auto instAlloc = allocate(std::max<u64>(1, drawCount) * sizeof(gpu::InstanceData));
    if (!frameAlloc.cpu || !instAlloc.cpu) {
        NOVA_CORE_ERROR("Renderer: per-frame upload buffer exhausted");
        return;
    }

    const bool useRt = s.rayTracedShadows && m_device.caps().rayQuery && !m_tlas.empty();
    // The ray traced reference view traces the TLAS too (built for it even without ray-traced shadows).
    const bool rtView = s.debugView == DebugView::RayTraced && m_rtPipeline && !m_tlas.empty();
    const bool ptView = s.debugView == DebugView::PathTraced && m_ptPipeline && !m_tlas.empty();
    const bool buildTlas = useRt || rtView || ptView || (s.rayTracingScene && !m_tlas.empty()); // the game's passes too

    // GPU culling statistics, counted by the GPU the last time this frame slot ran (it has finished).
    if (!m_cullCountersRead && !m_cullCounters.empty()) {
        m_cullCountersRead = true;
        auto* counters = static_cast<u32*>(m_device.mappedPointer(m_cullCounters[m_device.frameIndex()]));
        m_stats.gpuVisible = counters[0];
        m_stats.gpuTested = counters[1];
        m_stats.gpuOccluded = counters[2];
        counters[0] = counters[1] = counters[2] = 0;
        if (!m_exposureReadback.empty() && s.autoExposure)
            std::memcpy(&m_stats.autoExposure, m_device.mappedPointer(m_exposureReadback[m_device.frameIndex()]), sizeof(f32));
    }

    auto& fd = *static_cast<gpu::FrameData*>(frameAlloc.cpu);
    fd.view = scene.camera.view;
    fd.proj = projection;
    fd.viewProj = viewProj;
    fd.invViewProj = glm::inverse(fd.viewProj);
    // Last frame's view, seen from this frame's origin: a point x (from this origin) was x + moved from that one.
    fd.prevViewProj = motionKnown ? view.m_prevViewProjNoJitter * glm::translate(glm::mat4(1.0f), originMoved) : viewProjNoJitter;
    fd.jitter = glm::vec4(jitter, 0.0f, 0.0f);
    fd.cameraPos = glm::vec4(scene.camera.position, scene.time);
    fd.sunDirection = glm::vec4(glm::normalize(scene.sun.direction), scene.sun.intensity);
    fd.sunColor = glm::vec4(scene.sun.color, 0.0f);
    fd.skyZenith = glm::vec4(s.skyZenith, s.ambientIntensity);
    fd.skyHorizon = glm::vec4(s.skyHorizon, 0.0f);
    fd.groundColor = glm::vec4(s.groundColor, 0.0f);
    fd.originTiled = glm::vec4(tiledOrigin(scene.camera.origin), gpu::kWorldTile);
#if NOVA_WRAP_RENDER_MODES
    const glm::vec4 wrap(scene.wrapSize, static_cast<f32>(wrapMode()));
#else
    const glm::vec4 wrap(scene.wrapSize, 0.0f);
#endif
    fd.wrapSize = wrap;
    m_lastOrigin = scene.camera.origin;
    const u32 pointLightCount = std::min<u32>(static_cast<u32>(scene.pointLights.size()), gpu::kMaxPointLights);
    fd.pointLightCount = pointLightCount;
    u32 frameFlags = useRt ? gpu::kFrameFlagRtShadows : 0u;
    if (useRt && s.rayTracedReflections) frameFlags |= gpu::kFrameFlagRtReflections;
    if (useRt && s.rayTracedAo) frameFlags |= gpu::kFrameFlagRtAo;
    fd.rtParams = glm::vec4(std::max(s.rayTracedAoRadius, 0.01f), static_cast<f32>(std::clamp(s.rayTracedAoRays, 1u, 16u)),
                            std::clamp(s.reflectionMaxRoughness, 0.0f, 1.0f),
                            taa ? static_cast<f32>(frameCounter % 1024u) : 0.0f); // noise: turns with TAA
    fd.shadowBias = s.shadowBias;
    // Point shadow maps (without ray tracing): the nearest lights that cast shadows, one atlas row each.
    std::array<glm::ivec4, gpu::kMaxPointLights / 4> slots;
    slots.fill(glm::ivec4(-1));
    std::vector<u32> shadowLights;
    for (u32 i = 0; i < pointLightCount; ++i) {
        const PointLightData& l = scene.pointLights[i];
        fd.pointLights[i] = {glm::vec4(l.position, l.radius), glm::vec4(l.color, l.intensity)};
        if (!l.castShadows) slots[i / 4][i % 4] = -2;
        else if (l.radius > 0.0f && l.intensity > 0.0f) shadowLights.push_back(i);
    }
    if (!useRt && s.pointShadowMaps && s.maxPointShadows > 0 && m_shadowPipeline && !shadowLights.empty()) {
        auto reach = [&](u32 i) { return glm::length(scene.pointLights[i].position) - scene.pointLights[i].radius; };
        std::stable_sort(shadowLights.begin(), shadowLights.end(), [&](u32 a, u32 b) { return reach(a) < reach(b); });
        const u32 rowLimit = 8192u / std::clamp(s.pointShadowMapSize, 64u, 1024u); // the atlas stays within 8192 texels
        shadowLights.resize(std::min<size_t>(shadowLights.size(), std::min({s.maxPointShadows, 16u, rowLimit})));
        for (u32 k = 0; k < shadowLights.size(); ++k) slots[shadowLights[k] / 4][shadowLights[k] % 4] = static_cast<i32>(k);
        frameFlags |= gpu::kFrameFlagPointShadowMaps;
    } else {
        shadowLights.clear();
    }
    for (u32 k = 0; k < slots.size(); ++k) fd.pointShadowSlots[k] = slots[k];
    fd.pointShadowParams = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    fd.flags = frameFlags;

    // Joint matrices of skinned draws, for this frame.
    u64 jointsAddress = 0;
    if (!scene.joints.empty()) {
        const u64 bytes = scene.joints.size() * sizeof(glm::mat4);
        FrameAllocation jointAlloc = allocate(bytes, 64);
        if (jointAlloc.cpu) {
            std::memcpy(jointAlloc.cpu, scene.joints.data(), bytes);
            jointsAddress = jointAlloc.gpu;
        }
    }
    // With TAA, last frame's joint matrices too (in the same layout): the motion of the bones.
    u64 prevJointsAddress = 0;
    if (taa && jointsAddress) {
        std::vector<glm::mat4> previous = scene.joints; // unknown: as now (no motion of the bones)
        for (const DrawItem& d : scene.draws) {
            if (!d.objectKey || d.jointCount == 0 || d.jointOffset + d.jointCount > scene.joints.size()) continue;
            const auto first = scene.joints.begin() + d.jointOffset;
            if (motionKnown)
                if (auto it = view.m_prevJoints.find(d.objectKey); it != view.m_prevJoints.end() && it->second.size() == d.jointCount)
                    std::copy(it->second.begin(), it->second.end(), previous.begin() + d.jointOffset);
            view.m_joints[d.objectKey].assign(first, first + d.jointCount);
        }
        FrameAllocation prevAlloc = allocate(previous.size() * sizeof(glm::mat4), 64);
        if (prevAlloc.cpu) {
            std::memcpy(prevAlloc.cpu, previous.data(), previous.size() * sizeof(glm::mat4));
            prevJointsAddress = prevAlloc.gpu;
        }
    }
    std::swap(view.m_prevJoints, view.m_joints);
    view.m_joints.clear();

    // ---- the frame as a render graph: what each pass reads and writes; transitions, unneeded
    // passes and transient textures (shadow atlas, post-process images) are the graph's.
    RenderGraph& graph = m_graph;
    graph.clear();
    const RGTexture hdrTex = graph.import("hdr", hdr, extent, kHdrFormat);
    const RGTexture depthTex = graph.import("depth", depth, extent, kDepthFormat);
    const RGTexture outputTex = graph.import("output", output, extent, m_device.backbufferFormat(), outputState);
    if (!outputState) graph.markOutput(outputTex);

    // ---- skinned shapes for ray tracing: deform each skinned draw's vertices (skin.comp) into
    // a buffer of its own and refit its BLAS, so the rays see the pose, not the bind pose.
    ++m_frameNumber;
    std::unordered_map<u32, AccelStructHandle> skinnedBlas; // draw index -> its refitted BLAS
    std::unordered_map<u32, u64> skinnedVertices;           // draw index -> its deformed vertices (the pose)
    struct SkinJob {
        u64 source, skin, joints, target;
        u32 count, _pad[3];
    };
    std::vector<std::pair<SkinJob, AccelStructHandle>> skinJobs;
    if (buildTlas && m_skinPipeline && jointsAddress) {
        for (u32 di = 0; di < scene.draws.size(); ++di) {
            const DrawItem& d = scene.draws[di];
            if (!d.skinKey || d.jointCount == 0 || !d.castShadows || d.mesh.id >= m_meshes.size()) continue;
            const GpuMesh& mesh = m_meshes[d.mesh.id];
            if (!mesh.alive || !mesh.skinBuffer || d.jointOffset + d.jointCount > scene.joints.size()) continue;
            SkinnedShape& shape = m_skinnedShapes[d.skinKey];
            if (shape.mesh != d.mesh.id) { // new, or another mesh: its own vertices and BLAS
                releaseSkinnedShape(shape);
                const u64 bytes = u64(mesh.vertexCount) * sizeof(Vertex);
                shape.vertices = m_device.createBuffer({bytes, BufferUsage::Storage | BufferUsage::AccelStructInput,
                                                        MemoryUsage::GpuOnly, "skinned vertices"});
                m_device.uploadBuffer(shape.vertices, mesh.cpu.vertices.data(), bytes, 0); // the bind pose to start with
                BLASDesc blas;
                blas.vertexBuffer = shape.vertices;
                blas.vertexStride = sizeof(Vertex);
                blas.vertexCount = mesh.vertexCount;
                blas.indexBuffer = mesh.indexBuffer;
                blas.indexCount = mesh.indexCount;
                blas.allowUpdate = true;
                blas.debugName = "skinned blas";
                shape.blas = m_device.createBLAS(blas);
                shape.mesh = d.mesh.id;
            }
            shape.lastFrame = m_frameNumber;
            skinJobs.push_back({SkinJob{m_device.deviceAddress(mesh.vertexBuffer), m_device.deviceAddress(mesh.skinBuffer),
                                        jointsAddress + u64(d.jointOffset) * sizeof(glm::mat4),
                                        m_device.deviceAddress(shape.vertices), mesh.vertexCount, {0, 0, 0}},
                                shape.blas});
            skinnedBlas[di] = shape.blas;
            skinnedVertices[di] = m_device.deviceAddress(shape.vertices);
        }
    }
    if (!skinJobs.empty())
        graph.addPass("Skinned shapes").sideEffect().execute([this, &skinJobs](RenderPassContext& ctx) {
            ctx.cmd.bindPipeline(m_skinPipeline);
            for (const auto& [job, blas] : skinJobs) {
                ctx.cmd.pushConstants(job);
                ctx.cmd.dispatch((job.count + 63) / 64);
                ctx.cmd.updateBLAS(blas); // waits for the shader's writes
            }
        });
    // Shapes of skinned objects not drawn for a while (gone, or culled): released.
    for (auto it = m_skinnedShapes.begin(); it != m_skinnedShapes.end();)
        if (m_frameNumber - it->second.lastFrame > 120) {
            releaseSkinnedShape(it->second);
            it = m_skinnedShapes.erase(it);
        } else {
            ++it;
        }

    // ---- instances (+ TLAS records)
    auto* instances = static_cast<gpu::InstanceData*>(instAlloc.cpu);
    AccelStructInstance* tlasRecords = nullptr;
    FrameAllocation tlasAlloc;
    if (buildTlas) {
        tlasAlloc = allocate(std::max<u64>(1, drawCount) * sizeof(AccelStructInstance), 16);
        tlasRecords = static_cast<AccelStructInstance*>(tlasAlloc.cpu);
    }
    u32 tlasCount = 0;
    KeyHasher pathKey; // the path tracer's: objects here, then the camera, lights and sky
    for (u32 i = 0; i < drawCount; ++i) {
        const WrapInstance& di = m_instances[i];
        const DrawItem& d = scene.draws[di.draw];
        gpu::InstanceData& inst = instances[i];
        glm::mat4 model = d.transform;
        model[3] += glm::vec4(di.offset, 0.0f); // a copy in another lap: whole laps over
        inst.model = model;
        if (ptView) { // what the path tracer sees of it
            pathKey.add(model);
            pathKey.add(d.mesh.id);
            pathKey.add(d.material.baseColor);
            pathKey.add(d.material.emissive);
            pathKey.add(d.material.emissiveStrength);
            pathKey.add(d.material.metallic);
            pathKey.add(d.material.roughness);
            pathKey.add(d.material.baseColorTexture);
            pathKey.add(d.castShadows);
        }
        // Where it was last frame (from this frame's origin, in the same lap); still if unknown.
        // Only TAA reads it (motion vectors): without it, no bookkeeping.
        glm::mat4 prevModel = model;
        if (d.objectKey && taa) {
            if (motionKnown) {
                const auto it = std::lower_bound(view.m_prevModels.begin(), view.m_prevModels.end(), d.objectKey,
                                                 [](const RenderView::KeyedModel& m, u64 key) { return m.key < key; });
                if (it != view.m_prevModels.end() && it->key == d.objectKey) {
                    prevModel = it->model;
                    prevModel[3] -= glm::vec4(originMoved, 0.0f);
                    prevModel[3] += glm::vec4(di.offset, 0.0f);
                }
            }
            view.m_models.push_back({d.objectKey, d.transform});
        }
        inst.prevModel = prevModel;
        inst.normalMatrix = glm::mat4(glm::inverseTranspose(glm::mat3(d.transform))); // the shaders read its 3 × 3
        inst.baseColor = d.material.baseColor;
        inst.emissive = glm::vec4(d.material.emissive, d.material.emissiveStrength);
        u32 flags = d.material.checker ? gpu::kMaterialFlagChecker : 0u;
        f32 flagsAsFloat;
        std::memcpy(&flagsAsFloat, &flags, sizeof(f32));
        const u32 tex = d.material.baseColorTexture ? m_device.bindlessIndex(d.material.baseColorTexture) : ~0u;
        inst.params = glm::vec4(d.material.metallic, d.material.roughness, flagsAsFloat,
                                tex == ~0u ? -1.0f : static_cast<f32>(tex));
        inst.custom = d.material.customParams;
        const u32 customTex = d.material.customTexture ? m_device.bindlessIndex(d.material.customTexture) : ~0u;
        inst.extra = glm::vec4(customTex == ~0u ? -1.0f : static_cast<f32>(customTex), di.cell);
        auto mapIndex = [&](rhi::TextureHandle t) { return t ? static_cast<f32>(m_device.bindlessIndex(t)) : -1.0f; };
        inst.maps = glm::vec4(mapIndex(d.material.normalTexture), mapIndex(d.material.metallicRoughnessTexture),
                              mapIndex(d.material.occlusionTexture), mapIndex(d.material.emissiveTexture));
        inst.surface = glm::vec4(d.material.normalScale, d.material.occlusionStrength, d.material.alphaCutoff,
                                 static_cast<f32>(d.material.alphaMode));
        { // UV transform: offset · rotation · scale (KHR_texture_transform)
            const f32 cosine = std::cos(d.material.uvRotation), sine = std::sin(d.material.uvRotation);
            const glm::vec2 k = d.material.uvScale;
            inst.uvMatrix = glm::vec4(cosine * k.x, sine * k.x, -sine * k.y, cosine * k.y);
            inst.uvOffset = glm::vec4(d.material.uvOffset, 0.0f, 0.0f);
            const GpuMesh& gm = m_meshes[d.mesh.id];
            // Rays read the triangle they hit from here: skinned draws in the ray tracing scene, their pose.
            const auto deformed = skinnedVertices.find(di.draw);
            const u64 v = deformed != skinnedVertices.end() ? deformed->second : m_device.deviceAddress(gm.vertexBuffer);
            const u64 ix = m_device.deviceAddress(gm.indexBuffer);
            inst.mesh = glm::uvec4(u32(v), u32(v >> 32), u32(ix), u32(ix >> 32));
        }

        // Split copies (moved off their lap's center) stay out of the TLAS: the rays see each
        // object whole, once per lap; a copy laid over its original would shadow itself.
        // Skinned draws: their refitted BLAS (the pose); without one they cast no ray-traced shadow
        // (the mesh's own BLAS is the bind pose: it would cast the undeformed shape).
        const bool skinned = d.jointCount > 0;
        const auto refitted = skinned ? skinnedBlas.find(di.draw) : skinnedBlas.end();
        const AccelStructHandle blas = skinned ? (refitted != skinnedBlas.end() ? refitted->second : AccelStructHandle{})
                                               : m_meshes[d.mesh.id].blas;
        const bool seeThrough = d.material.alphaMode == AlphaMode::Blend;
        if (tlasRecords && d.castShadows && blas && !di.splitCopy && !seeThrough) {
            // Built here and copied whole: the record has bitfields, and setting a bitfield reads the
            // word first, very slow in the write-combined upload memory.
            AccelStructInstance r{};
            toInstanceTransform(model, r.transform);
            r.instanceCustomIndex = i;
            r.mask = 0xFF;
            r.sbtRecordOffset = 0;
            // Cut-out materials are tested by their alpha in the shadow rays; the rest is opaque.
            r.flags = AccelInstanceTriangleCullDisable |
                      (d.material.alphaMode == AlphaMode::Mask ? AccelInstanceForceNoOpaque : AccelInstanceForceOpaque);
            r.blasAddress = m_device.accelStructAddress(blas);
            std::memcpy(&tlasRecords[tlasCount++], &r, sizeof(r));
        }
    }

    // This frame becomes the view's last one (motion vectors next frame).
    if (taa) {
        std::sort(view.m_models.begin(), view.m_models.end(),
                  [](const RenderView::KeyedModel& a, const RenderView::KeyedModel& b) { return a.key < b.key; });
        std::swap(view.m_prevModels, view.m_models);
        view.m_models.clear(); // keeps its capacity
        view.m_prevViewProjNoJitter = viewProjNoJitter;
        view.m_motionFrame = frameCounter;
    } else {
        view.m_prevModels.clear();
        view.m_models.clear();
    }

    u64 tlasAddress = 0, builtTlas = 0; // tlasAddress: for the forward pass's ray-traced shadows
    if (buildTlas) {
        AccelStructHandle tlas = m_tlas[m_device.frameIndex()];
        const u64 records = tlasAlloc.gpu;
        graph.addPass("TLAS build").sideEffect().execute([tlas, records, tlasCount](RenderPassContext& ctx) {
            ctx.cmd.buildTLAS(tlas, records, tlasCount);
        });
        builtTlas = m_device.accelStructAddress(tlas);
        if (useRt) tlasAddress = builtTlas;
        m_stats.tlasInstances += tlasCount;
    }

    // ---- shadow maps (the sun), when rays do not draw the shadows
    RGTexture shadowAtlas;
    if (!useRt && s.shadowMaps && s.shadowDistance > 0.0f && m_shadowPipeline)
        shadowAtlas = addShadowMapPass(graph, scene, fd, instAlloc.gpu, jointsAddress, wrap);
    RGTexture pointShadowAtlas;
    if (!shadowLights.empty())
        pointShadowAtlas = addPointShadowPass(graph, scene, shadowLights, fd, instAlloc.gpu, jointsAddress, wrap);

    // ---- sky + opaque + see-through
    const bool wireframe = s.wireframe && m_wireframePipeline;

    // The opaque draws grouped by pipeline (the default material's variants, then each game
    // material shader's; one whose shader is not ready falls back to the default), then the
    // see-through ones from the farthest to the nearest, blended over what is behind them.
    // With GPU culling, the plain opaque draws (not skinned, not split per triangle) go to a compute
    // shader instead, in groups of the same pipeline and mesh: one indirect draw per group.
    m_drawOrder.clear();
    std::vector<std::pair<f32, u32>> seeThrough; // (distance², instance)
    const bool gpuCull = s.gpuCulling && m_cullPipeline && m_device.caps().drawIndirectFirstInstance;
    struct CullEntry {
        u64 key;
        u32 mesh, instance;
        PipelineHandle pipeline;
        u32 prepass; // 0: a cut-out (not in the depth prepass); 1: single sided; 2: double sided
    };
    std::vector<CullEntry> cullEntries;
    for (u32 i = 0; i < drawCount; ++i) {
        const WrapInstance& wi = m_instances[i];
        const DrawItem& d = scene.draws[wi.draw]; // meshes checked by expandDraws
        const u32 state = pipelineState(d.material);
        if ((state & 2u) && !wireframe) {
            const glm::vec3 at = glm::vec3(d.transform[3]) + wi.offset; // camera-relative
            seeThrough.emplace_back(glm::dot(at, at), i);
            continue;
        }
        PipelineHandle p = wireframe ? m_wireframePipeline : PipelineHandle{};
        if (!p && d.material.shader) p = materialPipeline(d.material.shader, useRt, state);
        if (!p) p = defaultPipeline(useRt, state);
        if (gpuCull && !wi.triangles && d.jointCount == 0)
            cullEntries.push_back({p.key(), d.mesh.id, i, p,
                                   d.material.alphaMode == AlphaMode::Mask ? 0u : d.material.doubleSided ? 2u : 1u});
        else m_drawOrder.emplace_back(p.key(), i);
    }
    struct CullGroup {
        PipelineHandle pipeline;
        u32 mesh;
        u32 prepass; // as CullEntry::prepass
    };
    std::vector<CullGroup> cullGroups;
    // Occlusion: two phases around a depth pyramid of the view (see cull.comp).
    const bool occlusion = s.occlusionCulling && m_hizPipeline;
    FrameAllocation cullCommands, cullItems, cullParams, cullParamsLate, cullZeros;
    bool lateCull = false;
    // Where the GPU writes, in this frame's cull buffer: offsets of each part.
    BufferHandle cullBuffer{};
    u64 cullBase = 0, commandBytes = 0, offCommandsLate = 0, offVisible = 0, offVisibleLate = 0, offStates = 0, offCounters = 0;
    if (!cullEntries.empty()) {
        std::sort(cullEntries.begin(), cullEntries.end(), [](const CullEntry& a, const CullEntry& b) {
            if (a.key != b.key) return a.key < b.key;
            if (a.mesh != b.mesh) return a.mesh < b.mesh;
            return a.prepass != b.prepass ? a.prepass < b.prepass : a.instance < b.instance;
        });
        u32 groups = 0;
        for (size_t k = 0; k < cullEntries.size(); ++k)
            groups += k == 0 || cullEntries[k].key != cullEntries[k - 1].key || cullEntries[k].mesh != cullEntries[k - 1].mesh ||
                      cullEntries[k].prepass != cullEntries[k - 1].prepass;
        const u64 n = cullEntries.size();
        commandBytes = groups * sizeof(gpu::DrawIndexedCommand);
        cullCommands = allocate(commandBytes, 16); // their starting values, copied to the cull buffer
        cullItems = allocate(n * sizeof(gpu::CullItem), 16);
        cullParams = allocate(sizeof(gpu::CullParams), 16);
        cullZeros = allocate(16, 16);
        bool ok = cullCommands.cpu && cullItems.cpu && cullParams.cpu && cullZeros.cpu;
        if (ok && occlusion) {
            cullParamsLate = allocate(sizeof(gpu::CullParams), 16);
            lateCull = cullParamsLate.cpu != nullptr;
        }
        if (ok) {
            std::memset(cullZeros.cpu, 0, 16);
            // The cull buffer: [commands | late commands | visible | late visible | states | counters].
            auto align = [](u64 v) { return (v + 15) & ~u64(15); };
            offCommandsLate = align(commandBytes);
            offVisible = offCommandsLate + (lateCull ? align(commandBytes) : 0);
            offVisibleLate = align(offVisible + n * sizeof(u32));
            offStates = offVisibleLate + (lateCull ? align(n * sizeof(u32)) : 0);
            offCounters = offStates + (lateCull ? align(n * sizeof(u32)) : 0);
            const u64 needed = offCounters + 16;
            CullBuffer& cb = m_cullBuffers[m_device.frameIndex()];
            if (cb.size < needed) {
                if (cb.buffer) m_device.destroyBuffer(cb.buffer);
                cb.size = std::max<u64>(needed + needed / 2, 64 * 1024);
                cb.buffer = m_device.createBuffer({cb.size, BufferUsage::Storage | BufferUsage::Indirect | BufferUsage::TransferDst |
                                                               BufferUsage::TransferSrc,
                                                   MemoryUsage::GpuOnly, "renderer.cull"});
            }
            cullBuffer = cb.buffer;
            cullBase = m_device.deviceAddress(cullBuffer);
            auto* commands = static_cast<gpu::DrawIndexedCommand*>(cullCommands.cpu);
            auto* items = static_cast<gpu::CullItem*>(cullItems.cpu);
            for (u32 k = 0; k < n; ++k) {
                const CullEntry& e = cullEntries[k];
                if (cullGroups.empty() || e.key != cullEntries[k - 1].key || e.mesh != cullEntries[k - 1].mesh ||
                    e.prepass != cullEntries[k - 1].prepass) {
                    // A group's instances take its part of the visible list, from firstInstance on.
                    commands[cullGroups.size()] = {m_meshes[e.mesh].indexCount, 0u, 0u, 0, k};
                    cullGroups.push_back({e.pipeline, e.mesh, e.prepass});
                }
                // The bounding sphere of the instance (camera relative, like viewProj), from the scene's
                // own copy: the upload buffer is write-combined memory, very slow for the CPU to read.
                glm::mat4 model = scene.draws[m_instances[e.instance].draw].transform;
                model[3] += glm::vec4(m_instances[e.instance].offset, 0.0f);
                const Bounds& b = m_meshes[e.mesh].bounds;
                const f32 scale = std::max({glm::length(glm::vec3(model[0])), glm::length(glm::vec3(model[1])),
                                            glm::length(glm::vec3(model[2]))});
                items[k] = {glm::vec4(glm::vec3(model * glm::vec4(b.center(), 1.0f)), glm::length(b.extents()) * scale),
                            static_cast<u32>(cullGroups.size() - 1), e.instance, 0u, 0u};
                m_stats.triangles += m_meshes[e.mesh].indexCount / 3; // submitted (before culling)
            }
            gpu::CullParams params{}; // built here, then copied (never read back from the upload buffer)
            // Frustum planes (Gribb–Hartmann, depth 0..1), normalized: inside where dot(n, p) + d >= 0.
            const glm::mat4 m = glm::transpose(viewProj); // rows as columns
            const glm::vec4 rows[6] = {m[3] + m[0], m[3] - m[0], m[3] + m[1], m[3] - m[1], m[2], m[3] - m[2]};
            for (int k = 0; k < 6; ++k) params.planes[k] = rows[k] / std::max(glm::length(glm::vec3(rows[k])), 1e-12f);
            if (lateCull) {
                // The view's pyramid: made (again) for its size; its contents are last frame's.
                if (!view.m_hiz || view.m_hizSize.width != extent.width || view.m_hizSize.height != extent.height) {
                    if (view.m_hiz) m_device.destroyBuffer(view.m_hiz);
                    u32 w = extent.width, h = extent.height, offset = 0, count = 0;
                    do {
                        w = (w + 1) / 2;
                        h = (h + 1) / 2;
                        view.m_hizLevels[count++] = glm::uvec4(w, h, offset, 0u);
                        offset += w * h;
                    } while ((w > 1 || h > 1) && count < 16);
                    view.m_hizLevelCount = count;
                    view.m_hiz = m_device.createBuffer({u64(offset) * sizeof(f32), BufferUsage::Storage, MemoryUsage::GpuOnly, "view.hiz"});
                    view.m_hizSize = extent;
                    view.m_hizValid = false;
                }
                for (u32 l = 0; l < 16; ++l) params.levels[l] = view.m_hizLevels[l];
                params.info = glm::uvec4(view.m_hizLevelCount, extent.width, extent.height, 0u);
                // Phase 1: last frame's pyramid, seen as last frame saw it; the spheres moved by how
                // far the camera's origin moved since.
                params.occlusionViewProj = view.m_prevViewProj;
                params.occlusionOffset = glm::vec4(glm::vec3((scene.camera.origin - view.m_prevOrigin).toMeters()),
                                                   view.m_hizValid ? 1.0f : 0.0f);
                // Phase 2: this frame's pyramid and view.
                gpu::CullParams late = params;
                late.occlusionViewProj = viewProj;
                late.occlusionOffset = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                std::memcpy(cullParamsLate.cpu, &late, sizeof(late));
            }
            std::memcpy(cullParams.cpu, &params, sizeof(params));
        } else { // out of upload memory: drawn one by one, not culled
            for (const CullEntry& e : cullEntries) m_drawOrder.emplace_back(e.key, e.instance);
            cullGroups.clear();
            lateCull = false;
        }
    }
    const u64 hizAddress = lateCull ? m_device.deviceAddress(view.m_hiz) : 0;
    // The counters go back to the CPU after the last culling of the view (cumulative over the frame's views).
    const BufferHandle readback = m_cullCounters.empty() ? BufferHandle{} : m_cullCounters[m_device.frameIndex()];
    if (!cullGroups.empty()) {
        const gpu::CullPush push{cullItems.gpu, cullBase, cullBase + offVisible, cullParams.gpu, cullBase + offCounters,
                                 hizAddress, lateCull ? cullBase + offStates : 0, static_cast<u32>(cullEntries.size()),
                                 lateCull ? 1u : 0u};
        const BufferHandle upload = m_frameBuffers[m_device.frameIndex()];
        const u64 commandsSource = cullCommands.offset, zerosSource = cullZeros.offset;
        const bool zeroCounters = !m_cullCountersZeroed; // the frame's first culling: its counters start at 0
        m_cullCountersZeroed = true;
        graph.addPass("GPU culling").sideEffect().execute([=, this](RenderPassContext& ctx) {
            ctx.cmd.globalBarrier(); // earlier uses of the cull buffer and last frame's pyramid are done
            ctx.cmd.copyBuffer(upload, cullBuffer, commandBytes, commandsSource, 0); // instance counts 0
            if (lateCull) ctx.cmd.copyBuffer(upload, cullBuffer, commandBytes, commandsSource, offCommandsLate);
            if (zeroCounters) ctx.cmd.copyBuffer(upload, cullBuffer, 16, zerosSource, offCounters);
            ctx.cmd.globalBarrier();
            ctx.cmd.bindPipeline(m_cullPipeline);
            ctx.cmd.pushConstants(push);
            ctx.cmd.dispatch((push.count + 63) / 64);
            ctx.cmd.globalBarrier(); // the commands and lists, before the draws read them
            if (!lateCull && readback) ctx.cmd.copyBuffer(cullBuffer, readback, 16, offCounters, 0);
        });
    }
    // The view's pyramid follows this frame from now on (or is not kept, without occlusion).
    view.m_prevViewProj = viewProj;
    view.m_prevOrigin = scene.camera.origin;
    view.m_hizValid = lateCull;

    std::stable_sort(m_drawOrder.begin(), m_drawOrder.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::stable_sort(seeThrough.begin(), seeThrough.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const size_t opaqueEnd = m_drawOrder.size(); // the see-through ones after it
    for (const auto& [distance, i] : seeThrough) m_drawOrder.emplace_back(~0ull, i); // bound per draw

    const u64 frameAddress = frameAlloc.gpu, instancesAddress = instAlloc.gpu;
    // The draws of the forward pass: early (sky, the groups culling kept, the other opaque draws,
    // and the see-through ones unless a late pass follows) or late (the groups found visible by the
    // second occlusion phase, then the see-through ones).
    auto record = [&, frameAddress, instancesAddress, tlasAddress, jointsAddress, wireframe](CommandList& cmd, bool late) {
        if (!late) {
            cmd.bindPipeline(m_skyPipeline);
            cmd.pushConstants(gpu::SkyPush{frameAddress});
            cmd.draw(3);
        }
        gpu::DrawPush push{};
        push.frame = frameAddress;
        push.instances = instancesAddress;
        push.tlas = tlasAddress;
        push.debugView = static_cast<u32>(s.debugView);
        BufferHandle boundIndex{};
        u64 boundPipeline = ~0ull;
        // The GPU-culled groups: one indirect draw each, of the instances the culling kept.
        const u64 commands = late ? offCommandsLate : 0;
        for (u32 g = 0; g < cullGroups.size(); ++g) {
            const CullGroup& group = cullGroups[g];
            if (group.pipeline.key() != boundPipeline) {
                cmd.bindPipeline(group.pipeline);
                boundPipeline = group.pipeline.key();
            }
            const GpuMesh& mesh = m_meshes[group.mesh];
            if (!(mesh.indexBuffer == boundIndex)) {
                cmd.bindIndexBuffer(mesh.indexBuffer);
                boundIndex = mesh.indexBuffer;
            }
            push.vertices = m_device.deviceAddress(mesh.vertexBuffer);
            push.instanceIndex = 0;
            push.indices = push.skin = push.joints = push.prevJoints = 0;
            push.visible = cullBase + (late ? offVisibleLate : offVisible);
            cmd.pushConstants(push);
            cmd.drawIndexedIndirect(cullBuffer, commands + g * sizeof(gpu::DrawIndexedCommand), 1,
                                    sizeof(gpu::DrawIndexedCommand));
            ++m_stats.drawCalls;
            ++m_stats.cullGroups;
        }
        push.visible = 0;
        const size_t from = late ? opaqueEnd : 0, to = late || !lateCull ? m_drawOrder.size() : opaqueEnd;
        for (size_t k = from; k < to; ++k) {
            const auto& [pipelineKey, i] = m_drawOrder[k];
            const WrapInstance& di = m_instances[i];
            const DrawItem& d = scene.draws[di.draw];
            if (pipelineKey != boundPipeline || pipelineKey == ~0ull) {
                const u32 state = pipelineState(d.material);
                PipelineHandle p = wireframe ? m_wireframePipeline : PipelineHandle{};
                if (!p && d.material.shader) p = materialPipeline(d.material.shader, useRt, state);
                if (!p) p = defaultPipeline(useRt, state);
                if (p.key() != boundPipeline) cmd.bindPipeline(p);
                boundPipeline = p.key();
            }
            const GpuMesh& mesh = m_meshes[d.mesh.id];
            if (!(mesh.indexBuffer == boundIndex)) {
                cmd.bindIndexBuffer(mesh.indexBuffer);
                boundIndex = mesh.indexBuffer;
            }
            push.vertices = m_device.deviceAddress(mesh.vertexBuffer);
            push.instanceIndex = i;
            push.indices = di.triangles ? m_device.deviceAddress(mesh.indexBuffer) : 0; // per-triangle wrap: non-indexed
            const bool skinned = d.jointCount > 0 && mesh.skinBuffer && jointsAddress &&
                                 d.jointOffset + d.jointCount <= scene.joints.size();
            push.skin = skinned ? m_device.deviceAddress(mesh.skinBuffer) : 0;
            push.joints = skinned ? jointsAddress + u64(d.jointOffset) * sizeof(glm::mat4) : 0;
            push.prevJoints = skinned && prevJointsAddress ? prevJointsAddress + u64(d.jointOffset) * sizeof(glm::mat4) : 0;
            cmd.pushConstants(push);
            if (di.triangles) cmd.draw(mesh.indexCount);
            else cmd.drawIndexed(mesh.indexCount);
            ++m_stats.drawCalls;
            m_stats.triangles += mesh.indexCount / 3;
        }
    };
    // Screen-space ambient occlusion needs the ambient light's share of the image: the forward
    // pass's second output (left unused without it).
    const bool ssao = s.ssao && s.ssaoRadius > 0.0f && m_ssaoPipeline && m_ssaoApplyPipeline && !(useRt && s.rayTracedAo) &&
                      (s.debugView == DebugView::Lit || s.debugView == DebugView::AmbientOcclusion);
    // Without SSAO the slot stays empty, or (GPUs that cannot leave it empty) gets a texture nobody reads.
    const RGTexture ambientTex = ssao || !m_device.caps().unusedAttachments
                                     ? graph.create({extent.width, extent.height, kHdrFormat, "ambient"})
                                     : RGTexture{};
    // TAA needs the motion vectors: the third output (left unused, or a texture nobody reads, without it).
    const RGTexture motionTex = taa || !m_device.caps().unusedAttachments
                                    ? graph.create({extent.width, extent.height, kMotionFormat, "motion"})
                                    : RGTexture{};
    // Depth prepass: the GPU-culled groups (not cut-outs) draw their depth with the same commands as
    // the forward pass's first phase; the forward pass then loads it and tests LessEqual, so hidden
    // fragments are rejected before shading.
    bool prepassed = false;
    if (s.depthPrepass && !wireframe && m_prepassPipeline[0] &&
        std::any_of(cullGroups.begin(), cullGroups.end(), [](const CullGroup& g) { return g.prepass != 0; })) {
        prepassed = true;
        const u64 prepassFrame = frameAlloc.gpu, prepassInstances = instAlloc.gpu;
        graph.addPass("Depth prepass").depth(depthTex).execute([&, prepassFrame, prepassInstances](RenderPassContext& ctx) {
            gpu::DrawPush push{};
            push.frame = prepassFrame;
            push.instances = prepassInstances;
            push.visible = cullBase + offVisible;
            u32 bound = ~0u;
            BufferHandle boundIndex{};
            for (u32 g = 0; g < cullGroups.size(); ++g) {
                const CullGroup& group = cullGroups[g];
                if (group.prepass == 0) continue; // cut-outs: the material decides what is cut
                const u32 variant = group.prepass == 2 ? 1u : 0u;
                if (variant != bound) {
                    ctx.cmd.bindPipeline(m_prepassPipeline[variant]);
                    bound = variant;
                }
                const GpuMesh& mesh = m_meshes[group.mesh];
                if (!(mesh.indexBuffer == boundIndex)) {
                    ctx.cmd.bindIndexBuffer(mesh.indexBuffer);
                    boundIndex = mesh.indexBuffer;
                }
                push.vertices = m_device.deviceAddress(mesh.vertexBuffer);
                ctx.cmd.pushConstants(push);
                ctx.cmd.drawIndexedIndirect(cullBuffer, g * sizeof(gpu::DrawIndexedCommand), 1, sizeof(gpu::DrawIndexedCommand));
                ++m_stats.drawCalls;
            }
        });
    }
    {
        RenderPassBuilder forward = graph.addPass("Forward");
        forward.color(hdrTex, LoadOp::DontCare).color(ambientTex, LoadOp::Clear, StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f});
        forward.color(motionTex, LoadOp::Clear, StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f});
        forward.depth(depthTex, prepassed ? LoadOp::Load : LoadOp::Clear); // depth is kept for the debug lines
        if (shadowAtlas) forward.sample(shadowAtlas);
        if (pointShadowAtlas) forward.sample(pointShadowAtlas);
        forward.execute([&record](RenderPassContext& ctx) { record(ctx.cmd, false); });
    }
    if (lateCull) {
        // The pyramid of what the early pass drew, then the second culling phase and the late draws.
        const std::array<glm::uvec4, 16> levels = view.m_hizLevels;
        const u32 levelCount = view.m_hizLevelCount;
        graph.addPass("Hi-Z").sample(depthTex).sideEffect().execute(
            [this, depthTex, hizAddress, levels, levelCount, extent](RenderPassContext& ctx) {
                ctx.cmd.globalBarrier(); // the first phase's reads of last frame's pyramid
                ctx.cmd.bindPipeline(m_hizPipeline);
                for (u32 l = 0; l < levelCount; ++l) {
                    gpu::HizPush push{};
                    push.hiz = hizAddress;
                    push.depthTexture = ctx.bindlessIndex(depthTex);
                    push.fromDepth = l == 0 ? 1u : 0u;
                    push.srcOffset = l == 0 ? 0u : levels[l - 1].z;
                    push.dstOffset = levels[l].z;
                    push.srcSize = l == 0 ? glm::uvec2(extent.width, extent.height) : glm::uvec2(levels[l - 1]);
                    push.dstSize = glm::uvec2(levels[l]);
                    ctx.cmd.pushConstants(push);
                    ctx.cmd.dispatch((push.dstSize.x + 7) / 8, (push.dstSize.y + 7) / 8);
                    ctx.cmd.globalBarrier();
                }
            });
        const gpu::CullPush push{cullItems.gpu, cullBase + offCommandsLate, cullBase + offVisibleLate, cullParamsLate.gpu,
                                 cullBase + offCounters, hizAddress, cullBase + offStates,
                                 static_cast<u32>(cullEntries.size()), 2u};
        graph.addPass("GPU culling (late)").sideEffect().execute([=, this](RenderPassContext& ctx) {
            ctx.cmd.bindPipeline(m_cullPipeline);
            ctx.cmd.pushConstants(push);
            ctx.cmd.dispatch((push.count + 63) / 64);
            ctx.cmd.globalBarrier();
            if (readback) ctx.cmd.copyBuffer(cullBuffer, readback, 16, offCounters, 0);
        });
        RenderPassBuilder late = graph.addPass("Forward (late)");
        late.color(hdrTex, LoadOp::Load).color(ambientTex, LoadOp::Load).color(motionTex, LoadOp::Load).depth(depthTex, LoadOp::Load);
        if (shadowAtlas) late.sample(shadowAtlas);
        if (pointShadowAtlas) late.sample(pointShadowAtlas);
        late.execute([&record](RenderPassContext& ctx) { record(ctx.cmd, true); });
    }
    m_stats.instances += drawCount;
    {
        u32 sources = 0, last = ~0u;
        for (const WrapInstance& di : m_instances) sources += di.draw != last, last = di.draw;
        m_stats.wrapCopies += drawCount - sources;
    }

    // ---- SSAO: the occlusion from depth, then the image with its ambient share darkened by it
    RGTexture sceneColor = hdrTex;
    if (ssao) {
        // The occlusion at half resolution (a quarter of the pixels), blurred back up when applied.
        const RGTexture aoTex = graph.create({(extent.width + 1) / 2, (extent.height + 1) / 2, Format::R8_UNORM, "ssao"});
        const RGTexture applied = graph.create({extent.width, extent.height, kHdrFormat, "ssao applied"});
        gpu::SsaoPush push{};
        push.frame = frameAddress;
        push.texel = glm::vec2(1.0f / f32(extent.width), 1.0f / f32(extent.height));
        push.radius = s.ssaoRadius;
        push.intensity = std::max(s.ssaoIntensity, 0.0f);
        push.noiseFrame = taa ? static_cast<u32>(frameCounter) : 0u; // TAA averages a pattern that turns
        const PipelineHandle occlusionPipeline = m_ssaoPipeline, applyPipeline = m_ssaoApplyPipeline;
        graph.addPass("SSAO").sample(depthTex).color(aoTex, LoadOp::DontCare).execute([=](RenderPassContext& ctx) {
            gpu::SsaoPush p = push;
            p.depthTexture = ctx.bindlessIndex(depthTex);
            p.mode = 0;
            ctx.cmd.bindPipeline(occlusionPipeline);
            ctx.cmd.pushConstants(p);
            ctx.cmd.draw(3);
        });
        const u32 applyMode = s.debugView == DebugView::AmbientOcclusion ? 2u : 1u;
        graph.addPass("SSAO apply")
            .sample(hdrTex)
            .sample(ambientTex)
            .sample(aoTex)
            .sample(depthTex)
            .color(applied, LoadOp::DontCare)
            .execute([=](RenderPassContext& ctx) {
                gpu::SsaoPush p = push;
                p.depthTexture = ctx.bindlessIndex(depthTex);
                p.mode = applyMode;
                p.colorTexture = ctx.bindlessIndex(hdrTex);
                p.ambientTexture = ctx.bindlessIndex(ambientTex);
                p.aoTexture = ctx.bindlessIndex(aoTex);
                ctx.cmd.bindPipeline(applyPipeline);
                ctx.cmd.pushConstants(p);
                ctx.cmd.draw(3);
            });
        sceneColor = applied;
    }

    // ---- the ray traced reference view: the image traced instead (a ray tracing pipeline, one
    // ray generation per pixel into a storage image); the passes after it take it as the image
    if (rtView) {
        const RGTexture traced = graph.create({extent.width, extent.height, kHdrFormat, "ray traced"});
        const gpu::RtPush push{frameAddress, instAlloc.gpu, builtTlas, 0u, std::max(s.shadowBias, 1e-3f)};
        const PipelineHandle pipeline = m_rtPipeline;
        const Extent2D size = extent;
        graph.addPass("Ray traced").storage(traced).execute([=, this](RenderPassContext& ctx) {
            gpu::RtPush p = push;
            p.outputImage = m_device.storageIndex(ctx.texture(traced));
            ctx.cmd.bindPipeline(pipeline);
            ctx.cmd.pushConstants(p);
            ctx.cmd.traceRays(size.width, size.height);
        });
        sceneColor = traced;
    }

    // ---- the reference path tracer: one more path per pixel into the view's running average,
    // started over when anything it sees changes; the average is the image from here on
    if (ptView) {
        pathKey.add(viewProjNoJitter);
        for (int i = 0; i < 3; ++i) pathKey.add(scene.camera.origin.raw(i));
        pathKey.add(scene.sun);
        for (const PointLightData& l : scene.pointLights) { // field by field: the struct has padding
            pathKey.add(l.position);
            pathKey.add(l.radius);
            pathKey.add(l.color);
            pathKey.add(l.intensity);
            pathKey.add(l.castShadows);
        }
        if (!scene.joints.empty()) pathKey.add(scene.joints.data(), scene.joints.size() * sizeof(glm::mat4));
        pathKey.add(s.skyZenith);
        pathKey.add(s.skyHorizon);
        pathKey.add(s.groundColor);
        pathKey.add(s.pathTracerBounces);
        pathKey.add(extent);
        if (!view.m_accumulation) {
            view.m_accumulation = m_device.createTexture({extent.width, extent.height, 1, Format::RGBA32_FLOAT,
                                                          TextureUsage::Storage | TextureUsage::Sampled, "view.pathTracer"});
            view.m_pathSamples = 0;
        }
        if (pathKey.value != view.m_pathKey) {
            view.m_pathKey = pathKey.value;
            view.m_pathSamples = 0;
        }
        const RGTexture accumulation = graph.import("path traced", view.m_accumulation, extent, Format::RGBA32_FLOAT);
        if (view.m_pathSamples < std::max(s.pathTracerMaxSamples, 1u)) {
            gpu::RtPush push{frameAddress, instAlloc.gpu, builtTlas, 0u, std::max(s.shadowBias, 1e-3f)};
            push.sampleIndex = view.m_pathSamples++;
            push.bounces = std::min(s.pathTracerBounces, 32u);
            const PipelineHandle pipeline = m_ptPipeline;
            const Extent2D size = extent;
            graph.addPass("Path traced").storage(accumulation).execute([=, this](RenderPassContext& ctx) {
                gpu::RtPush p = push;
                p.outputImage = m_device.storageIndex(ctx.texture(accumulation));
                ctx.cmd.bindPipeline(pipeline);
                ctx.cmd.pushConstants(p);
                ctx.cmd.traceRays(size.width, size.height);
            });
        }
        m_stats.pathSamples = view.m_pathSamples;
        sceneColor = accumulation;
    }

    // ---- the game's passes before TAA (RenderStage::BeforeTaa): TAA averages their noise
    {
        RenderGraphFrame early{graph, scene, extent, sceneColor, depthTex, frameAddress, builtTlas, instAlloc.gpu};
        runGraphCallbacks(RenderStage::BeforeTaa, early);
        sceneColor = early.color;
    }

    // ---- TAA: this frame blended into the view's history (two images, written in turn); the
    // result is the image from here on (post-processing, bloom and the tonemap read it)
    if (taa) {
        for (TextureHandle& h : view.m_history)
            if (!h)
                h = m_device.createTexture({extent.width, extent.height, 1, kHdrFormat,
                                            TextureUsage::RenderTarget | TextureUsage::Sampled, "view.taaHistory"});
        const bool showMotion = s.debugView == DebugView::Motion;
        const bool historyValid = view.m_historyValid && view.m_taaFrame + 1 == frameCounter;
        const u32 write = view.m_historyIndex ^ 1u;
        const RGTexture previous = graph.import("taa history", view.m_history[view.m_historyIndex], extent, kHdrFormat);
        const RGTexture result = graph.import("taa result", view.m_history[write], extent, kHdrFormat);
        gpu::TaaPush push{};
        push.texel = glm::vec2(1.0f / f32(extent.width), 1.0f / f32(extent.height));
        push.feedback = std::clamp(s.taaFeedback, 0.01f, 1.0f);
        push.mode = showMotion ? 2u : historyValid ? 0u : 1u;
        const PipelineHandle pipeline = m_taaPipeline;
        const RGTexture source = sceneColor;
        RenderPassBuilder resolve = graph.addPass("TAA");
        resolve.sample(source).sample(motionTex).sample(depthTex);
        if (historyValid) resolve.sample(previous);
        resolve.color(result, LoadOp::DontCare).execute([=](RenderPassContext& ctx) {
            gpu::TaaPush p = push;
            p.color = ctx.bindlessIndex(source);
            p.velocity = ctx.bindlessIndex(motionTex);
            p.depth = ctx.bindlessIndex(depthTex);
            p.history = historyValid ? ctx.bindlessIndex(previous) : p.color;
            ctx.cmd.bindPipeline(pipeline);
            ctx.cmd.pushConstants(p);
            ctx.cmd.draw(3);
        });
        sceneColor = result;
        view.m_historyIndex = write;
        view.m_historyValid = !showMotion; // the motion view is no history to build on
        view.m_taaFrame = frameCounter;
    }

    RenderGraphFrame frame{graph, scene, extent, sceneColor, depthTex, frameAddress, builtTlas, instAlloc.gpu};
    runGraphCallbacks(RenderStage::AfterOpaque, frame);

    // ---- post-process: each pass reads the image so far and writes a new transient one (the
    // graph alternates two textures)
    for (const PostProcessDesc& fx : s.postProcess) {
        if (!fx.enabled) continue;
        PipelineHandle p = postProcessPipeline(fx.shader);
        if (!p) continue;
        const RGTexture source = frame.color;
        const RGTexture target = graph.create({extent.width, extent.height, kHdrFormat, "post-process"});
        const glm::vec4 params = fx.params;
        graph.addPass(fx.name.empty() ? "Post-process" : fx.name)
            .sample(source)
            .color(target, LoadOp::DontCare)
            .execute([p, source, frameAddress, extent, params](RenderPassContext& ctx) {
                ctx.cmd.bindPipeline(p);
                gpu::PostProcessPush pushPost{};
                pushPost.frame = frameAddress;
                pushPost.inputTexture = ctx.bindlessIndex(source);
                pushPost.texelSize = glm::vec2(1.0f / f32(extent.width), 1.0f / f32(extent.height));
                pushPost.params = params;
                ctx.cmd.pushConstants(pushPost);
                ctx.cmd.draw(3);
            });
        frame.color = target;
        ++m_stats.postProcessPasses;
    }
    runGraphCallbacks(RenderStage::BeforeTonemap, frame);

    // ---- bloom: down a chain of half-size images, then back up adding each level (transients)
    RGTexture bloomTex;
    if (s.bloom && s.bloomStrength > 0.0f && m_bloomPipeline) {
        std::vector<RGTexture> down;
        std::vector<Extent2D> sizes;
        u32 w = extent.width, h = extent.height;
        for (u32 l = 0; l < std::clamp(s.bloomLevels, 1u, 10u) && w >= 4 && h >= 4; ++l) {
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
            sizes.push_back({w, h});
            down.push_back(graph.create({w, h, kHdrFormat, "bloom down"}));
        }
        const PipelineHandle bloom = m_bloomPipeline;
        auto pass = [&](const char* name, RGTexture target, RGTexture source, RGTexture lower, u32 mode, glm::vec2 texel) {
            RenderPassBuilder b = graph.addPass(name);
            b.sample(source);
            if (lower) b.sample(lower);
            const f32 threshold = s.bloomThreshold, radius = s.bloomRadius;
            b.color(target, LoadOp::DontCare).execute([=](RenderPassContext& ctx) {
                ctx.cmd.bindPipeline(bloom);
                ctx.cmd.pushConstants(gpu::BloomPush{ctx.bindlessIndex(source), lower ? ctx.bindlessIndex(lower) : 0u, mode,
                                                     threshold, texel, radius, 0.5f});
                ctx.cmd.draw(3);
            });
        };
        if (!down.empty()) {
            RGTexture source = frame.color;
            Extent2D sourceSize = extent;
            for (size_t l = 0; l < down.size(); ++l) {
                pass("Bloom down", down[l], source, RGTexture{}, l == 0 ? 0u : 1u,
                     glm::vec2(1.0f / f32(sourceSize.width), 1.0f / f32(sourceSize.height)));
                source = down[l];
                sourceSize = sizes[l];
            }
            RGTexture up = down.back();
            for (size_t l = down.size() - 1; l-- > 0;) {
                const RGTexture next = graph.create({sizes[l].width, sizes[l].height, kHdrFormat, "bloom up"});
                pass("Bloom up", next, down[l], up, 2u,
                     glm::vec2(1.0f / f32(sizes[l + 1].width), 1.0f / f32(sizes[l + 1].height)));
                up = next;
            }
            bloomTex = up;
        }
    }

    // ---- auto exposure: a histogram of the image's luminance, then its average eased toward
    // (exposure.comp), into the view's state that the tonemap reads
    u64 exposureAddress = 0;
    if (s.autoExposure && m_exposurePipeline) {
        if (!view.m_exposure) {
            view.m_exposure = m_device.createBuffer({sizeof(gpu::ExposureState), BufferUsage::Storage | BufferUsage::TransferDst |
                                                                                     BufferUsage::TransferSrc,
                                                     MemoryUsage::GpuOnly, "view.exposure"});
            view.m_exposureValid = false;
        }
        const f64 now = std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const f64 dt = view.m_exposureValid ? std::clamp(now - view.m_exposureTime, 0.0, 0.5) : 0.0;
        FrameAllocation reset; // a new state: empty histogram, no average yet
        if (!view.m_exposureValid) {
            reset = allocate(sizeof(gpu::ExposureState), 16);
            if (reset.cpu) {
                gpu::ExposureState init{};
                init.adaptedLog2 = std::nanf("");
                init.exposure = 1.0f;
                std::memcpy(reset.cpu, &init, sizeof(init));
            }
        }
        if (view.m_exposureValid || reset.cpu) {
            view.m_exposureValid = true;
            view.m_exposureTime = now;
            exposureAddress = m_device.deviceAddress(view.m_exposure);
            gpu::ExposurePush push{};
            push.state = exposureAddress;
            push.size = glm::uvec2(extent.width, extent.height);
            push.minLog2 = -12.0f;
            push.rangeLog2 = 24.0f; // luminance from 2^-12 to 2^12
            push.adapt = s.autoExposureSpeed > 0.0f ? static_cast<f32>(1.0 - std::exp(-dt * s.autoExposureSpeed)) : 1.0f;
            push.key = std::max(s.autoExposureKey, 1e-4f);
            push.minExposure = std::max(s.autoExposureMin, 1e-4f);
            push.maxExposure = std::max(s.autoExposureMax, push.minExposure);
            push.lowPercentile = 0.4f;
            push.highPercentile = 0.95f;
            const RGTexture source = frame.color;
            const BufferHandle state = view.m_exposure, upload = m_frameBuffers[m_device.frameIndex()];
            const BufferHandle exposureReadback = m_exposureReadback.empty() ? BufferHandle{} : m_exposureReadback[m_device.frameIndex()];
            const u64 resetSource = reset.cpu ? reset.offset : ~0ull;
            graph.addPass("Auto exposure").sample(source).sideEffect().execute([=, this](RenderPassContext& ctx) {
                ctx.cmd.globalBarrier(); // last frame's tonemap read the state
                if (resetSource != ~0ull) {
                    ctx.cmd.copyBuffer(upload, state, sizeof(gpu::ExposureState), resetSource, 0);
                    ctx.cmd.globalBarrier();
                }
                ctx.cmd.bindPipeline(m_exposurePipeline);
                gpu::ExposurePush p = push;
                p.hdrTexture = ctx.bindlessIndex(source);
                p.mode = 0;
                ctx.cmd.pushConstants(p);
                ctx.cmd.dispatch((p.size.x + 15) / 16, (p.size.y + 15) / 16);
                ctx.cmd.globalBarrier();
                p.mode = 1;
                ctx.cmd.pushConstants(p);
                ctx.cmd.dispatch(1);
                ctx.cmd.globalBarrier(); // before the tonemap reads it
                if (exposureReadback) ctx.cmd.copyBuffer(state, exposureReadback, 4, offsetof(gpu::ExposureState, exposure), 0);
            });
        }
    }

    // ---- tonemap (into an RGBA8 transient when FXAA follows), then FXAA into the output
    const bool fxaa = s.antiAliasing == AntiAliasing::FXAA;
    const RGTexture tonemapTarget =
        fxaa ? graph.create({extent.width, extent.height, Format::RGBA8_UNORM, "tonemapped"}) : outputTex;
    {
        const RGTexture source = frame.color;
        const PipelineHandle tonemap = tonemapPipeline(fxaa ? Format::RGBA8_UNORM : m_device.backbufferFormat());
        const f32 exposure = s.exposure, bloomStrength = std::clamp(s.bloomStrength, 0.0f, 1.0f);
        const u32 op = static_cast<u32>(s.tonemapper);
        RenderPassBuilder tm = graph.addPass("Tonemap");
        tm.sample(source);
        if (bloomTex) tm.sample(bloomTex);
        tm.color(tonemapTarget, LoadOp::DontCare)
            .execute([tonemap, source, exposure, op, bloomTex, bloomStrength, exposureAddress](RenderPassContext& ctx) {
                ctx.cmd.bindPipeline(tonemap);
                gpu::TonemapPush push{ctx.bindlessIndex(source), exposure, op, bloomTex ? ctx.bindlessIndex(bloomTex) : ~0u,
                                      bloomTex ? bloomStrength : 0.0f};
                push.autoExposure = exposureAddress;
                ctx.cmd.pushConstants(push);
                ctx.cmd.draw(3);
            });
    }
    if (fxaa) {
        const PipelineHandle pipeline = fxaaPipeline(m_device.backbufferFormat());
        const gpu::FxaaPush push{0u, std::clamp(s.fxaaSubpixel, 0.0f, 1.0f), 0.166f, 0.0833f,
                                 glm::vec2(1.0f / f32(extent.width), 1.0f / f32(extent.height))};
        graph.addPass("FXAA").sample(tonemapTarget).color(outputTex, LoadOp::DontCare).execute([=](RenderPassContext& ctx) {
            gpu::FxaaPush p = push;
            p.source = ctx.bindlessIndex(tonemapTarget);
            ctx.cmd.bindPipeline(pipeline);
            ctx.cmd.pushConstants(p);
            ctx.cmd.draw(3);
        });
    }
    frame.color = outputTex;
    runGraphCallbacks(RenderStage::AfterTonemap, frame);

    if (m_debugDraw.lineCount() > 0) {
        const GlobalPosition origin = scene.camera.origin;
        graph.addPass("Debug lines")
            .color(outputTex, LoadOp::Load)
            .depth(depthTex, LoadOp::Load, StoreOp::DontCare)
            .execute([this, frameAddress, origin](RenderPassContext& ctx) { drawDebugLines(ctx.cmd, frameAddress, origin); });
    }

    // ---- capture (captureNextFrame): the output copied into a buffer, written next frame
    if (!m_capturePath.empty() && !m_pendingCapture) {
        const Format format = m_device.backbufferFormat(); // the output's format
        const bool fourBytes = format == Format::RGBA8_UNORM || format == Format::RGBA8_SRGB ||
                               format == Format::BGRA8_UNORM || format == Format::BGRA8_SRGB;
        if (fourBytes) {
            PendingCapture c;
            c.buffer = m_device.createBuffer({u64(extent.width) * extent.height * 4, BufferUsage::TransferDst,
                                              MemoryUsage::CpuToGpu, "renderer.capture"});
            c.extent = extent;
            c.bgra = format == Format::BGRA8_UNORM || format == Format::BGRA8_SRGB;
            c.path = std::move(m_capturePath);
            const BufferHandle buffer = c.buffer;
            graph.addPass("Capture").sideEffect().execute([outputTex, buffer](RenderPassContext& ctx) {
                ctx.cmd.copyTextureToBuffer(ctx.texture(outputTex), buffer);
            });
            m_pendingCapture = std::move(c);
        } else {
            NOVA_CORE_WARN("Renderer: cannot capture this output format");
        }
        m_capturePath.clear();
    }

    graph.compile();
    cmd.beginGpuTimer("Render"); // the view's whole graph; each pass nested in it
    graph.execute(cmd, *m_graphPool);
    cmd.endGpuTimer();
    m_stats.graphPasses += static_cast<u32>(graph.passes().size()) - graph.culledCount();
    m_stats.graphCulled += graph.culledCount();
    m_stats.transientTextures += graph.transientCount();
    m_stats.pooledTextures = m_graphPool->size();
    m_lastGraph = graph.describe();
    m_stats.cpuMs += std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();
}

PipelineHandle Renderer::linePipeline(Format format, bool onTop) {
    const u32 key = static_cast<u32>(format) | (onTop ? 0x100u : 0u);
    auto it = m_linePipelines.find(key);
    if (it != m_linePipelines.end()) return it->second;
    GraphicsPipelineDesc desc;
    desc.vertex = m_lineVS;
    desc.fragment = m_lineFS;
    desc.colorFormats = {format};
    desc.depthFormat = kDepthFormat;
    desc.topology = PrimitiveTopology::LineList;
    desc.cullMode = CullMode::None;
    desc.depthTest = !onTop;
    desc.depthWrite = false;
    desc.depthCompare = CompareOp::LessEqual;
    desc.blend = BlendMode::Alpha;
    desc.debugName = onTop ? "pipeline.debugLines.onTop" : "pipeline.debugLines";
    PipelineHandle p = m_device.createGraphicsPipeline(desc);
    m_linePipelines.emplace(key, p);
    return p;
}

void Renderer::drawDebugLines(CommandList& cmd, u64 frameAddress, const GlobalPosition& origin) {
    struct LinePush {
        u64 frame;
        u64 vertices;
        glm::vec4 offset; // DebugDraw origin − frame origin (lines are recorded relative to the former)
    };
    const glm::vec4 offset(glm::vec3((m_debugDraw.origin() - origin).toMeters()), 0.0f);
    for (bool onTop : {false, true}) {
        const auto& verts = m_debugDraw.vertices(onTop);
        if (verts.empty()) continue;
        const u64 bytes = verts.size() * sizeof(DebugDraw::Vertex);
        FrameAllocation alloc = allocate(bytes, 16);
        if (!alloc.cpu) {
            NOVA_CORE_WARN("Renderer: too many debug lines for the frame upload buffer");
            break;
        }
        std::memcpy(alloc.cpu, verts.data(), bytes);
        cmd.bindPipeline(linePipeline(m_device.backbufferFormat(), onTop)); // outputs use the swapchain format
        cmd.pushConstants(LinePush{frameAddress, alloc.gpu, offset});
        cmd.draw(static_cast<u32>(verts.size()));
    }
}

// ------------------------------------------------------------ graph hooks --
u32 Renderer::addRenderPasses(RenderStage stage, RenderGraphCallback callback) {
    const u32 id = m_nextGraphCallback++;
    m_graphCallbacks.push_back({id, stage, std::move(callback)});
    return id;
}

void Renderer::removeRenderPasses(u32 id) {
    std::erase_if(m_graphCallbacks, [id](const GraphCallback& c) { return c.id == id; });
}

void Renderer::runGraphCallbacks(RenderStage stage, RenderGraphFrame& frame) {
    for (const GraphCallback& c : m_graphCallbacks)
        if (c.stage == stage && c.callback) c.callback(frame);
}

} // namespace nova
