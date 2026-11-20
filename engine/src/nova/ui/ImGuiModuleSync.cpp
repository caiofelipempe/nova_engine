// Compiled into every module that links a shared Nova::Engine (see ImGuiSync.h): makes this
// module's copy of Dear ImGui use the engine's context.
#include "nova/ui/ImGuiSync.h"

#include <imgui.h>

namespace {

void setThisModuleContext(void* context) { ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context)); }

struct ImGuiModuleRegistrar {
    ImGuiModuleRegistrar() { nova::detail::registerImGuiModule(&setThisModuleContext); }
    ~ImGuiModuleRegistrar() { nova::detail::unregisterImGuiModule(&setThisModuleContext); }
} g_imguiModuleRegistrar;

} // namespace
