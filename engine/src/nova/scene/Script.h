#pragma once

/// @file
/// @brief Native C++ gameplay scripts.

#include "nova/scene/Entity.h"

#include <vector>

namespace nova {

/// @brief A contact as seen from the script's own entity ("self").
/// @see physics::ContactEvent for the symmetric, backend-level event.
struct Collision {
    Entity other;                       ///< The entity that was touched.
    glm::vec3 normal{0.0f};             ///< Contact normal, from `other` towards self (direction self is pushed out).
    GlobalPosition point;               ///< Contact point on self's surface.
    GlobalPosition otherPoint;          ///< Contact point on the other entity's surface.
    glm::vec3 surfaceNormal{0.0f};      ///< Self's surface normal at `point` (its face that touched).
    glm::vec3 otherSurfaceNormal{0.0f}; ///< Other's surface normal at `otherPoint`.
};

/// @brief A body touched a barrier edge of the world (see WorldSpace, WorldBoundary::Barrier).
struct WorldBoundaryHit {
    int axis = 0;           ///< 0 = X, 1 = Y, 2 = Z.
    int side = 0;           ///< −1 = the low edge, +1 = the high edge.
    GlobalPosition point;   ///< Contact point on the body.
    glm::vec3 normal{0.0f}; ///< Direction the body is pushed back into the world.
};

/// @brief Native gameplay script attached to an entity.
///
/// Scripts run inside Scene::update(): onCreate() before the first onUpdate(), then onUpdate()
/// every frame. An entity may have several scripts, run in the order they were added.
/// @code
/// struct Spinner : nova::Script {
///     void onUpdate(float dt) override { transform().rotateLocal(dt, {0, 1, 0}); }
///     void onCollision(const nova::Collision& c) override { NOVA_INFO("hit {}", c.other.name()); }
/// };
/// entity.addScript<Spinner>();
/// @endcode
class Script {
public:
    virtual ~Script() = default;
    /// @brief Called once, on the first Scene::update() after the script was added.
    virtual void onCreate() {}
    /// @brief Called every Scene::update(), before built-in systems and physics.
    /// @param dt Seconds since the previous update.
    virtual void onUpdate(f32 /*dt*/) {}
    /// @brief Called when the entity is destroyed or the scene is cleared (only if onCreate ran).
    virtual void onDestroy() {}
    /// @brief Called when this entity's rigid body starts touching another one (not sensors).
    virtual void onCollision(const Collision& /*collision*/) {}
    /// @brief Called when this entity's rigid body stops touching `other` (also when either one is
    ///        destroyed or stops simulating; `other` is then possibly invalid).
    virtual void onCollisionEnd(Entity /*other*/) {}
    /// @brief A sensor and a body started overlapping: called on both (`other` is the other one).
    virtual void onTriggerEnter(Entity /*other*/) {}
    /// @brief A sensor and a body stopped overlapping (or one was destroyed: `other` may be invalid).
    virtual void onTriggerExit(Entity /*other*/) {}
    /// @brief Called when this entity's rigid body touches a barrier edge of the world
    ///        (e.g. destroy whatever falls off the map).
    virtual void onWorldBoundary(const WorldBoundaryHit& /*hit*/) {}

    /// @brief The entity this script is attached to.
    Entity entity() const { return m_entity; }
    /// @brief Shortcut for `entity().transform()`.
    TransformComponent& transform() { return m_entity.transform(); }
    /// @brief Shortcut for `entity().get<T>()`.
    template <typename T>
    T& get() { return m_entity.get<T>(); }
    /// @brief The scene the entity lives in (queries, spawning, physics helpers).
    Scene& scene() { return *m_entity.scene(); }

private:
    friend class Scene;
    friend class Entity;
    Entity m_entity;
    bool m_created = false;
};

/// @brief Holds the scripts of an entity. Managed by Entity::addScript; move-only.
struct ScriptComponent {
    std::vector<Scope<Script>> scripts; ///< Scripts in execution order.

    ScriptComponent() = default;
    ScriptComponent(ScriptComponent&&) noexcept = default;
    ScriptComponent& operator=(ScriptComponent&&) noexcept = default;
    ScriptComponent(const ScriptComponent&) = delete; // std::vector<unique_ptr> lies about copyability
    ScriptComponent& operator=(const ScriptComponent&) = delete;
};

template <typename S, typename... Args>
S& Entity::addScript(Args&&... args) {
    static_assert(std::is_base_of_v<Script, S>, "S must derive from nova::Script");
    return static_cast<S&>(attachScript(makeScope<S>(std::forward<Args>(args)...)));
}

inline Script& Entity::attachScript(Scope<Script> script) {
    auto& sc = registry().get_or_emplace<ScriptComponent>(m_handle);
    Script& ref = *script;
    ref.m_entity = *this;
    ref.m_created = false;
    sc.scripts.push_back(std::move(script));
    return ref;
}

} // namespace nova
