#pragma once

/// @file
/// @brief Render graph: a frame as passes that declare the textures they read and write.
///
/// The graph orders nothing by itself (passes run in the order they were added) but, from what
/// each pass declares, it:
/// - drops the passes nothing needs (no output, no later pass and no side effect depends on them);
/// - moves every texture to the state each pass needs (attachments, sampled, storage) and imported
///   textures to their final state at the end;
/// - creates the transient textures (RenderGraph::create) from a pool kept across frames, and lets
///   transients of the same size and format whose lifetimes do not overlap share one texture.
///
/// @code
/// RenderGraph& g = frame.graph;
/// RGTexture blurred = g.create({frame.extent.width, frame.extent.height, Renderer::kHdrFormat, "blur"});
/// g.addPass("Blur")
///     .sample(frame.color)
///     .color(blurred, rhi::LoadOp::DontCare)
///     .execute([=](RenderPassContext& ctx) {
///         ctx.cmd.bindPipeline(...);
///         ctx.cmd.pushConstants(BlurPush{ctx.bindlessIndex(frame.color)});
///         ctx.cmd.draw(3);
///     });
/// frame.color = blurred; // later passes (and the tonemap) read the blurred image
/// @endcode

#include "nova/rhi/Device.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nova {

/// @brief A texture of a RenderGraph (an index into it; valid for that graph's frame only).
struct RGTexture {
    u32 id = ~0u;
    explicit operator bool() const { return id != ~0u; }
    bool operator==(const RGTexture& o) const { return id == o.id; }
    bool operator!=(const RGTexture& o) const { return id != o.id; }
};

/// @brief A transient texture: created by the graph for this frame, from its pool.
struct RGTextureDesc {
    u32 width = 1;
    u32 height = 1;
    rhi::Format format = rhi::Format::RGBA16_FLOAT;
    const char* name = "transient";
    /// @brief Usages beyond those the passes imply (attachments, sampled, storage are added by the graph).
    rhi::TextureUsage usage = rhi::TextureUsage::None;
};

class RenderGraph;
class RenderGraphPool;

/// @brief What a pass's execute function gets: the command list and the real textures.
class RenderPassContext {
public:
    rhi::CommandList& cmd;
    /// @brief The texture behind a graph texture this frame.
    rhi::TextureHandle texture(RGTexture t) const;
    /// @brief Its bindless index (sampled textures).
    u32 bindlessIndex(RGTexture t) const;
    /// @brief Its size in pixels.
    Extent2D extent(RGTexture t) const;

private:
    friend class RenderGraph;
    RenderPassContext(rhi::CommandList& c, const RenderGraph& g, rhi::Device* d) : cmd(c), m_graph(g), m_device(d) {}
    const RenderGraph& m_graph;
    rhi::Device* m_device;
};

/// @brief Declares what a pass touches (RenderGraph::addPass) and what it does.
class RenderPassBuilder {
public:
    /// @brief Draws into `t` as a color attachment (the graph begins and ends the rendering).
    ///        Attachments are numbered in the order they are declared; an invalid `t` (RGTexture{})
    ///        leaves that slot unused: the pipeline's writes to it are discarded (needs
    ///        rhi::DeviceCaps::unusedAttachments).
    RenderPassBuilder& color(RGTexture t, rhi::LoadOp load = rhi::LoadOp::Clear, rhi::StoreOp store = rhi::StoreOp::Store,
                             std::array<f32, 4> clear = {0.0f, 0.0f, 0.0f, 1.0f});
    /// @brief Uses `t` as the depth attachment.
    RenderPassBuilder& depth(RGTexture t, rhi::LoadOp load = rhi::LoadOp::Clear, rhi::StoreOp store = rhi::StoreOp::Store,
                             f32 clear = 1.0f);
    /// @brief Samples `t` in its shaders (ShaderRead before the pass).
    RenderPassBuilder& sample(RGTexture t);
    /// @brief Reads and writes `t` as a storage image (UnorderedAccess before the pass).
    RenderPassBuilder& storage(RGTexture t);
    /// @brief The pass does something the graph cannot see (writes buffers, builds acceleration
    ///        structures, ...): it is never dropped.
    RenderPassBuilder& sideEffect();
    /// @brief What the pass records. Attachments, if any, are already bound (beginRendering).
    RenderPassBuilder& execute(std::function<void(RenderPassContext&)> fn);

private:
    friend class RenderGraph;
    RenderPassBuilder(RenderGraph& g, u32 pass) : m_graph(g), m_pass(pass) {}
    RenderGraph& m_graph;
    u32 m_pass;
};

/// @brief One frame's passes and textures. Build it, compile() it, then execute() it.
class RenderGraph : NonCopyable {
public:
    /// @brief How a pass uses a texture.
    enum class Access : u8 { Color, Depth, Sample, Storage };

    struct TextureUse {
        RGTexture texture;
        Access access;
        rhi::LoadOp load = rhi::LoadOp::Clear;
        rhi::StoreOp store = rhi::StoreOp::Store;
        std::array<f32, 4> clearColor{0.0f, 0.0f, 0.0f, 1.0f};
        f32 clearDepth = 1.0f;
        /// @brief True if the pass reads what was there before (sampled, storage, attachments loaded).
        bool reads() const { return access == Access::Sample || access == Access::Storage || load == rhi::LoadOp::Load; }
        /// @brief True if the pass changes it (attachments, storage).
        bool writes() const { return access != Access::Sample; }
        /// @brief The state the texture needs during the pass.
        rhi::ResourceState state() const;
    };

    struct Pass {
        std::string name;
        std::vector<TextureUse> uses;
        std::function<void(RenderPassContext&)> run;
        bool sideEffect = false;
        bool culled = false;                 ///< After compile(): nothing needs it.
        std::vector<u32> dependsOn;          ///< After compile(): the passes whose writes it reads.
        bool raster() const;                 ///< Has attachments (run inside beginRendering).
    };

    struct Texture {
        std::string name;
        bool imported = false;
        rhi::TextureHandle handle;           ///< Imported: given; transient: set by execute().
        Extent2D extent{};
        rhi::Format format = rhi::Format::RGBA8_UNORM;
        rhi::TextureUsage usage = rhi::TextureUsage::None;
        std::optional<rhi::ResourceState> finalState; ///< Imported: the state it ends the frame in.
        bool output = false;                 ///< Kept: the passes writing it are never dropped.
        // After compile() (transients): the passes it lives between, and the pooled texture it uses.
        u32 firstPass = ~0u, lastPass = 0;
        u32 slot = ~0u;
    };

    RenderGraph() = default;

    /// @brief A texture made outside the graph (a view's targets, the swapchain image).
    /// @param finalState If set, the texture ends the frame in this state, and it is an output.
    RGTexture import(std::string name, rhi::TextureHandle texture, Extent2D extent, rhi::Format format,
                     std::optional<rhi::ResourceState> finalState = std::nullopt);
    /// @brief A transient texture: lives this frame only, from the graph's pool.
    RGTexture create(const RGTextureDesc& desc);
    /// @brief Keeps every pass writing `t` (and what they need) even if nothing reads it.
    void markOutput(RGTexture t);
    /// @brief Adds a pass; it runs after those added before it.
    RenderPassBuilder addPass(std::string name);

    /// @brief Works out dependencies, drops unneeded passes, the transients' lifetimes and which
    ///        of them share a pooled texture. Needs no device.
    void compile();
    /// @brief Records the passes not dropped (compiles first if needed): transitions, rendering,
    ///        debug labels. Transients come from `pool`.
    void execute(rhi::CommandList& cmd, RenderGraphPool& pool);
    /// @brief Empties the graph for another frame (keeps capacity).
    void clear();

    /// @name Inspection (after compile)
    /// @{
    const std::vector<Pass>& passes() const { return m_passes; }
    const std::vector<Texture>& textures() const { return m_textures; }
    u32 culledCount() const;
    u32 transientCount() const;
    /// @brief Pooled textures the transients need (transients that do not overlap share one).
    u32 slotCount() const { return static_cast<u32>(m_slots.size()); }
    /// @brief One line per pass: kept or dropped, and what it reads and writes.
    std::string describe() const;
    /// @}

private:
    friend class RenderPassBuilder;
    friend class RenderPassContext;
    struct Slot {
        u32 width, height;
        rhi::Format format;
        rhi::TextureUsage usage;
        const char* name;
    };
    std::vector<Pass> m_passes;
    std::vector<Texture> m_textures;
    std::vector<Slot> m_slots;
    bool m_compiled = false;
};

/// @brief The textures behind transient graph textures, kept from frame to frame (by size,
///        format and usage) and released after some frames unused. Owned by the Renderer.
class RenderGraphPool : NonCopyable {
public:
    explicit RenderGraphPool(rhi::Device& device) : m_device(device) {}
    ~RenderGraphPool();
    /// @brief Marks every texture free for the next graph (called by RenderGraph::execute).
    void beginGraph();
    /// @brief A free texture of this kind (made if none), taken until the next beginGraph().
    rhi::TextureHandle acquire(u32 width, u32 height, rhi::Format format, rhi::TextureUsage usage, const char* name);
    /// @brief Releases the textures no graph used for `frames` calls of endFrame().
    void endFrame(u32 frames = 60);
    /// @brief Textures held.
    u32 size() const { return static_cast<u32>(m_entries.size()); }
    rhi::Device& device() { return m_device; }

private:
    struct Entry {
        u32 width, height;
        rhi::Format format;
        rhi::TextureUsage usage;
        rhi::TextureHandle texture;
        bool taken = false; // by the current graph
        bool used = false;  // by any graph since the last endFrame()
        u32 idle = 0;
    };
    rhi::Device& m_device;
    std::vector<Entry> m_entries;
};

} // namespace nova
