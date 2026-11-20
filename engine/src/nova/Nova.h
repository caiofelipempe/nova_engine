#pragma once

/// @file
/// @brief Nova Engine public API — the only header an application needs.
///
/// @code
/// #include <nova/Nova.h>
///
/// struct MyGame : nova::GameLayer {
///     void onStart() override { /* build the scene */ }
/// };
/// int main() { return nova::run<MyGame>({.name = "My Game"}); }
/// @endcode

#include "nova/core/Application.h"
#include "nova/core/Base.h"
#include "nova/core/Layer.h"
#include "nova/core/Log.h"

#include "nova/platform/Input.h"
#include "nova/platform/KeyCodes.h"
#include "nova/platform/Window.h"

#include "nova/rhi/Device.h"

#include "nova/physics/PhysicsWorld.h"

#include "nova/renderer/DebugDraw.h"
#include "nova/renderer/Mesh.h"
#include "nova/renderer/Renderer.h"

#include "nova/scene/Components.h"
#include "nova/scene/Entity.h"
#include "nova/scene/GameLayer.h"
#include "nova/scene/Scene.h"
#include "nova/scene/SceneSerializer.h"
#include "nova/scene/Script.h"

#include "nova/reflect/Reflect.h"

#include "nova/assets/AssetDatabase.h"
#include "nova/core/Module.h"
#include "nova/platform/DynamicLibrary.h"
#include "nova/assets/GltfLoader.h"

#include <imgui.h>

/// @brief Nova Engine.
///
/// Layers, from bottom to top:
/// - **core** — Application, Layer, logging (Log, `NOVA_*` macros).
/// - **platform** (nova::platform) — Window and Input, GLFW backend.
/// - **rhi** (nova::rhi) — graphics API abstraction (Device, CommandList), Vulkan backend.
/// - **physics** (nova::physics) — PhysicsWorld, Jolt backend.
/// - **renderer** — Renderer, RenderView, meshes and materials (talks only to the RHI).
/// - **scene** — Scene (EnTT), Entity, components, Script, GameLayer.
namespace nova {}
