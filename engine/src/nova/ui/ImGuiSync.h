#pragma once

/// @file
/// @brief Keeps the Dear ImGui context shared by every module (exe, editor DLL, game DLLs).
///
/// Dear ImGui is linked statically, so each module has its own `GImGui` pointer. When Nova is a
/// shared library, `ImGuiModuleSync.cpp` is compiled into every module that links Nova::Engine and
/// registers a setter here; the engine calls all setters whenever its context changes, so ImGui
/// calls from any module draw into the same context. Nothing to do in game code.

namespace nova::detail {

/// @brief Sets the current ImGui context of one module (argument: `ImGuiContext*`).
using ImGuiContextSetter = void (*)(void* context);

/// @brief Registers a module's setter and calls it with the current context (if any).
void registerImGuiModule(ImGuiContextSetter setter);
/// @brief Removes a module's setter (module unloading).
void unregisterImGuiModule(ImGuiContextSetter setter);
/// @brief Sends the engine's context to every module (`nullptr` after destruction).
void broadcastImGuiContext(void* context);

} // namespace nova::detail
