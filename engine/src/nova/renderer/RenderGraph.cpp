#include "nova/renderer/RenderGraph.h"

#include <algorithm>

namespace nova {

using namespace rhi;

// ------------------------------------------------------------------ context --
TextureHandle RenderPassContext::texture(RGTexture t) const {
    return t.id < m_graph.m_textures.size() ? m_graph.m_textures[t.id].handle : TextureHandle{};
}
u32 RenderPassContext::bindlessIndex(RGTexture t) const {
    const TextureHandle h = texture(t);
    return h && m_device ? m_device->bindlessIndex(h) : ~0u;
}
Extent2D RenderPassContext::extent(RGTexture t) const {
    return t.id < m_graph.m_textures.size() ? m_graph.m_textures[t.id].extent : Extent2D{};
}

// ------------------------------------------------------------------ builder --
RenderPassBuilder& RenderPassBuilder::color(RGTexture t, LoadOp load, StoreOp store, std::array<f32, 4> clear) {
    RenderGraph::TextureUse u{t, RenderGraph::Access::Color, load, store};
    u.clearColor = clear;
    m_graph.m_passes[m_pass].uses.push_back(u);
    return *this;
}
RenderPassBuilder& RenderPassBuilder::depth(RGTexture t, LoadOp load, StoreOp store, f32 clear) {
    RenderGraph::TextureUse u{t, RenderGraph::Access::Depth, load, store};
    u.clearDepth = clear;
    m_graph.m_passes[m_pass].uses.push_back(u);
    return *this;
}
RenderPassBuilder& RenderPassBuilder::sample(RGTexture t) {
    m_graph.m_passes[m_pass].uses.push_back({t, RenderGraph::Access::Sample});
    return *this;
}
RenderPassBuilder& RenderPassBuilder::storage(RGTexture t) {
    m_graph.m_passes[m_pass].uses.push_back({t, RenderGraph::Access::Storage});
    return *this;
}
RenderPassBuilder& RenderPassBuilder::sideEffect() {
    m_graph.m_passes[m_pass].sideEffect = true;
    return *this;
}
RenderPassBuilder& RenderPassBuilder::execute(std::function<void(RenderPassContext&)> fn) {
    m_graph.m_passes[m_pass].run = std::move(fn);
    return *this;
}

// -------------------------------------------------------------------- graph --
ResourceState RenderGraph::TextureUse::state() const {
    switch (access) {
    case Access::Color: return ResourceState::RenderTarget;
    case Access::Depth: return ResourceState::DepthWrite;
    case Access::Sample: return ResourceState::ShaderRead;
    case Access::Storage: return ResourceState::UnorderedAccess;
    }
    return ResourceState::Undefined;
}

bool RenderGraph::Pass::raster() const {
    for (const TextureUse& u : uses)
        if (u.access == Access::Color || u.access == Access::Depth) return true;
    return false;
}

RGTexture RenderGraph::import(std::string name, TextureHandle texture, Extent2D extent, Format format,
                              std::optional<ResourceState> finalState) {
    Texture t;
    t.name = std::move(name);
    t.imported = true;
    t.handle = texture;
    t.extent = extent;
    t.format = format;
    t.finalState = finalState;
    t.output = finalState.has_value();
    m_textures.push_back(std::move(t));
    m_compiled = false;
    return RGTexture{static_cast<u32>(m_textures.size() - 1)};
}

RGTexture RenderGraph::create(const RGTextureDesc& desc) {
    Texture t;
    t.name = desc.name ? desc.name : "transient";
    t.extent = {std::max(1u, desc.width), std::max(1u, desc.height)};
    t.format = desc.format;
    t.usage = desc.usage;
    m_textures.push_back(std::move(t));
    m_compiled = false;
    return RGTexture{static_cast<u32>(m_textures.size() - 1)};
}

void RenderGraph::markOutput(RGTexture t) {
    if (t.id < m_textures.size()) m_textures[t.id].output = true;
    m_compiled = false;
}

RenderPassBuilder RenderGraph::addPass(std::string name) {
    Pass p;
    p.name = std::move(name);
    m_passes.push_back(std::move(p));
    m_compiled = false;
    return RenderPassBuilder(*this, static_cast<u32>(m_passes.size() - 1));
}

void RenderGraph::compile() {
    const u32 passCount = static_cast<u32>(m_passes.size());
    // Dependencies: a pass that reads a texture needs the last pass before it that wrote it.
    std::vector<u32> lastWriter(m_textures.size(), ~0u);
    for (u32 i = 0; i < passCount; ++i) {
        Pass& p = m_passes[i];
        p.dependsOn.clear();
        p.culled = true;
        for (const TextureUse& u : p.uses) {
            if (u.texture.id >= m_textures.size()) continue;
            const u32 w = lastWriter[u.texture.id];
            if (u.reads() && w != ~0u && std::find(p.dependsOn.begin(), p.dependsOn.end(), w) == p.dependsOn.end())
                p.dependsOn.push_back(w);
        }
        for (const TextureUse& u : p.uses)
            if (u.texture.id < m_textures.size() && u.writes()) lastWriter[u.texture.id] = i;
    }
    // Kept: passes with side effects or writing an output, and everything they need.
    std::vector<u32> stack;
    for (u32 i = 0; i < passCount; ++i) {
        const Pass& p = m_passes[i];
        bool root = p.sideEffect;
        for (const TextureUse& u : p.uses)
            root = root || (u.writes() && u.texture.id < m_textures.size() && m_textures[u.texture.id].output);
        if (root) stack.push_back(i);
    }
    while (!stack.empty()) {
        const u32 i = stack.back();
        stack.pop_back();
        if (!m_passes[i].culled) continue;
        m_passes[i].culled = false;
        for (u32 d : m_passes[i].dependsOn) stack.push_back(d);
    }

    // Transients: lifetimes over the kept passes, the usages they need, then pooled textures shared
    // by those of the same kind that do not overlap.
    for (Texture& t : m_textures) {
        t.firstPass = ~0u;
        t.lastPass = 0;
        t.slot = ~0u;
        if (!t.imported) t.handle = {};
    }
    for (u32 i = 0; i < passCount; ++i) {
        if (m_passes[i].culled) continue;
        for (const TextureUse& u : m_passes[i].uses) {
            if (u.texture.id >= m_textures.size()) continue;
            Texture& t = m_textures[u.texture.id];
            t.firstPass = std::min(t.firstPass, i);
            t.lastPass = std::max(t.lastPass, i);
            if (t.imported) continue;
            switch (u.access) {
            case Access::Color: t.usage |= TextureUsage::RenderTarget; break;
            case Access::Depth: t.usage |= TextureUsage::DepthStencil; break;
            case Access::Sample: t.usage |= TextureUsage::Sampled; break;
            case Access::Storage: t.usage |= TextureUsage::Storage; break;
            }
        }
    }
    std::vector<u32> order;
    for (u32 i = 0; i < m_textures.size(); ++i)
        if (!m_textures[i].imported && m_textures[i].firstPass != ~0u) order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [&](u32 a, u32 b) { return m_textures[a].firstPass < m_textures[b].firstPass; });
    m_slots.clear();
    std::vector<u32> slotEnd; // the last pass using each slot so far
    for (u32 i : order) {
        Texture& t = m_textures[i];
        for (u32 s = 0; s < m_slots.size() && t.slot == ~0u; ++s) {
            const Slot& sl = m_slots[s];
            if (sl.width == t.extent.width && sl.height == t.extent.height && sl.format == t.format &&
                sl.usage == t.usage && slotEnd[s] < t.firstPass)
                t.slot = s;
        }
        if (t.slot == ~0u) {
            t.slot = static_cast<u32>(m_slots.size());
            m_slots.push_back({t.extent.width, t.extent.height, t.format, t.usage, nullptr});
            slotEnd.push_back(0);
        }
        slotEnd[t.slot] = t.lastPass;
        if (!m_slots[t.slot].name) m_slots[t.slot].name = t.name.c_str();
    }
    m_compiled = true;
}

void RenderGraph::execute(CommandList& cmd, RenderGraphPool& pool) {
    if (!m_compiled) compile();
    pool.beginGraph();
    std::vector<TextureHandle> slotTextures(m_slots.size());
    for (u32 s = 0; s < m_slots.size(); ++s)
        slotTextures[s] = pool.acquire(m_slots[s].width, m_slots[s].height, m_slots[s].format, m_slots[s].usage, m_slots[s].name);
    for (Texture& t : m_textures)
        if (!t.imported && t.slot != ~0u) t.handle = slotTextures[t.slot];

    RenderPassContext ctx(cmd, *this, &pool.device());
    for (Pass& p : m_passes) {
        if (p.culled) continue;
        cmd.beginDebugLabel(p.name.c_str());
        for (const TextureUse& u : p.uses)
            if ((u.access == Access::Sample || u.access == Access::Storage) && u.texture.id < m_textures.size() &&
                m_textures[u.texture.id].handle)
                cmd.transition(m_textures[u.texture.id].handle, u.state());
        if (p.raster()) {
            RenderingDesc desc;
            for (const TextureUse& u : p.uses) {
                const TextureHandle h = u.texture.id < m_textures.size() ? m_textures[u.texture.id].handle : TextureHandle{};
                if (u.access == Access::Color) desc.colors.push_back({h, u.load, u.store, u.clearColor});
                else if (u.access == Access::Depth) desc.depth = DepthAttachment{h, u.load, u.store, u.clearDepth};
            }
            cmd.beginRendering(desc);
            if (p.run) p.run(ctx);
            cmd.endRendering();
        } else if (p.run) {
            p.run(ctx);
        }
        cmd.endDebugLabel();
    }
    for (const Texture& t : m_textures)
        if (t.imported && t.finalState && t.handle) cmd.transition(t.handle, *t.finalState);
}

void RenderGraph::clear() {
    m_passes.clear();
    m_textures.clear();
    m_slots.clear();
    m_compiled = false;
}

u32 RenderGraph::culledCount() const {
    u32 n = 0;
    for (const Pass& p : m_passes) n += p.culled ? 1u : 0u;
    return n;
}

u32 RenderGraph::transientCount() const {
    u32 n = 0;
    for (const Texture& t : m_textures) n += !t.imported && t.slot != ~0u ? 1u : 0u;
    return n;
}

std::string RenderGraph::describe() const {
    static const char* kAccess[] = {"color", "depth", "sample", "storage"};
    std::string out;
    for (const Pass& p : m_passes) {
        out += p.culled ? "  (dropped) " : "  ";
        out += p.name;
        if (p.sideEffect) out += " [side effect]";
        out += ":";
        for (const TextureUse& u : p.uses) {
            out += " ";
            out += kAccess[static_cast<int>(u.access)];
            out += " ";
            out += u.texture.id < m_textures.size() ? m_textures[u.texture.id].name : "(unused)";
            if (u.access == Access::Color || u.access == Access::Depth)
                out += u.load == LoadOp::Load ? "(load)" : u.load == LoadOp::Clear ? "(clear)" : "";
        }
        out += "\n";
    }
    return out;
}

// --------------------------------------------------------------------- pool --
RenderGraphPool::~RenderGraphPool() {
    for (Entry& e : m_entries) m_device.destroyTexture(e.texture);
}

void RenderGraphPool::beginGraph() {
    for (Entry& e : m_entries) e.taken = false;
}

TextureHandle RenderGraphPool::acquire(u32 width, u32 height, Format format, TextureUsage usage, const char* name) {
    for (Entry& e : m_entries)
        if (!e.taken && e.width == width && e.height == height && e.format == format && e.usage == usage) {
            e.taken = e.used = true;
            return e.texture;
        }
    Entry e{width, height, format, usage, {}, true, true, 0};
    e.texture = m_device.createTexture({width, height, 1, format, usage, name});
    m_entries.push_back(e);
    return e.texture;
}

void RenderGraphPool::endFrame(u32 frames) {
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it->used) {
            it->idle = 0;
            it->used = false;
        } else if (++it->idle > frames) {
            m_device.destroyTexture(it->texture);
            it = m_entries.erase(it);
            continue;
        }
        ++it;
    }
}

} // namespace nova
