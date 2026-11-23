#pragma once

/// @file
/// @brief Lightweight entity handle.

#include "nova/scene/Components.h"

#include <entt/entt.hpp>

#include <vector>

namespace nova {

class Scene;

/// @brief Thin, copyable handle over an EnTT entity and the Scene it belongs to.
///
/// Holding an Entity does not keep it alive; check valid() if it may have been destroyed.
/// @code
/// auto e = scene.createEntity("Lamp");
/// e.transform().position = {0, 2, 0};
/// e.add<nova::PointLightComponent>();
/// if (auto* light = e.tryGet<nova::PointLightComponent>()) light->intensity = 20.0f;
/// @endcode
class Entity {
public:
    /// @brief An invalid entity.
    Entity() = default;
    /// @brief Wraps an existing EnTT entity of `scene`.
    Entity(entt::entity handle, Scene* scene) : m_handle(handle), m_scene(scene) {}

    /// @brief Adds (or replaces) a component.
    /// @tparam T Component type.
    /// @param args Constructor / aggregate-initialization arguments.
    /// @return Reference to the stored component (invalidated if more `T` components are added elsewhere).
    template <typename T, typename... Args>
    T& add(Args&&... args) {
        return registry().emplace_or_replace<T>(m_handle, std::forward<Args>(args)...);
    }
    /// @brief The component of type `T`. @pre has<T>().
    template <typename T>
    T& get() { return registry().get<T>(m_handle); }
    /// @brief The component of type `T`, or `nullptr` if absent.
    template <typename T>
    T* tryGet() { return registry().try_get<T>(m_handle); }
    /// @brief True if the entity has a `T` component.
    template <typename T>
    bool has() const { return registry().all_of<T>(m_handle); }
    /// @brief Removes the `T` component. @pre has<T>().
    template <typename T>
    void remove() { registry().remove<T>(m_handle); }

    /// @brief Attaches a native C++ script (see Script).
    /// @tparam S Script type, derived from Script.
    /// @param args Arguments forwarded to `S`'s constructor.
    /// @return Reference to the script, owned by the entity.
    template <typename S, typename... Args>
    S& addScript(Args&&... args); // defined in Script.h

    /// @brief Attaches an already created script (type-erased version of addScript; used by
    ///        serialization and tools). Defined in Script.h.
    /// @return Reference to the script, owned by the entity.
    Script& attachScript(Scope<Script> script);

    /// @brief The entity's stable UUID (IdComponent), 0 if it has none.
    u64 uuid() const {
        const auto* id = registry().try_get<IdComponent>(m_handle);
        return id ? id->uuid : 0;
    }

    /// @brief Shortcut for `get<TransformComponent>()`.
    TransformComponent& transform() { return get<TransformComponent>(); }
    /// @brief Makes this entity a child of `parent` (an invalid one: a root), keeping its pose in
    ///        the world. False (and nothing changes) if `parent` is this entity or one of its descendants.
    bool setParent(Entity parent);
    /// @brief The parent, or an invalid Entity for a root.
    Entity parent() const;
    /// @brief The direct children, in creation order.
    std::vector<Entity> children() const;
    /// @brief The entity's NameComponent value.
    const std::string& name() const { return registry().get<NameComponent>(m_handle).name; }

    /// @brief True if the entity exists in its scene.
    bool valid() const;
    explicit operator bool() const { return valid(); }
    /// @brief The underlying EnTT entity.
    entt::entity handle() const { return m_handle; }
    /// @brief The owning scene (`nullptr` for default-constructed entities).
    Scene* scene() const { return m_scene; }
    bool operator==(const Entity& o) const { return m_handle == o.m_handle && m_scene == o.m_scene; }

private:
    entt::registry& registry() const;

    entt::entity m_handle = entt::null;
    Scene* m_scene = nullptr;
};

} // namespace nova
