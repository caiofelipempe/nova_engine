// Headless tests of the render graph's compilation: which passes are dropped, dependencies,
// transient lifetimes, which transients share a pooled texture, and the usages they get.
#include <nova/Nova.h>
#include <nova/renderer/GpuTypes.h>

#include <cstdio>
#include <functional>
#include <vector>

using namespace nova;
using rhi::Format;
using rhi::LoadOp;
using rhi::ResourceState;
using rhi::TextureUsage;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAILED: %s (%s:%d)\n", expr, file, line);
    }
}
#define CHECK(x) check((x), #x, __FILE__, __LINE__)

const Extent2D kSize{640, 360};

void culling() {
    RenderGraph g;
    const RGTexture out = g.import("output", rhi::TextureHandle{}, kSize, Format::BGRA8_UNORM, ResourceState::ShaderRead);
    const RGTexture hdr = g.create({kSize.width, kSize.height, Format::RGBA16_FLOAT, "hdr"});
    const RGTexture unused = g.create({kSize.width, kSize.height, Format::RGBA16_FLOAT, "unused"});
    g.addPass("Scene").color(hdr);                     // 0: needed by the tonemap
    g.addPass("Nobody reads this").color(unused);       // 1: dropped
    g.addPass("Upload").sideEffect();                   // 2: kept (side effect)
    g.addPass("Reads the unused").sample(unused).color( // 3: writes a texture nobody reads: dropped
        g.create({kSize.width, kSize.height, Format::RGBA16_FLOAT, "also unused"}));
    g.addPass("Tonemap").sample(hdr).color(out, LoadOp::DontCare); // 4: writes the output
    g.compile();
    const auto& p = g.passes();
    CHECK(!p[0].culled && p[1].culled && !p[2].culled && p[3].culled && !p[4].culled);
    CHECK(g.culledCount() == 2);
    CHECK(p[4].dependsOn.size() == 1 && p[4].dependsOn[0] == 0);
    CHECK(p[3].dependsOn.size() == 1 && p[3].dependsOn[0] == 1); // depends, but nothing needs it
    // Only transients of kept passes get a texture.
    CHECK(g.transientCount() == 1 && g.textures()[hdr.id].slot != ~0u && g.textures()[unused.id].slot == ~0u);
    CHECK(g.describe().find("(dropped) Nobody reads this") != std::string::npos);
}

void loadsAndOutputs() {
    // A pass that loads an attachment needs the last pass that wrote it; markOutput keeps the
    // writers of a texture nothing reads (a backbuffer presented later).
    RenderGraph g;
    const RGTexture back = g.import("backbuffer", rhi::TextureHandle{}, kSize, Format::BGRA8_UNORM);
    const RGTexture depth = g.import("depth", rhi::TextureHandle{}, kSize, Format::D32_FLOAT);
    g.addPass("Forward").color(back).depth(depth);                                         // 0
    g.addPass("Overlay").color(back, LoadOp::Load).depth(depth, LoadOp::Load);             // 1
    g.compile();
    CHECK(g.culledCount() == 2); // not an output yet: nothing needs them
    g.markOutput(back);
    g.compile();
    CHECK(g.culledCount() == 0);
    CHECK(g.passes()[1].dependsOn.size() == 1 && g.passes()[1].dependsOn[0] == 0);
    // Clearing an attachment does not depend on what was there.
    RenderGraph h;
    const RGTexture t = h.import("t", rhi::TextureHandle{}, kSize, Format::RGBA8_UNORM, ResourceState::ShaderRead);
    h.addPass("A").color(t);
    h.addPass("B").color(t, LoadOp::Clear);
    h.compile();
    CHECK(h.passes()[1].dependsOn.empty() && !h.passes()[0].culled); // A writes the output too
}

void aliasing() {
    // A chain of post-process passes: each reads the previous image and writes a new one. Two
    // textures are enough, whatever the length of the chain.
    RenderGraph g;
    const RGTexture out = g.import("output", rhi::TextureHandle{}, kSize, Format::BGRA8_UNORM, ResourceState::ShaderRead);
    const RGTexture hdr = g.import("hdr", rhi::TextureHandle{}, kSize, Format::RGBA16_FLOAT);
    g.addPass("Forward").color(hdr);
    RGTexture color = hdr;
    std::vector<RGTexture> chain;
    for (int i = 0; i < 5; ++i) {
        const RGTexture next = g.create({kSize.width, kSize.height, Format::RGBA16_FLOAT, "post"});
        g.addPass("Post").sample(color).color(next, LoadOp::DontCare);
        chain.push_back(next);
        color = next;
    }
    g.addPass("Tonemap").sample(color).color(out, LoadOp::DontCare);
    g.compile();
    CHECK(g.transientCount() == 5 && g.slotCount() == 2);
    for (size_t i = 1; i < chain.size(); ++i) // neighbors overlap (one reads what the other writes)
        CHECK(g.textures()[chain[i].id].slot != g.textures()[chain[i - 1].id].slot);
    CHECK(g.textures()[chain[0].id].firstPass == 1 && g.textures()[chain[0].id].lastPass == 2);
    // The usages come from the passes: drawn into and sampled.
    CHECK(g.textures()[chain[0].id].usage == (TextureUsage::RenderTarget | TextureUsage::Sampled));

    // Different sizes or formats never share; a depth atlas is a depth target and sampled.
    RenderGraph h;
    const RGTexture o = h.import("output", rhi::TextureHandle{}, kSize, Format::BGRA8_UNORM, ResourceState::ShaderRead);
    const RGTexture atlas = h.create({2048, 2048, Format::D32_FLOAT, "shadow atlas"});
    const RGTexture half = h.create({320, 180, Format::RGBA16_FLOAT, "half"});
    const RGTexture full = h.create({640, 360, Format::RGBA16_FLOAT, "full"});
    h.addPass("Shadows").depth(atlas);
    h.addPass("Half").sample(atlas).color(half);
    h.addPass("Full").sample(half).color(full);
    h.addPass("Out").sample(full).color(o);
    h.compile();
    CHECK(h.slotCount() == 3);
    CHECK(h.textures()[atlas.id].usage == (TextureUsage::DepthStencil | TextureUsage::Sampled));
    // The states each use asks for.
    const auto& uses = h.passes()[1].uses;
    CHECK(uses[0].state() == ResourceState::ShaderRead && uses[1].state() == ResourceState::RenderTarget);
    CHECK(h.passes()[0].uses[0].state() == ResourceState::DepthWrite);
}

void reuse() {
    // clear() empties the graph for the next frame; compiling again gives the same result.
    RenderGraph g;
    for (int frame = 0; frame < 3; ++frame) {
        g.clear();
        const RGTexture out = g.import("output", rhi::TextureHandle{}, kSize, Format::BGRA8_UNORM, ResourceState::ShaderRead);
        const RGTexture a = g.create({kSize.width, kSize.height, Format::RGBA16_FLOAT, "a"});
        g.addPass("A").color(a);
        g.addPass("B").sample(a).color(out);
        g.compile();
        CHECK(g.passes().size() == 2 && g.culledCount() == 0 && g.slotCount() == 1);
    }
}

// The README's example of a game pass (compiled, not run: it needs a GPU).
[[maybe_unused]] void readmeExample(Renderer& renderer, ShaderHandle blur) {
    renderer.addRenderPasses(RenderStage::BeforeTonemap, [&renderer, blur](RenderGraphFrame& f) {
        rhi::PipelineHandle p = renderer.postProcessPipeline(blur);
        if (!p) return;
        const RGTexture src = f.color;
        const RGTexture out = f.graph.create({f.extent.width, f.extent.height, Renderer::kHdrFormat, "blur"});
        const u64 frame = f.frameData;
        f.graph.addPass("Blur").sample(src).color(out, rhi::LoadOp::DontCare).execute([=](RenderPassContext& ctx) {
            ctx.cmd.bindPipeline(p);
            gpu::PostProcessPush push{frame, ctx.bindlessIndex(src)};
            ctx.cmd.pushConstants(push);
            ctx.cmd.draw(3);
        });
        f.color = out;
    });
}

void unusedColorSlot() {
    // An invalid texture leaves a color slot unused (a pipeline's second output, not wanted this
    // frame): the pass still compiles and its other attachments keep their dependencies.
    RenderGraph g;
    const RGTexture out = g.import("output", rhi::TextureHandle{}, kSize, Format::BGRA8_UNORM, ResourceState::ShaderRead);
    const RGTexture hdr = g.create({kSize.width, kSize.height, Format::RGBA16_FLOAT, "hdr"});
    g.addPass("Forward").color(hdr).color(RGTexture{});
    g.addPass("Tonemap").sample(hdr).color(out, LoadOp::DontCare);
    g.compile();
    CHECK(g.culledCount() == 0 && g.transientCount() == 1);
    CHECK(g.passes()[0].raster() && g.passes()[0].uses.size() == 2);
    CHECK(g.describe().find("color (unused)") != std::string::npos);
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"passes nothing needs are dropped", culling},
        {"loaded attachments depend on their writer; outputs keep their writers", loadsAndOutputs},
        {"transients share textures when their lifetimes do not overlap", aliasing},
        {"a graph is rebuilt every frame", reuse},
        {"an invalid color texture leaves the slot unused", unusedColorSlot},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
