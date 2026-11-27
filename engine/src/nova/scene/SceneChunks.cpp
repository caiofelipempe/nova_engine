// Streaming of a large world by chunks (Chunks.h): the focus chunk with hysteresis, the rings of
// loaded and kept chunks with the game's ChunkSource, and the dormancy of what is beyond the
// physics ring. Distances between chunks come from shortest GlobalOffsets, so the seam of the
// circular world needs no special case.
#include "nova/scene/Scene.h"

#include "nova/core/Log.h"

#include <algorithm>
#include <vector>

namespace nova {

namespace {
// a / b rounded down (b > 0).
i64 floorDiv(i64 a, i64 b) { return a / b - ((a % b != 0 && a < 0) ? 1 : 0); }
} // namespace

// ------------------------------------------------------------------- grid --
i64 Scene::chunkSteps() const { return std::max<i64>(m_chunks.size.raw_value(), 1); } // Length raw = world steps

ChunkCoord Scene::chunkOf(const GlobalPosition& p) const {
    ChunkCoord c;
    for (int i = 0; i < 3; ++i)
        if (m_chunks.axes[i]) c[i] = floorDiv(p.raw(i), chunkSteps());
    return c;
}

GlobalPosition Scene::chunkCorner(const ChunkCoord& c) const {
    const i64 s = chunkSteps();
    return GlobalPosition::fromRaw(m_chunks.axes[0] ? c.x * s : 0, m_chunks.axes[1] ? c.y * s : 0, m_chunks.axes[2] ? c.z * s : 0);
}

// Chunks from the focus chunk to the one `p` is in, per axis (the shortest way around the world).
ChunkCoord Scene::chunkDelta(const GlobalPosition& p) const {
    const GlobalOffset o = p - chunkCorner(m_focusChunk);
    ChunkCoord d;
    for (int i = 0; i < 3; ++i)
        if (m_chunks.axes[i]) d[i] = floorDiv(o[i], chunkSteps());
    return d;
}

// The ring `p` is in: the largest number of chunks between it and the focus chunk on an axis.
i64 Scene::chunkRing(const GlobalPosition& p) const {
    const ChunkCoord d = chunkDelta(p);
    return std::max({std::abs(d.x), std::abs(d.y), std::abs(d.z)});
}

void Scene::updateFocusChunk(const GlobalPosition& focus) {
    if (!m_hasFocusChunk) {
        m_focusChunk = chunkOf(focus);
        m_hasFocusChunk = true;
        return;
    }
    // Hysteresis: the focus chunk changes only when the focus is 1/8 of a chunk past its border.
    const i64 s = chunkSteps(), margin = s / 8;
    const GlobalOffset o = focus - chunkCorner(m_focusChunk);
    for (int i = 0; i < 3; ++i)
        if (m_chunks.axes[i] && (o[i] < -margin || o[i] >= s + margin)) {
            m_focusChunk = chunkOf(focus);
            return;
        }
}

// --------------------------------------------------------------------- API --
void Scene::setChunkStreaming(const ChunkSettings& settings) {
    m_chunks = settings;
    m_chunks.physicsRadius = std::max(m_chunks.physicsRadius, 0);
    m_chunks.loadRadius = std::max(m_chunks.loadRadius, 0);
    m_chunks.unloadRadius = std::max(m_chunks.unloadRadius, m_chunks.loadRadius);
    m_hasFocusChunk = false; // taken again from the focus
    for (int i = 0; i < 3; ++i)
        if (m_chunks.enabled && m_chunks.axes[i] && m_worldSize.steps[i] % chunkSteps() != 0)
            NOVA_CORE_WARN("Scene: chunks of {} m do not divide axis {} of the world ({} m): the chunk at its seam is smaller",
                           toDouble(m_chunks.size), i, m_worldSize.size(i));
}

bool Scene::isChunkLoaded(const ChunkCoord& c) const { return m_loadedChunks.contains(c); }

std::vector<ChunkCoord> Scene::loadedChunks() const {
    std::vector<ChunkCoord> out;
    out.reserve(m_loadedChunks.size());
    for (const auto& [c, state] : m_loadedChunks) out.push_back(c);
    return out;
}

ChunkCoord Scene::focusChunk() const { return m_focusChunk; }

bool Scene::isDormant(Entity e) const {
    return e.valid() && (m_dormantBodies.contains(e.handle()) || m_dormantCharacters.contains(e.handle()));
}

// ------------------------------------------------------------------ update --
void Scene::updateChunks(Duration frame) {
    if (!m_chunks.enabled) return;
    updateFocusChunk(currentFocus());
    const GlobalPosition corner = chunkCorner(m_focusChunk);
    const i64 s = chunkSteps();

    // Every chunk of the loaded ring (once, even when the ring is wider than a small world).
    const i32 r = m_chunks.loadRadius;
    const i32 rx = m_chunks.axes[0] ? r : 0, ry = m_chunks.axes[1] ? r : 0, rz = m_chunks.axes[2] ? r : 0;
    std::vector<std::pair<i64, ChunkCoord>> wanted; // (squared distance in chunks, chunk)
    for (i32 dx = -rx; dx <= rx; ++dx)
        for (i32 dy = -ry; dy <= ry; ++dy)
            for (i32 dz = -rz; dz <= rz; ++dz) {
                const ChunkCoord c = chunkOf(corner + GlobalOffset(dx * s, dy * s, dz * s)); // around the world
                if (m_loadedChunks.contains(c)) continue;
                const i64 d2 = i64(dx) * dx + i64(dy) * dy + i64(dz) * dz;
                if (std::none_of(wanted.begin(), wanted.end(), [&](const auto& w) { return w.second == c; }))
                    wanted.emplace_back(d2, c);
            }
    // Nearest first (ties in a fixed order: the same on every run); at most loadsPerFrame.
    std::sort(wanted.begin(), wanted.end());
    const size_t budget = m_chunks.loadsPerFrame == 0 ? wanted.size() : std::min<size_t>(wanted.size(), m_chunks.loadsPerFrame);
    for (size_t i = 0; i < budget; ++i) {
        const ChunkCoord c = wanted[i].second;
        m_loadedChunks[c] = {};
        if (m_chunkSource.load) m_chunkSource.load(*this, c);
    }

    // Loaded chunks out of the kept ring: unloaded after unloadDelay out of it.
    std::vector<ChunkCoord> leaving;
    for (auto& [c, state] : m_loadedChunks) {
        if (chunkRing(chunkCorner(c)) <= m_chunks.unloadRadius) {
            state.outFor = Duration();
            continue;
        }
        state.outFor += frame;
        if (state.outFor >= m_chunks.unloadDelay) leaving.push_back(c);
    }
    for (const ChunkCoord& c : leaving) {
        if (m_chunkSource.unload) m_chunkSource.unload(*this, c);
        std::vector<entt::entity> gone;
        for (auto [e, streamed, t] : m_registry.view<StreamedComponent, TransformComponent>().each())
            if (streamed.unload && chunkOf(t.position) == c) gone.push_back(e);
        for (entt::entity e : gone) destroyEntity(Entity{e, this});
        m_loadedChunks.erase(c);
    }

    // Streamed entities that wandered beyond the kept ring into a chunk that is not loaded.
    std::vector<entt::entity> strays;
    for (auto [e, streamed, t] : m_registry.view<StreamedComponent, TransformComponent>().each())
        if (streamed.unload && chunkRing(t.position) > m_chunks.unloadRadius && !m_loadedChunks.contains(chunkOf(t.position))) strays.push_back(e);
    for (entt::entity e : strays) destroyEntity(Entity{e, this});
}

// Bodies and characters beyond the physics ring leave the physics engine (their velocities kept)
// and come back as they were when the focus comes near. Joints to them wait (syncJoints).
void Scene::updateDormancy() {
    const bool streaming = m_chunks.enabled;
    if (streaming) updateFocusChunk(currentFocus());
    auto inPhysics = [&](const GlobalPosition& p) { return !streaming || chunkRing(p) <= m_chunks.physicsRadius; };

    for (auto [e, rb, t] : m_registry.view<RigidBodyComponent, TransformComponent>().each()) {
        const bool near = inPhysics(t.position);
        if (!near && rb.body) {
            Dormant d;
            if (rb.type == physics::BodyType::Dynamic) {
                d.velocity = SpeedVec(m_physics->linearVelocity(rb.body));
                d.spin = AngleSpeedVec(m_physics->angularVelocity(rb.body));
            }
            for (auto [je, jc] : m_registry.view<JointComponent>().each())
                if (jc.joint && (je == e || jc.connectedBody == rb.body)) destroyJoint(jc);
            m_physics->destroyBody(rb.body);
            rb.body = {};
            m_dormantBodies[e] = d;
        } else if (near) {
            if (auto it = m_dormantBodies.find(e); it != m_dormantBodies.end()) {
                const Dormant d = it->second;
                m_dormantBodies.erase(it);
                createBody(e);
                if (rb.body && rb.type == physics::BodyType::Dynamic) {
                    m_physics->setLinearVelocity(rb.body, d.velocity.toGlm());
                    m_physics->setAngularVelocity(rb.body, d.spin.toGlm());
                }
            }
        }
    }
    for (auto [e, cc, t] : m_registry.view<CharacterControllerComponent, TransformComponent>().each()) {
        const bool near = inPhysics(t.position);
        if (!near && cc.character) {
            m_physics->destroyCharacter(cc.character); // its velocity stays in the component
            cc.character = {};
            m_dormantCharacters.insert(e);
        } else if (near) {
            m_dormantCharacters.erase(e); // syncCharacters makes it again
        }
    }
}

} // namespace nova
