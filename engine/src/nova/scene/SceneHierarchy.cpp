// The hierarchy of the scene: children follow their parents (ParentComponent). Transforms stay
// poses in the world; a child's offset from its parent is derived from them and kept here.
#include "nova/scene/Scene.h"

#include <algorithm>
#include <cmath>

namespace nova {

namespace {

// A scale component that can be divided by (a zero scale has no inverse).
glm::dvec3 invertible(glm::dvec3 s) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(s[i]) < 1e-12) s[i] = 1e-12;
    return s;
}

// The pose `t` as an offset from the parent pose `p`.
LocalTransform offsetFrom(const TransformComponent& p, const TransformComponent& t) {
    const glm::dquat turnBack = glm::conjugate(p.rotation.toDouble());
    const glm::dvec3 scale = invertible(p.scale.toDouble());
    LocalTransform l;
    l.position = (turnBack * (t.position - p.position).toMeters()) / scale; // the shortest way, across seams too
    l.rotation = glm::normalize(turnBack * t.rotation.toDouble());
    l.scale = t.scale.toDouble() / scale;
    return l;
}

// Places `t` at the offset `l` from the parent pose `p`.
void placeAt(const TransformComponent& p, const LocalTransform& l, TransformComponent& t) {
    const glm::dquat turn = p.rotation.toDouble();
    const glm::dvec3 scale = p.scale.toDouble();
    t.position = p.position + GlobalOffset::fromMeters(turn * (scale * l.position));
    t.rotation = Quat(turn * l.rotation);
    t.scale = ScalarVec(scale * l.scale);
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
    l.position = t.position.toMeters();
    l.rotation = t.rotation.toDouble();
    l.scale = t.scale.toDouble();
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
    t.position = GlobalPosition(local.position);
    t.rotation = Quat(local.rotation);
    t.scale = ScalarVec(local.scale);
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
