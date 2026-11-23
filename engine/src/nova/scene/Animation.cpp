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

void Scene::updateAnimations(Duration frame) {
    std::vector<entt::entity> animated;
    for (auto e : m_registry.view<AnimatorComponent>()) animated.push_back(e);
    std::reverse(animated.begin(), animated.end()); // creation order
    for (entt::entity e : animated) {
        if (!m_registry.valid(e)) continue;
        auto& anim = m_registry.get<AnimatorComponent>(e);
        // The clip: set directly, or (again, when the reference changed) found by its reference.
        if (!anim.clip.empty() && (anim.resolved != anim.clip || !anim.data)) {
            anim.data = findAnimation(anim.clip);
            anim.resolved = anim.clip;
        }
        const AnimationClip* clip = anim.data.get();
        if (!clip) continue;
        // Its channels' entities, found again when the clip changed or one of them is gone.
        bool stale = anim.boundClip != clip || anim.targets.size() != clip->channels.size();
        for (entt::entity t : anim.targets) stale = stale || (t != entt::null && !m_registry.valid(t));
        if (stale) {
            anim.targets.clear();
            for (const AnimationChannel& ch : clip->channels) anim.targets.push_back(findByPath(e, ch.target));
            anim.boundClip = clip;
        }
        if (anim.playing) {
            anim.time += toDouble(anim.speed) * frame.seconds();
            const f64 length = clip->duration;
            if (length > 0.0) {
                if (anim.loop) anim.time -= std::floor(anim.time / length) * length;
                else anim.time = std::clamp(anim.time, 0.0, length);
            } else {
                anim.time = 0.0;
            }
        }
        // Each channel sets one property of its entity's offset from its parent.
        for (size_t i = 0; i < clip->channels.size(); ++i) {
            const entt::entity target = anim.targets[i];
            if (target == entt::null || !m_registry.valid(target)) continue;
            const AnimationChannel& ch = clip->channels[i];
            const glm::vec4 v = ch.sample(static_cast<f32>(anim.time));
            Entity te{target, this};
            LocalTransform local = localTransform(te);
            switch (ch.path) {
                case AnimationPath::Translation: local.position = glm::dvec3(v); break;
                case AnimationPath::Rotation: local.rotation = glm::dquat(v.w, v.x, v.y, v.z); break;
                case AnimationPath::Scale: local.scale = glm::dvec3(v); break;
            }
            setLocalTransform(te, local);
        }
    }
}

} // namespace nova
