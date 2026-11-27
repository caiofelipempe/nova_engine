// Node animation: sampling clips, and the scene system that plays them (Scene::updateAnimations).
#include "nova/scene/Animation.h"

#include "nova/scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace nova {

namespace {

glm::quat asQuat(const glm::vec4& v) { return glm::quat(v.w, v.x, v.y, v.z); }
glm::vec4 asVec4(const glm::quat& q) { return {q.x, q.y, q.z, q.w}; }

} // namespace

glm::vec4 AnimationChannel::sample(f32 time) const {
    const size_t keys = times.size();
    const bool cubic = interpolation == AnimationInterpolation::CubicSpline;
    const size_t stride = cubic ? 3 : 1;
    if (keys == 0 || values.size() < keys * stride) return path == AnimationPath::Scale ? glm::vec4(1.0f) : glm::vec4(0, 0, 0, path == AnimationPath::Rotation ? 1.0f : 0.0f);
    auto value = [&](size_t k) { return values[k * stride + (cubic ? 1 : 0)]; };
    if (keys == 1 || time <= times.front()) return value(0);
    if (time >= times.back()) return value(keys - 1);

    // The keys around `time`.
    const size_t next = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), time) - times.begin());
    const size_t prev = next - 1;
    const f32 span = times[next] - times[prev];
    const f32 t = span > 0.0f ? (time - times[prev]) / span : 0.0f;
    const bool rotation = path == AnimationPath::Rotation;

    switch (interpolation) {
        case AnimationInterpolation::Step: return value(prev);
        case AnimationInterpolation::Linear:
            if (rotation) return asVec4(glm::slerp(asQuat(value(prev)), asQuat(value(next)), t));
            return glm::mix(value(prev), value(next), t);
        case AnimationInterpolation::CubicSpline: {
            // Hermite: p0, its out-tangent, p1, its in-tangent (tangents scaled by the key span).
            const glm::vec4 p0 = value(prev), m0 = values[prev * 3 + 2] * span;
            const glm::vec4 p1 = value(next), m1 = values[next * 3 + 0] * span;
            const f32 t2 = t * t, t3 = t2 * t;
            glm::vec4 v = (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
            if (rotation) v = asVec4(glm::normalize(asQuat(v)));
            return v;
        }
    }
    return value(prev);
}

bool inMask(const std::vector<std::string>& mask, const std::string& path) {
    if (mask.empty()) return true;
    for (const std::string& m : mask)
        if (m.empty() || path == m || (path.size() > m.size() && path.compare(0, m.size(), m) == 0 && path[m.size()] == '/'))
            return true;
    return false;
}

// ------------------------------------------------------------------- Scene --
namespace {
Scene::AnimationSource& defaultAnimationSource() {
    static Scene::AnimationSource source;
    return source;
}
} // namespace

void Scene::setDefaultAnimationSource(AnimationSource source) { defaultAnimationSource() = std::move(source); }

std::shared_ptr<const AnimationClip> Scene::findAnimation(const std::string& clip) const {
    if (clip.empty()) return nullptr;
    if (m_animationSource) return m_animationSource(clip);
    const AnimationSource& fallback = defaultAnimationSource();
    return fallback ? fallback(clip) : nullptr;
}

// The entity a channel's path names, from `from` down by child names.
entt::entity Scene::findByPath(entt::entity from, const std::string& path) {
    Entity at{from, this};
    size_t start = 0;
    while (at.valid() && start < path.size()) {
        const size_t slash = path.find('/', start);
        const std::string name = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        Entity found;
        for (Entity c : childrenOf(at))
            if (c.name() == name) {
                found = c;
                break;
            }
        at = found;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return at.valid() ? at.handle() : entt::null;
}

namespace {
Scene::SkinSource& defaultSkinSource() {
    static Scene::SkinSource source;
    return source;
}

// The pose as a matrix: rotation and scale, translated by `offset` (doubles).
glm::dmat4 poseMatrix(const TransformComponent& t, const glm::dvec3& offset) {
    glm::dmat4 m = glm::mat4_cast(t.rotation.toDouble());
    const glm::dvec3 s = t.scale.toDouble();
    m[0] *= s.x;
    m[1] *= s.y;
    m[2] *= s.z;
    m[3] = glm::dvec4(offset, 1.0);
    return m;
}
} // namespace

void Scene::setDefaultSkinSource(SkinSource source) { defaultSkinSource() = std::move(source); }

std::shared_ptr<const SkinData> Scene::findSkin(const std::string& skin) const {
    if (skin.empty()) return nullptr;
    if (m_skinSource) return m_skinSource(skin);
    const SkinSource& fallback = defaultSkinSource();
    return fallback ? fallback(skin) : nullptr;
}

// A skinned mesh's joint matrices for this frame, appended to out.joints: each maps the bind pose
// to the pose, in the mesh's own space (the draw's model matrix places it). Worked out from poses
// relative to the mesh, in doubles: exact however far out in the world.
bool Scene::skinJoints(entt::entity e, const TransformComponent& pose, RenderScene& out, u32& offset, u32& count) {
    offset = static_cast<u32>(out.joints.size());
    if (!jointMatrices(e, pose, out.joints, true)) {
        out.joints.resize(offset);
        return false;
    }
    count = static_cast<u32>(out.joints.size()) - offset;
    return true;
}

bool Scene::skinPose(Entity entity, std::vector<glm::mat4>& joints) {
    joints.clear();
    if (!entity.valid() || entity.scene() != this || !entity.has<SkinComponent>()) return false;
    updateTransforms(); // the joints where their parents put them now
    return jointMatrices(entity.handle(), entity.transform(), joints, false);
}

// The joint matrices of entity `e`'s skin, appended to `out` (mesh space, for the mesh at `pose`).
// `drawn`: the poses drawn between physics steps (rendering) rather than the transforms (tools).
bool Scene::jointMatrices(entt::entity e, const TransformComponent& pose, std::vector<glm::mat4>& out, bool drawn) {
    auto& sc = m_registry.get<SkinComponent>(e);
    if (!sc.skin.empty() && (sc.resolved != sc.skin || !sc.data)) {
        sc.data = findSkin(sc.skin);
        sc.resolved = sc.skin;
    }
    const SkinData* skin = sc.data.get();
    if (!skin || skin->joints.empty()) return false;
    const entt::entity root = lookup(sc.root.uuid);
    if (root == entt::null) return false;
    bool stale = sc.boundSkin != skin || sc.boundRoot != sc.root.uuid || sc.joints.size() != skin->joints.size();
    for (entt::entity j : sc.joints) stale = stale || (j != entt::null && !m_registry.valid(j));
    if (stale) {
        sc.joints.clear();
        for (const std::string& path : skin->joints) sc.joints.push_back(findByPath(root, path));
        sc.boundSkin = skin;
        sc.boundRoot = sc.root.uuid;
    }
    const glm::dmat4 meshInverse = glm::inverse(poseMatrix(pose, glm::dvec3(0.0))); // the mesh's frame, at its origin
    TransformComponent between;
    for (size_t i = 0; i < skin->joints.size(); ++i) {
        const entt::entity j = sc.joints[i];
        const glm::dmat4 bindInverse = i < skin->inverseBind.size() ? glm::dmat4(skin->inverseBind[i]) : glm::dmat4(1.0);
        if (j == entt::null || !m_registry.valid(j)) {
            out.push_back(glm::mat4(1.0f)); // a missing joint: that part stays in the bind pose
            continue;
        }
        const TransformComponent& jt = m_registry.get<TransformComponent>(j);
        const TransformComponent& at = drawn && drawnPose(j, jt, between) ? between : jt; // like the mesh
        const glm::dmat4 joint = poseMatrix(at, (at.position - pose.position).toMeters());
        out.push_back(glm::mat4(meshInverse * joint * bindInverse));
    }
    return true;
}

// Finds the entities a clip's channels name, again when the clip changed or one of them is gone.
void Scene::bindTargets(entt::entity e, const AnimationClip* clip, const AnimationClip*& bound,
                        std::vector<entt::entity>& targets) {
    bool stale = bound != clip || targets.size() != clip->channels.size();
    for (entt::entity t : targets) stale = stale || (t != entt::null && !m_registry.valid(t));
    if (!stale) return;
    targets.clear();
    for (const AnimationChannel& ch : clip->channels) targets.push_back(findByPath(e, ch.target));
    bound = clip;
}

namespace {

// Moves a clip's time by `seconds`, wrapping or holding at its ends.
void advance(f64& time, f64 seconds, const AnimationClip& clip, bool loop) {
    time += seconds;
    const f64 length = clip.duration;
    if (length <= 0.0) time = 0.0;
    else if (loop) time -= std::floor(time / length) * length;
    else time = std::clamp(time, 0.0, length);
}

// The events of `clip` the playback passes going `delta` seconds from `from` (backwards when
// negative), in the order passed: each loop of a looping clip again, at most 8 per event per
// update; a clip that does not loop holds at its ends. `fromIncluded`: an event right at `from`
// counts (a clip just started).
void passedEvents(const AnimationClip& clip, f64 from, f64 delta, bool loop, bool fromIncluded,
                  std::vector<std::pair<f64, const AnimationEvent*>>& out) {
    out.clear();
    if (clip.events.empty() || (delta == 0.0 && !fromIncluded)) return;
    const f64 length = clip.duration;
    const bool forward = delta >= 0.0;
    if (!loop || length <= 0.0) {
        const f64 to = length > 0.0 ? std::clamp(from + delta, 0.0, length) : from;
        for (const AnimationEvent& ev : clip.events) {
            const f64 t = ev.time;
            const bool in = forward ? ((t > from || (fromIncluded && t == from)) && t <= to)
                                    : ((t < from || (fromIncluded && t == from)) && t >= to);
            if (in) out.emplace_back(t, &ev);
        }
    } else {
        const f64 to = from + delta; // unwrapped
        for (const AnimationEvent& ev : clip.events) {
            const f64 t = ev.time;
            // Every lap k with t + k·length in (from, to] (forward) or [to, from) (backward).
            i64 first, last;
            if (forward) {
                first = fromIncluded ? static_cast<i64>(std::ceil((from - t) / length)) : static_cast<i64>(std::floor((from - t) / length)) + 1;
                last = static_cast<i64>(std::floor((to - t) / length));
            } else {
                first = static_cast<i64>(std::ceil((to - t) / length));
                last = fromIncluded ? static_cast<i64>(std::floor((from - t) / length)) : static_cast<i64>(std::ceil((from - t) / length)) - 1;
            }
            for (i64 k = first, n = 0; k <= last && n < 8; ++k, ++n) out.emplace_back(t + static_cast<f64>(k) * length, &ev);
        }
    }
    std::sort(out.begin(), out.end(), [forward](const auto& a, const auto& b) { return forward ? a.first < b.first : a.first > b.first; });
}

// One node's mix: the weighted sums of each property's values.
struct Mix {
    glm::dvec3 position{0.0}, scale{0.0};
    glm::dvec4 rotation{0.0}; // quaternions (x, y, z, w), each turned to the side of the first
    f64 positionWeight = 0.0, rotationWeight = 0.0, scaleWeight = 0.0;
};

} // namespace

void Scene::updateAnimations(Duration frame) {
    const f64 dt = frame.seconds();
    std::vector<entt::entity> animated;
    for (auto e : m_registry.view<AnimatorComponent>()) animated.push_back(e);
    std::reverse(animated.begin(), animated.end()); // creation order
    std::vector<std::pair<entt::entity, Mix>> mixes; // per node, in the order first met
    std::vector<std::pair<entt::entity, LocalTransform>> poses; // the nodes' offsets being made
    std::vector<std::pair<f64, const AnimationEvent*>> passed;  // events passed by one clip
    m_animationHits.clear();
    for (entt::entity e : animated) {
        if (!m_registry.valid(e)) continue;
        auto& anim = m_registry.get<AnimatorComponent>(e);
        // The clip: set directly, or (again, when the reference changed) found by its reference.
        if (!anim.clip.empty() && (anim.resolved != anim.clip || !anim.data)) {
            anim.data = findAnimation(anim.clip);
            anim.resolved = anim.clip;
        }
        const AnimationClip* clip = anim.data.get();
        if (clip) bindTargets(e, clip, anim.boundClip, anim.targets);

        // Time and weights move on; the events passed on the way are noted.
        if (anim.playing) {
            if (clip) {
                const f64 delta = toDouble(anim.speed) * dt;
                if (anim.weight > 0.0f || anim.fade > 0.0f) {
                    passedEvents(*clip, anim.time, delta, anim.loop, anim.started, passed);
                    for (const auto& [at, ev] : passed)
                        m_animationHits.emplace_back(Entity{e, this}, AnimationEventHit{ev->name, ev->value, clip->name, ev->time});
                }
                anim.started = false;
                advance(anim.time, delta, *clip, anim.loop);
            }
            anim.weight = std::clamp(anim.weight + anim.fade * static_cast<f32>(dt), 0.0f, 1.0f);
            if (anim.weight >= 1.0f) anim.fade = 0.0f;
            const f64 phase = clip && clip->duration > 0.0f ? anim.time / clip->duration : 0.0;
            for (AnimationLayer& layer : anim.layers) {
                if (!layer.data) continue;
                const f64 before = layer.time;
                f64 delta = layer.speed * dt;
                if (layer.sync && clip) {
                    layer.time = phase * layer.data->duration;
                    delta = clip->duration > 0.0f ? toDouble(anim.speed) * dt * layer.data->duration / clip->duration : 0.0;
                } else {
                    advance(layer.time, delta, *layer.data, layer.loop);
                }
                if (layer.events && layer.fade >= 0.0f && layer.weight > 0.0f) {
                    passedEvents(*layer.data, before, delta, layer.loop, false, passed);
                    for (const auto& [at, ev] : passed)
                        m_animationHits.emplace_back(Entity{e, this}, AnimationEventHit{ev->name, ev->value, layer.data->name, ev->time});
                }
                layer.weight += layer.fade * static_cast<f32>(dt);
            }
            std::erase_if(anim.layers, [](const AnimationLayer& l) { return !l.data || (l.fade < 0.0f && l.weight <= 0.0f); });
        }

        // Every clip's channels, weighted, into the mix of the node each one moves.
        mixes.clear();
        auto mixOf = [&](entt::entity target) -> Mix& {
            for (auto& [t, m] : mixes)
                if (t == target) return m;
            return mixes.emplace_back(target, Mix{}).second;
        };
        auto accumulate = [&](const AnimationClip& c, const std::vector<entt::entity>& targets, f64 time, f32 weight,
                              const std::vector<std::string>& mask) {
            if (weight <= 0.0f) return;
            for (size_t i = 0; i < c.channels.size() && i < targets.size(); ++i) {
                const entt::entity target = targets[i];
                if (target == entt::null || !m_registry.valid(target)) continue;
                const AnimationChannel& ch = c.channels[i];
                if (!inMask(mask, ch.target)) continue;
                const glm::dvec4 v(ch.sample(static_cast<f32>(time)));
                Mix& m = mixOf(target);
                switch (ch.path) {
                    case AnimationPath::Translation:
                        m.position += glm::dvec3(v) * f64(weight);
                        m.positionWeight += weight;
                        break;
                    case AnimationPath::Rotation: {
                        const bool flip = m.rotationWeight > 0.0 && glm::dot(m.rotation, v) < 0.0; // the same side
                        m.rotation += (flip ? -v : v) * f64(weight);
                        m.rotationWeight += weight;
                        break;
                    }
                    case AnimationPath::Scale:
                        m.scale += glm::dvec3(v) * f64(weight);
                        m.scaleWeight += weight;
                        break;
                }
            }
        };
        static const std::vector<std::string> everything;
        if (clip) accumulate(*clip, anim.targets, anim.time, anim.weight, everything);
        for (AnimationLayer& layer : anim.layers) {
            if (!layer.data) continue;
            bindTargets(e, layer.data.get(), layer.boundClip, layer.targets);
            if (layer.mode == AnimationBlend::Average) accumulate(*layer.data, layer.targets, layer.time, layer.weight, layer.mask);
        }

        // Each node's offset from its parent: the weighted averages (what no clip moves stays)...
        poses.clear();
        auto poseOf = [&](entt::entity target) -> LocalTransform& {
            for (auto& [t, p] : poses)
                if (t == target) return p;
            return poses.emplace_back(target, localTransform(Entity{target, this})).second;
        };
        for (const auto& [target, m] : mixes) {
            // The clips' values are float (glTF keys), mixed in doubles: quantized into the pose here.
            LocalTransform& local = poseOf(target);
            if (m.positionWeight > 0.0) local.position = GlobalLengthVec(m.position / m.positionWeight);
            if (m.rotationWeight > 0.0 && glm::length(m.rotation) > 1e-12) {
                const glm::dvec4 q = glm::normalize(m.rotation);
                local.rotation = Quat(glm::dquat(q.w, q.x, q.y, q.z));
            }
            if (m.scaleWeight > 0.0) local.scale = ScalarVec(m.scale / m.scaleWeight);
        }
        // ... then the overriding layers, in order, each over what is there by its weight.
        for (const AnimationLayer& layer : anim.layers) {
            if (!layer.data || layer.mode != AnimationBlend::Override) continue;
            const f64 w = std::clamp(static_cast<f64>(layer.weight), 0.0, 1.0);
            if (w <= 0.0) continue;
            const AnimationClip& c = *layer.data;
            for (size_t i = 0; i < c.channels.size() && i < layer.targets.size(); ++i) {
                const entt::entity target = layer.targets[i];
                const AnimationChannel& ch = c.channels[i];
                if (target == entt::null || !m_registry.valid(target) || !inMask(layer.mask, ch.target)) continue;
                const glm::dvec4 v(ch.sample(static_cast<f32>(layer.time)));
                LocalTransform& local = poseOf(target);
                switch (ch.path) {
                    case AnimationPath::Translation:
                        local.position = GlobalLengthVec(glm::mix(local.position.toDouble(), glm::dvec3(v), w));
                        break;
                    case AnimationPath::Rotation:
                        local.rotation = slerp(local.rotation, Quat(glm::dquat(v.w, v.x, v.y, v.z)), toFixed<Unit>(w));
                        break;
                    case AnimationPath::Scale: local.scale = ScalarVec(glm::mix(local.scale.toDouble(), glm::dvec3(v), w)); break;
                }
            }
        }
        for (const auto& [target, local] : poses) setLocalTransform(Entity{target, this}, local);
    }
}

void Scene::playAnimation(Entity entity, std::shared_ptr<const AnimationClip> clip, f32 fadeSeconds, bool loop) {
    if (!entity.valid() || entity.scene() != this) return;
    if (!entity.has<AnimatorComponent>()) entity.add<AnimatorComponent>();
    auto& anim = entity.get<AnimatorComponent>();
    // The clip it was playing fades out from where it is (no fade: it stops at once).
    if (anim.data && fadeSeconds > 0.0f && anim.weight > 0.0f) {
        AnimationLayer old;
        old.data = anim.data;
        old.time = anim.time;
        old.speed = toFloat(anim.speed);
        old.loop = anim.loop;
        old.weight = anim.weight;
        old.fade = -anim.weight / fadeSeconds;
        anim.layers.push_back(std::move(old));
    }
    for (AnimationLayer& layer : anim.layers) // earlier fades: out in the same time at most
        if (layer.fade < 0.0f && fadeSeconds > 0.0f) layer.fade = std::min(layer.fade, -layer.weight / fadeSeconds);
    if (fadeSeconds <= 0.0f) std::erase_if(anim.layers, [](const AnimationLayer& l) { return l.fade < 0.0f; });
    anim.data = std::move(clip);
    anim.clip.clear();
    anim.resolved.clear();
    anim.time = 0.0;
    anim.loop = loop;
    anim.playing = true;
    anim.weight = fadeSeconds > 0.0f ? 0.0f : 1.0f;
    anim.fade = fadeSeconds > 0.0f ? 1.0f / fadeSeconds : 0.0f;
    anim.started = true; // events at its start fire on the next update
}

void Scene::dispatchAnimationEvents() {
    for (size_t i = 0; i < m_animationHits.size(); ++i) {
        const auto [entity, hit] = m_animationHits[i]; // a copy: scripts may play other clips
        if (!entity.valid()) continue;
        if (auto* sc = m_registry.try_get<ScriptComponent>(entity.handle()))
            for (size_t k = 0; k < sc->scripts.size(); ++k)
                if (sc->scripts[k]->m_created) sc->scripts[k]->onAnimationEvent(hit);
    }
}

void Scene::playAnimation(Entity entity, const std::string& clip, f32 fadeSeconds, bool loop) {
    playAnimation(entity, findAnimation(clip), fadeSeconds, loop);
    if (entity.valid() && entity.has<AnimatorComponent>()) { // saved by its reference
        auto& anim = entity.get<AnimatorComponent>();
        anim.clip = clip;
        anim.resolved = clip;
    }
}

void Scene::blendAnimation(Entity entity, std::shared_ptr<const AnimationClip> clip, f32 weight, bool sync) {
    AnimationMix mix;
    mix.weight = weight;
    mix.sync = sync;
    blendAnimation(entity, std::move(clip), mix);
}

void Scene::blendAnimation(Entity entity, std::shared_ptr<const AnimationClip> clip, const AnimationMix& mix) {
    if (!entity.valid() || entity.scene() != this || !clip) return;
    if (!entity.has<AnimatorComponent>()) entity.add<AnimatorComponent>();
    auto& anim = entity.get<AnimatorComponent>();
    auto set = [&](AnimationLayer& layer) {
        layer.weight = mix.weight;
        layer.sync = mix.sync;
        layer.mode = mix.mode;
        layer.mask = mix.mask;
        layer.events = mix.events;
    };
    for (auto it = anim.layers.begin(); it != anim.layers.end(); ++it)
        if (it->data == clip && it->fade >= 0.0f) { // already mixed in: new options
            if (mix.weight <= 0.0f) anim.layers.erase(it);
            else set(*it);
            return;
        }
    if (mix.weight <= 0.0f) return;
    AnimationLayer layer;
    layer.data = std::move(clip);
    set(layer);
    anim.layers.push_back(std::move(layer));
}

} // namespace nova
