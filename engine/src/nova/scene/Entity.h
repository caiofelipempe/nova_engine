#pragma once

#include "nova/scene/Components.h"

#include <entt/entt.hpp>

namespace nova {

class Scene;

// Thin, copyable handle over an EnTT entity + its scene.
class Entity {
public:
    Entity() = default;
    Entity(entt::entity handle, Scene* scene) : m_handle(handle), m_scene(scene) {}

    template <typename T, typename... Args>
    T& add(Args&&... args) {
        return registry().emplace_or_replace<T>(m_handle, std::forward<Args>(args)...);
    }
    template <typename T>
    T& get() { return registry().get<T>(m_handle); }
    template <typename T>
    T* tryGet() { return registry().try_get<T>(m_handle); }
    template <typename T>
    bool has() const { return registry().all_of<T>(m_handle); }
    template <typename T>
    void remove() { registry().remove<T>(m_handle); }

    // Attaches a native C++ script (see Script.h). Defined in Script.h.
    template <typename S, typename... Args>
    S& addScript(Args&&... args);

    TransformComponent& transform() { return get<TransformComponent>(); }
    const std::string& name() const { return registry().get<NameComponent>(m_handle).name; }

    bool valid() const;
    explicit operator bool() const { return valid(); }
    entt::entity handle() const { return m_handle; }
    Scene* scene() const { return m_scene; }
    bool operator==(const Entity& o) const { return m_handle == o.m_handle && m_scene == o.m_scene; }

private:
    entt::registry& registry() const;

    entt::entity m_handle = entt::null;
    Scene* m_scene = nullptr;
};

} // namespace nova
