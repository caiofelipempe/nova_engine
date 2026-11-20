#include "ExamplePlugin.h"

#include <nova/editor/builtin/CoreCommands.h>

using namespace nova;
using namespace nova::editor;

void reflectGameTypes() {
    // Describing the types is all it takes: the inspector, "Add Component", scene files and
    // editor Play/Stop all work from this.
    reflect::type<ScoreTarget>("ScoreTarget")
        .field("points", &ScoreTarget::points, {.min = 0, .max = 1000})
        .field("active", &ScoreTarget::active)
        .component();

    reflect::type<Bob>("Bob")
        .field("amplitude", &Bob::amplitude, {.min = 0.0f, .max = 3.0f})
        .field("speed", &Bob::speed, {.speed = 0.05f, .tooltip = "Radians per second"})
        .script();
}

namespace {

// Panel: lists every ScoreTarget and lets you jump to it.
class ScoreTargetsPanel final : public Panel {
public:
    ScoreTargetsPanel() : Panel("Score Targets", DockSlot::RightBottom) {}

    // React to editor events through the context's signals (no coupling to other panels).
    void onAttach(EditorContext& ctx) override {
        m_connection = ctx.playStateChanged.connect([&ctx](PlayState state) {
            if (state == PlayState::Play)
                NOVA_INFO("Play started with {} score targets", ctx.scene().view<ScoreTarget>().size());
        });
    }
    void onDetach(EditorContext& ctx) override { ctx.playStateChanged.disconnect(m_connection); }

    void onImGui(EditorContext& ctx) override {
        int total = 0;
        for (auto [e, target, name] : ctx.scene().view<ScoreTarget, NameComponent>().each()) {
            if (target.active) total += target.points;
            Entity entity{e, &ctx.scene()};
            ImGui::PushID(static_cast<int>(entt::to_integral(e)));
            if (ImGui::Selectable(name.name.c_str(), ctx.selected() == entity)) {
                ctx.select(entity);
                ctx.camera().focus(entity.transform().position);
            }
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40.0f);
            ImGui::TextDisabled("%d pts", target.points);
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::Text("Total: %d points", total);
    }

private:
    u32 m_connection = 0;
};

} // namespace

void ExamplePlugin::onRegister(Registry& r) {
    r.panel<ScoreTargetsPanel>();

    // Command: menu entry + shortcut + command palette, in one line.
    r.command("Tools/Spawn Score Target", [](EditorContext& ctx) {
        Entity e = builtin::createMeshEntity(ctx, "Target", ctx.renderer().sphere());
        e.get<MeshComponent>().material.baseColor = {1.0f, 0.85f, 0.1f, 1.0f};
        e.add<ScoreTarget>();
        e.addScript<Bob>();
    }, "Ctrl+T");
}
