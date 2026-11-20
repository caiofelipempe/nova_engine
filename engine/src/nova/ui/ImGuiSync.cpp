#include "nova/ui/ImGuiSync.h"

#include <algorithm>
#include <vector>

namespace nova::detail {

namespace {
struct State {
    std::vector<ImGuiContextSetter> setters;
    void* context = nullptr;
};
State& state() {
    static State s;
    return s;
}
} // namespace

void registerImGuiModule(ImGuiContextSetter setter) {
    State& s = state();
    if (std::find(s.setters.begin(), s.setters.end(), setter) == s.setters.end()) s.setters.push_back(setter);
    if (s.context) setter(s.context);
}

void unregisterImGuiModule(ImGuiContextSetter setter) { std::erase(state().setters, setter); }

void broadcastImGuiContext(void* context) {
    State& s = state();
    s.context = context;
    for (ImGuiContextSetter setter : s.setters) setter(context);
}

} // namespace nova::detail
