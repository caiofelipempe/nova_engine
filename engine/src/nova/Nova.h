#pragma once

// Nova Engine public API — the only header an application needs.
//
//   #include <nova/Nova.h>
//
//   struct MyGame : nova::GameLayer {
//       void onStart() override { /* build the scene */ }
//   };
//   int main() { return nova::run<MyGame>({.name = "My Game"}); }

#include "nova/core/Application.h"
#include "nova/core/Base.h"
#include "nova/core/Layer.h"
#include "nova/core/Log.h"

#include "nova/platform/Input.h"
#include "nova/platform/KeyCodes.h"
#include "nova/platform/Window.h"

#include "nova/rhi/Device.h"

#include "nova/physics/PhysicsWorld.h"

#include "nova/renderer/Mesh.h"
#include "nova/renderer/Renderer.h"

#include "nova/scene/Components.h"
#include "nova/scene/Entity.h"
#include "nova/scene/GameLayer.h"
#include "nova/scene/Scene.h"
#include "nova/scene/Script.h"

#include "nova/assets/GltfLoader.h"

#include <imgui.h>
