#pragma once

#include "nova/scene/Entity.h"

#include <vector>

namespace nova {

// A contact as seen from the script's own entity ("self").
struct Collision {
    Entity other;
    glm::vec3 normal{0.0f};             // contact normal, from `other` towards self (direction self is pushed out)
    glm::vec3 point{0.0f};              // contact point on self's surface
    glm::vec3 otherPoint{0.0f};         // contact point on the other entity's surface
    glm::vec3 surfaceNormal{0.0f};      // self's surface normal at `point`
    glm::vec3 otherSurfaceNormal{0.0f}; // other's surface normal at `otherPoint`
};

// Native gameplay script attached to an entity:
//
//   struct Spinner : nova::Script {
//       void onUpdate(float dt) override { transform().rotation.y += dt; }
//   };
//   entity.addScript<Spinner>();
class Script {
public:
    virtual ~Script() = default;
    virtual void onCreate() {}
    virtual void onUpdate(f32 /*dt*/) {}
    virtual void onDestroy() {}
    // Called when this entity's rigid body starts touching another one.
    virtual void onCollision(const Collision& /*collision*/) {}

    Entity entity() const { return m_entity; }
    TransformComponent& transform() { return m_entity.transform(); }
    template <typename T>
    T& get() { return m_entity.get<T>(); }
    Scene& scene() { return *m_entity.scene(); }

private:
    friend class Scene;
    friend class Entity;
    Entity m_entity;
    bool m_created = false;
};

struct ScriptComponent {
    std::vector<Scope<Script>> scripts;

    ScriptComponent() = default;
    ScriptComponent(ScriptComponent&&) noexcept = default;
    ScriptComponent& operator=(ScriptComponent&&) noexcept = default;
    ScriptComponent(const ScriptComponent&) = delete; // std::vector<unique_ptr> lies about copyability
    ScriptComponent& operator=(const ScriptComponent&) = delete;
};

template <typename S, typename... Args>
S& Entity::addScript(Args&&... args) {
    static_assert(std::is_base_of_v<Script, S>, "S must derive from nova::Script");
    auto& sc = registry().get_or_emplace<ScriptComponent>(m_handle);
    auto script = makeScope<S>(std::forward<Args>(args)...);
    S& ref = *script;
    ref.m_entity = *this;
    sc.scripts.push_back(std::move(script));
    return ref;
}

} // namespace nova
