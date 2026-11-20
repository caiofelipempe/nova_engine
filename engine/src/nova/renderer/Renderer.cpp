#include "nova/renderer/Renderer.h"

#include "nova/core/Log.h"
#include "nova/platform/FileSystem.h"
#include "nova/renderer/GpuTypes.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
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
        {m_size.width, m_size.height, 1, Renderer::kDepthFormat, TextureUsage::DepthStencil, "view.depth"});
    if (m_withOutput)
        m_output = d.createTexture({m_size.width, m_size.height, 1, d.backbufferFormat(),
                                    TextureUsage::RenderTarget | TextureUsage::Sampled, "view.output"});
}

void RenderView::destroy() {
    Device& d = m_renderer.device();
    d.destroyTexture(m_hdr);
    d.destroyTexture(m_depth);
    if (m_output) d.destroyTexture(m_output);
    m_hdr = m_depth = m_output = {};
}

// ---------------------------------------------------------------- Renderer --
Renderer::Renderer(Device& device) : m_device(device) {
    createPipelines();

    const u32 frames = device.caps().framesInFlight;
    BufferUsage usage = BufferUsage::Storage;
    if (device.caps().accelerationStructures) usage |= BufferUsage::AccelStructInput;
    for (u32 i = 0; i < frames; ++i) {
        m_frameBuffers.push_back(
            device.createBuffer({kFrameBufferSize, usage, MemoryUsage::CpuToGpu, "renderer.frameUpload"}));
        if (device.caps().accelerationStructures)
            m_tlas.push_back(device.createTLAS({kMaxInstances, "renderer.tlas"}));
    }

    m_cube = createMesh(MeshData::cube(), "Cube");
    m_sphere = createMesh(MeshData::sphere(), "Sphere");
    m_plane = createMesh(MeshData::plane(1.0f, 1), "Plane");
}

Renderer::~Renderer() {
    m_device.waitIdle();
    m_backbufferView.reset();
    for (u32 i = 0; i < m_meshes.size(); ++i)
        if (m_meshes[i].alive) destroyMesh({i});
    for (TextureHandle t : m_textures) m_device.destroyTexture(t);
    for (BufferHandle b : m_frameBuffers) m_device.destroyBuffer(b);
    for (AccelStructHandle t : m_tlas) m_device.destroyAccelStruct(t);
    for (PipelineHandle p : {m_skyPipeline, m_forwardPipeline, m_forwardRtPipeline, m_wireframePipeline})
        if (p) m_device.destroyPipeline(p);
    for (auto& [fmt, p] : m_tonemapPipelines) m_device.destroyPipeline(p);
    for (auto& [key, p] : m_linePipelines) m_device.destroyPipeline(p);
    m_device.waitIdle();
}

void Renderer::createPipelines() {
    m_fullscreenVS = loadShader("fullscreen.vert");
    m_tonemapFS = loadShader("tonemap.frag");
    m_lineVS = loadShader("debug_line.vert");
    m_lineFS = loadShader("debug_line.frag");
    ShaderDesc forwardVS = loadShader("forward.vert");

    GraphicsPipelineDesc sky;
    sky.vertex = m_fullscreenVS;
    sky.fragment = loadShader("sky.frag");
    sky.colorFormats = {kHdrFormat};
    sky.depthFormat = kDepthFormat;
    sky.cullMode = CullMode::None;
    sky.depthTest = false;
    sky.depthWrite = false;
    sky.debugName = "pipeline.sky";
    m_skyPipeline = m_device.createGraphicsPipeline(sky);

    GraphicsPipelineDesc fwd;
    fwd.vertex = forwardVS;
    fwd.fragment = loadShader("forward.frag");
    fwd.colorFormats = {kHdrFormat};
    fwd.depthFormat = kDepthFormat;
    fwd.debugName = "pipeline.forward";
    m_forwardPipeline = m_device.createGraphicsPipeline(fwd);

    if (m_device.caps().wireframe) {
        GraphicsPipelineDesc wire = fwd;
        wire.polygonMode = PolygonMode::Line;
        wire.cullMode = CullMode::None;
        wire.debugName = "pipeline.wireframe";
        m_wireframePipeline = m_device.createGraphicsPipeline(wire);
    }

    if (m_device.caps().rayQuery) {
        GraphicsPipelineDesc rt = fwd;
        rt.fragment = loadShader("forward_rq.frag");
        rt.debugName = "pipeline.forward.rayquery";
        m_forwardRtPipeline = m_device.createGraphicsPipeline(rt);
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

void Renderer::destroyMesh(MeshHandle h) {
    if (h.id >= m_meshes.size() || !m_meshes[h.id].alive) return;
    GpuMesh& m = m_meshes[h.id];
    m_device.destroyBuffer(m.vertexBuffer);
    m_device.destroyBuffer(m.indexBuffer);
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
    TextureHandle t = m_device.createTexture(
        {width, height, 1, Format::RGBA8_SRGB, TextureUsage::Sampled | TextureUsage::TransferDst, name});
    m_device.uploadTexture(t, rgba8, u64(width) * height * 4);
    m_textures.push_back(t);
    return t;
}

Scope<RenderView> Renderer::createView(Extent2D size) {
    return Scope<RenderView>(new RenderView(*this, size, true));
}

void Renderer::beginFrame() {
    m_frameOffset = 0;
    m_lastStats = m_stats; // UI code runs before rendering, so expose the previous frame's numbers
    m_stats = {};
    m_debugDraw.clear();
}

Renderer::FrameAllocation Renderer::allocate(u64 size, u64 alignment) {
    u64 offset = (m_frameOffset + alignment - 1) & ~(alignment - 1);
    if (offset + size > kFrameBufferSize) return {};
    m_frameOffset = offset + size;
    BufferHandle buf = m_frameBuffers[m_device.frameIndex()];
    return {static_cast<u8*>(m_device.mappedPointer(buf)) + offset, m_device.deviceAddress(buf) + offset};
}

void Renderer::render(CommandList& cmd, const RenderScene& scene, RenderView& view) {
    renderInternal(cmd, scene, view.m_hdr, view.m_depth, view.m_output);
    cmd.transition(view.m_output, ResourceState::ShaderRead);
}

void Renderer::renderToBackbuffer(CommandList& cmd, const RenderScene& scene) {
    Extent2D size = m_device.backbufferExtent();
    if (!m_backbufferView) m_backbufferView.reset(new RenderView(*this, size, false));
    else m_backbufferView->resize(size);
    renderInternal(cmd, scene, m_backbufferView->m_hdr, m_backbufferView->m_depth, m_device.backbuffer());
}

void Renderer::renderInternal(CommandList& cmd, const RenderScene& scene, TextureHandle hdr, TextureHandle depth,
                              TextureHandle output) {
    const Extent2D extent = m_device.textureExtent(hdr);
    const RenderSettings& s = m_settings;

    // ---- per-frame constants
    auto frameAlloc = allocate(sizeof(gpu::FrameData));
    const u32 drawCount = std::min<u32>(static_cast<u32>(scene.draws.size()), kMaxInstances);
    auto instAlloc = allocate(std::max<u64>(1, drawCount) * sizeof(gpu::InstanceData));
    if (!frameAlloc.cpu || !instAlloc.cpu) {
        NOVA_CORE_ERROR("Renderer: per-frame upload buffer exhausted");
        return;
    }

    const bool useRt = s.rayTracedShadows && m_device.caps().rayQuery && !m_tlas.empty();

    auto& fd = *static_cast<gpu::FrameData*>(frameAlloc.cpu);
    fd.view = scene.camera.view;
    fd.proj = scene.camera.projection;
    fd.viewProj = fd.proj * fd.view;
    fd.invViewProj = glm::inverse(fd.viewProj);
    fd.cameraPos = glm::vec4(scene.camera.position, scene.time);
    fd.sunDirection = glm::vec4(glm::normalize(scene.sun.direction), scene.sun.intensity);
    fd.sunColor = glm::vec4(scene.sun.color, 0.0f);
    fd.skyZenith = glm::vec4(s.skyZenith, s.ambientIntensity);
    fd.skyHorizon = glm::vec4(s.skyHorizon, 0.0f);
    fd.groundColor = glm::vec4(s.groundColor, 0.0f);
    fd.pointLightCount = std::min<u32>(static_cast<u32>(scene.pointLights.size()), gpu::kMaxPointLights);
    fd.flags = useRt ? gpu::kFrameFlagRtShadows : 0u;
    fd.shadowBias = s.shadowBias;
    for (u32 i = 0; i < fd.pointLightCount; ++i) {
        const PointLightData& l = scene.pointLights[i];
        fd.pointLights[i] = {glm::vec4(l.position, l.radius), glm::vec4(l.color, l.intensity)};
    }

    // ---- instances (+ TLAS records)
    auto* instances = static_cast<gpu::InstanceData*>(instAlloc.cpu);
    AccelStructInstance* tlasRecords = nullptr;
    FrameAllocation tlasAlloc;
    if (useRt) {
        tlasAlloc = allocate(std::max<u64>(1, drawCount) * sizeof(AccelStructInstance), 16);
        tlasRecords = static_cast<AccelStructInstance*>(tlasAlloc.cpu);
    }
    u32 tlasCount = 0;
    for (u32 i = 0; i < drawCount; ++i) {
        const DrawItem& d = scene.draws[i];
        gpu::InstanceData& inst = instances[i];
        inst.model = d.transform;
        inst.normalMatrix = glm::transpose(glm::inverse(d.transform));
        inst.baseColor = d.material.baseColor;
        inst.emissive = glm::vec4(d.material.emissive, d.material.emissiveStrength);
        u32 flags = d.material.checker ? gpu::kMaterialFlagChecker : 0u;
        f32 flagsAsFloat;
        std::memcpy(&flagsAsFloat, &flags, sizeof(f32));
        const u32 tex = d.material.baseColorTexture ? m_device.bindlessIndex(d.material.baseColorTexture) : ~0u;
        inst.params = glm::vec4(d.material.metallic, d.material.roughness, flagsAsFloat,
                                tex == ~0u ? -1.0f : static_cast<f32>(tex));

        if (tlasRecords && d.castShadows && d.mesh.id < m_meshes.size() && m_meshes[d.mesh.id].alive &&
            m_meshes[d.mesh.id].blas) {
            AccelStructInstance& r = tlasRecords[tlasCount++];
            toInstanceTransform(d.transform, r.transform);
            r.instanceCustomIndex = i;
            r.mask = 0xFF;
            r.sbtRecordOffset = 0;
            r.flags = AccelInstanceTriangleCullDisable;
            r.blasAddress = m_device.accelStructAddress(m_meshes[d.mesh.id].blas);
        }
    }

    u64 tlasAddress = 0;
    if (useRt) {
        AccelStructHandle tlas = m_tlas[m_device.frameIndex()];
        cmd.beginDebugLabel("TLAS build");
        cmd.buildTLAS(tlas, tlasAlloc.gpu, tlasCount);
        cmd.endDebugLabel();
        tlasAddress = m_device.accelStructAddress(tlas);
        m_stats.tlasInstances += tlasCount;
    }

    // ---- sky + opaque
    cmd.beginDebugLabel("Forward");
    RenderingDesc pass;
    pass.colors.push_back({hdr, LoadOp::DontCare, StoreOp::Store});
    // Depth is kept for the debug line pass (depth-tested lines after tonemapping).
    pass.depth = DepthAttachment{depth, LoadOp::Clear, StoreOp::Store, 1.0f};
    cmd.beginRendering(pass);

    cmd.bindPipeline(m_skyPipeline);
    cmd.pushConstants(gpu::SkyPush{frameAlloc.gpu});
    cmd.draw(3);

    PipelineHandle pipeline = m_forwardPipeline;
    if (s.wireframe && m_wireframePipeline) pipeline = m_wireframePipeline;
    else if (useRt && m_forwardRtPipeline) pipeline = m_forwardRtPipeline;
    cmd.bindPipeline(pipeline);

    gpu::DrawPush push{};
    push.frame = frameAlloc.gpu;
    push.instances = instAlloc.gpu;
    push.tlas = tlasAddress;
    push.debugView = static_cast<u32>(s.debugView);
    BufferHandle boundIndex{};
    for (u32 i = 0; i < drawCount; ++i) {
        const DrawItem& d = scene.draws[i];
        if (d.mesh.id >= m_meshes.size() || !m_meshes[d.mesh.id].alive) continue;
        const GpuMesh& mesh = m_meshes[d.mesh.id];
        if (!(mesh.indexBuffer == boundIndex)) {
            cmd.bindIndexBuffer(mesh.indexBuffer);
            boundIndex = mesh.indexBuffer;
        }
        push.vertices = m_device.deviceAddress(mesh.vertexBuffer);
        push.instanceIndex = i;
        cmd.pushConstants(push);
        cmd.drawIndexed(mesh.indexCount);
        ++m_stats.drawCalls;
        m_stats.triangles += mesh.indexCount / 3;
    }
    m_stats.instances += drawCount;
    cmd.endRendering();
    cmd.endDebugLabel();

    // ---- tonemap
    cmd.beginDebugLabel("Tonemap");
    cmd.transition(hdr, ResourceState::ShaderRead);
    RenderingDesc tm;
    tm.colors.push_back({output, LoadOp::DontCare, StoreOp::Store});
    cmd.beginRendering(tm);
    cmd.bindPipeline(tonemapPipeline(m_device.backbufferFormat()));
    cmd.pushConstants(gpu::TonemapPush{m_device.bindlessIndex(hdr), s.exposure, static_cast<u32>(s.tonemapper)});
    cmd.draw(3);
    cmd.endRendering();
    cmd.endDebugLabel();

    if (m_debugDraw.lineCount() > 0) drawDebugLines(cmd, frameAlloc.gpu, depth, output);
    (void)extent;
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

void Renderer::drawDebugLines(CommandList& cmd, u64 frameAddress, TextureHandle depth, TextureHandle output) {
    struct LinePush {
        u64 frame;
        u64 vertices;
    };
    cmd.beginDebugLabel("Debug lines");
    RenderingDesc pass;
    pass.colors.push_back({output, LoadOp::Load, StoreOp::Store});
    pass.depth = DepthAttachment{depth, LoadOp::Load, StoreOp::DontCare, 1.0f};
    cmd.beginRendering(pass);
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
        cmd.pushConstants(LinePush{frameAddress, alloc.gpu});
        cmd.draw(static_cast<u32>(verts.size()));
    }
    cmd.endRendering();
    cmd.endDebugLabel();
}

} // namespace nova
