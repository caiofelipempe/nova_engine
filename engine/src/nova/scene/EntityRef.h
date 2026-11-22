#pragma once

/// @file
/// @brief A reference to another entity that survives saving, loading and Play/Stop.

#include "nova/core/Base.h"

namespace nova {

/// @brief Refers to an entity by its IdComponent UUID (0 = none).
///
/// Use it in components and scripts instead of an Entity handle when the reference must be saved
/// (it is a reflected field kind: the inspector shows an entity picker). Resolve it with
/// Scene::resolve(). Prefab instances get their references remapped to the new copies.
struct EntityRef {
    u64 uuid = 0; ///< IdComponent::uuid of the target; 0 = no entity.
    explicit operator bool() const { return uuid != 0; }
    bool operator==(const EntityRef&) const = default;
};

} // namespace nova
