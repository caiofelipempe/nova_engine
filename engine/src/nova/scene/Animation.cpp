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
