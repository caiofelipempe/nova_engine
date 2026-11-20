#pragma once

/// @file
/// @brief Inspector support for the engine's built-in components.

#include "nova/editor/Registry.h"

namespace nova::editor::builtin {

/// @brief Registers component editors for Transform, Mesh, Camera, lights, physics, Rotator and scripts.
///
/// Registered with Registry::component, exactly like a game registers its own components, so
/// any of them can be replaced (same name) or removed (Registry::removeComponentEditor).
class CoreComponentsPlugin final : public Plugin {
public:
    void onRegister(Registry& registry) override;
};

} // namespace nova::editor::builtin
