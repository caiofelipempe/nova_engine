// The hierarchy of the scene: children follow their parents (ParentComponent). Transforms stay
// poses in the world; a child's offset from its parent is derived from them and kept here.
#include "nova/scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nova {

namespace {

// Offsets are world steps (2⁻¹⁸ m) in 64 bits, scales Q11.20: integer math, one rounding per
// operation. Only the rotation of an offset longer than 8 km goes through doubles.
constexpr int kScaleBits = static_cast<int>(fractionBits<Scale>);

u64 magnitude(i64 v) { return v < 0 ? 0 - static_cast<u64>(v) : static_cast<u64>(v); }
i64 clampToI64(f64 v) {
    if (!(v == v)) return 0;
    if (v >= 9.2e18) return std::numeric_limits<i64>::max();
    if (v <= -9.2e18) return std::numeric_limits<i64>::min();
    return std::llround(v);
}

// `v` turned by `q`: fixed point while every axis stays in the range of Length (under 8 192 m).
glm::i64vec3 turned(const Quat& q, const glm::i64vec3& v) {
    const u64 limit = u64(1) << 31;
    if (magnitude(v.x) < limit && magnitude(v.y) < limit && magnitude(v.z) < limit) {
        const LengthVec l{fromRaw<Length>(v.x), fromRaw<Length>(v.y), fromRaw<Length>(v.z)};
        if (l.lengthSquaredRaw() < (u64(1) << 62)) { // shorter than 8 192 m: the turned axes fit too
            const LengthVec r = q.rotate(l);
            return {r.x.raw_value(), r.y.raw_value(), r.z.raw_value()};
        }
    }
    const glm::dvec3 r = q.toDouble() * glm::dvec3(v);
    return {clampToI64(r.x), clampToI64(r.y), clampToI64(r.z)};
}

// Steps times a scale (rounded; exact integer math below 2³² steps per axis).
i64 scaledSteps(i64 steps, Scale s) {
    const i64 f = s.raw_value();
    if (magnitude(steps) < (u64(1) << 32)) return detail::roundShift<kScaleBits>(steps * f);
    return clampToI64(static_cast<f64>(steps) * toDouble(s));
}
// Steps divided by a scale (rounded). A zero scale has no inverse: the offset is kept as it is
// (the child sits on the parent while the scale is zero, and where it was when it grows again).
i64 unscaledSteps(i64 steps, Scale s) {
    const i64 f = s.raw_value();
    if (f == 0) return steps;
    if (magnitude(steps) < (u64(1) << 46)) return detail::roundDiv(steps * (i64(1) << kScaleBits), f);
    return clampToI64(static_cast<f64>(steps) / toDouble(s));
}
// A scale divided by another (rounded, saturating; a zero divisor keeps `a`).
Scale scaleRatio(Scale a, Scale b) {
    if (b.raw_value() == 0) return a;
    const i64 q = detail::roundDiv(static_cast<i64>(a.raw_value()) * (i64(1) << kScaleBits), b.raw_value());
    return fromRaw<Scale>(std::clamp<i64>(q, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()));
}
// The product of two scales (rounded, saturating).
Scale scaleProduct(Scale a, Scale b) {
    const i64 p = detail::roundShift<kScaleBits>(static_cast<i64>(a.raw_value()) * b.raw_value());
    return fromRaw<Scale>(std::clamp<i64>(p, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max()));
}

// The pose `t` as an offset from the parent pose `p`.
LocalTransform offsetFrom(const TransformComponent& p, const TransformComponent& t) {
    const Quat turnBack = p.rotation.conjugate();
    const glm::i64vec3 back = turned(turnBack, (t.position - p.position).raw()); // the shortest way, across seams too
    LocalTransform l;
    for (int i = 0; i < 3; ++i) {
        l.position[i] = fromRaw<GlobalLength>(unscaledSteps(back[i], p.scale[i]));
        l.scale[i] = scaleRatio(t.scale[i], p.scale[i]);
    }
    l.rotation = (turnBack * t.rotation).normalized();
    return l;
}

// Places `t` at the offset `l` from the parent pose `p`.
void placeAt(const TransformComponent& p, const LocalTransform& l, TransformComponent& t) {
    glm::i64vec3 scaled;
    for (int i = 0; i < 3; ++i) {
        scaled[i] = scaledSteps(l.position[i].raw_value(), p.scale[i]);
        t.scale[i] = scaleProduct(p.scale[i], l.scale[i]);
    }
    const glm::i64vec3 offset = turned(p.rotation, scaled);
    t.position = p.position + GlobalOffset(offset.x, offset.y, offset.z); // moving loops around the world
    t.rotation = (p.rotation * l.rotation).normalized();
}

// Remembers where the child was put and for which parent pose: what changes next is a move.
void remember(ParentComponent& pc, const TransformComponent& t, const TransformComponent& p) {
    pc.linkedTo = pc.parent.uuid;
    pc.synced = true;
    pc.placedPosition = t.position;
    pc.placedRotation = t.rotation;
    pc.placedScale = t.scale;
    pc.parentPosition = p.position;
    pc.parentRotation = p.rotation;
    pc.parentScale = p.scale;
}

bool movedSincePlaced(const ParentComponent& pc, const TransformComponent& t) {
    return !pc.synced || pc.linkedTo != pc.parent.uuid || !(t.position == pc.placedPosition) ||
           !(t.rotation == pc.placedRotation) || !(t.scale == pc.placedScale);
}

} // namespace

entt::entity Scene::lookup(u64 uuid) const {
    if (uuid == 0) return entt::null;
    if (auto it = m_byUuid.find(uuid); it != m_byUuid.end() && m_registry.valid(it->second))
        if (const auto* id = m_registry.try_get<IdComponent>(it->second); id && id->uuid == uuid) return it->second;
    for (auto [e, id] : m_registry.view<const IdComponent>().each()) // created since the last update
        if (id.uuid == uuid) return e;
    return entt::null;
}

void Scene::updateTransforms() {
    m_byUuid.clear();
    for (auto [e, id] : m_registry.view<IdComponent>().each()) m_byUuid[id.uuid] = e;

    // Parents first: by depth (a loop of parents is left alone).
    struct Child {
        entt::entity e;
        int depth;
    };
    std::vector<Child> children;
    for (auto [e, pc] : m_registry.view<ParentComponent>().each()) {
        int depth = 0;
        bool loop = false;
        for (const ParentComponent* at = &pc; at && at->parent;) {
            const entt::entity up = lookup(at->parent.uuid);
            if (up == entt::null) break;
            if (up == e || ++depth > 1024) {
                loop = true;
                break;
            }
            at = m_registry.try_get<ParentComponent>(up);
        }
        if (!loop) children.push_back({e, depth});
    }
    std::reverse(children.begin(), children.end()); // creation order (views go newest first)
    std::stable_sort(children.begin(), children.end(), [](const Child& a, const Child& b) { return a.depth < b.depth; });

    for (const Child& c : children) {
        auto& pc = m_registry.get<ParentComponent>(c.e);
        auto* t = m_registry.try_get<TransformComponent>(c.e);
        const entt::entity up = lookup(pc.parent.uuid);
        const auto* p = up == entt::null ? nullptr : m_registry.try_get<TransformComponent>(up);
        if (!t || !p) { // no parent (any more): a root, where it is
            pc.synced = false;
            continue;
        }
        if (movedSincePlaced(pc, *t)) pc.local = offsetFrom(*p, *t); // moved (or new link): keeps its pose
        else if (!(p->position == pc.parentPosition) || !(p->rotation == pc.parentRotation) || !(p->scale == pc.parentScale))
            placeAt(*p, pc.local, *t); // the parent moved: follow it
        remember(pc, *t, *p);
    }
}

bool Scene::setParent(Entity child, Entity parent) {
    if (!child.valid() || child.scene() != this) return false;
    if (!parent.valid()) {
        m_registry.remove<ParentComponent>(child.handle());
        return true;
    }
    if (parent.scene() != this) return false;
    for (Entity at = parent; at.valid(); at = parentOf(at)) // no loops
        if (at == child) return false;
    auto& pc = m_registry.emplace_or_replace<ParentComponent>(child.handle());
    pc.parent = refOf(parent);
    const TransformComponent& p = parent.transform();
    TransformComponent& t = child.transform();
    pc.local = offsetFrom(p, t);
    remember(pc, t, p);
    return true;
}

Entity Scene::parentOf(Entity entity) {
    if (!entity.valid()) return {};
    const auto* pc = entity.tryGet<ParentComponent>();
    if (!pc || !pc->parent) return {};
    const entt::entity up = lookup(pc->parent.uuid);
    return up == entt::null ? Entity{} : Entity{up, this};
}

std::vector<Entity> Scene::childrenOf(Entity entity) {
    std::vector<Entity> out;
    if (!entity.valid()) return out;
    const u64 uuid = entity.uuid();
    for (auto [e, pc] : m_registry.view<ParentComponent>().each())
        if (pc.parent.uuid == uuid && e != entity.handle()) out.push_back({e, this});
    std::reverse(out.begin(), out.end()); // creation order
    return out;
}

std::vector<Entity> Scene::descendantsOf(Entity entity) {
    std::vector<Entity> out;
    if (!entity.valid()) return out;
    // Breadth first: the children of `entity`, then of each one found, in turn.
    for (size_t i = 0; i <= out.size(); ++i) {
        const Entity from = i == 0 ? entity : out[i - 1];
        for (Entity c : childrenOf(from))
            if (c != entity && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c); // a loop ends here
    }
    return out;
}

LocalTransform Scene::localTransform(Entity entity) {
    LocalTransform l;
    if (!entity.valid()) return l;
    const TransformComponent& t = entity.transform();
    if (Entity parent = parentOf(entity); parent.valid()) {
        const auto& pc = entity.get<ParentComponent>();
        if (!movedSincePlaced(pc, t)) return pc.local;
        return offsetFrom(parent.transform(), t);
    }
    l.position = t.position.coords();
    l.rotation = t.rotation;
    l.scale = t.scale;
    return l;
}

void Scene::setLocalTransform(Entity entity, const LocalTransform& local) {
    if (!entity.valid()) return;
    TransformComponent& t = entity.transform();
    if (Entity parent = parentOf(entity); parent.valid()) {
        auto& pc = entity.get<ParentComponent>();
        pc.local = local;
        placeAt(parent.transform(), local, t);
        remember(pc, t, parent.transform());
        return;
    }
    t.position = GlobalPosition::fromCoords(local.position);
    t.rotation = local.rotation.normalized();
    t.scale = local.scale;
}

// Drawn between physics steps: a child follows where its parent is drawn.
bool Scene::drawnPose(entt::entity e, const TransformComponent& t, TransformComponent& out, int depth) const {
    if (!interpolatePhysics || !m_physics || depth > 64) return false;
    if (const auto it = m_stepPoses.find(e); it != m_stepPoses.end()) {
        const StepPoses& poses = it->second;
        // Moved since the step (a teleport, the editor): drawn where it is. A kinematic body's
        // transform is the target its script set for the next steps: drawn where the physics has it.
        if (!poses.kinematic && !(t.position == poses.after.position)) return false;
        const f64 alpha = fixedTimeStep > Duration() ? m_physicsAccumulator.seconds() / fixedTimeStep.seconds() : 1.0;
        out = TransformComponent::lerp(poses.before, poses.after, static_cast<f32>(std::clamp(alpha, 0.0, 1.0)));
        out.scale = t.scale;
        return true;
    }
    const auto* pc = m_registry.try_get<ParentComponent>(e);
    if (!pc || movedSincePlaced(*pc, t)) return false;
    const entt::entity up = lookup(pc->parent.uuid);
    const auto* p = up == entt::null ? nullptr : m_registry.try_get<TransformComponent>(up);
    TransformComponent parentDrawn;
    if (!p || !drawnPose(up, *p, parentDrawn, depth + 1)) return false;
    out = t;
    placeAt(parentDrawn, pc->local, out);
    return true;
}

// ------------------------------------------------------------------ Entity --
bool Entity::setParent(Entity parent) { return valid() && m_scene->setParent(*this, parent); }
Entity Entity::parent() const { return valid() ? m_scene->parentOf(*this) : Entity{}; }
std::vector<Entity> Entity::children() const { return valid() ? m_scene->childrenOf(*this) : std::vector<Entity>{}; }

} // namespace nova
