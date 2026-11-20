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

// Tool: drag over any surface to paint score targets. One stroke = one undo step.
class PaintTargetsTool final : public Tool {
public:
    PaintTargetsTool() : Tool("Paint Targets", "B") {}

    bool clickSelects() const override { return false; } // the left button paints
    bool isInteracting() const override { return m_stroke.active(); }
    void onDeactivate(EditorContext&) override { m_stroke.commit(); }

    void onViewport(EditorContext& ctx, const ViewportState& vp) override {
        // What surface is under the brush? (ignore the targets we are painting)
        Renderer& renderer = ctx.renderer();
        PickOptions options;
        options.markerRadius = 0.0f;
        options.ignore = [](Entity e) { return e.has<ScoreTarget>(); };
        m_hover = vp.hovered ? pick(ctx.scene(), [&](MeshHandle m) { return renderer.meshData(m); }, vp.rayOrigin,
                                    vp.rayDirection, options)
                             : std::nullopt;

        if (vp.hovered && !ctx.inputCaptured() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_stroke = ctx.beginCreation("Paint Targets");
            m_last.reset();
        }
        if (m_stroke.active()) {
            if (m_hover && (!m_last || glm::distance(*m_last, m_hover->point) >= spacing)) {
                Entity e = builtin::createMeshEntity(ctx, "Target", renderer.sphere());
                e.transform().position = m_hover->point + m_hover->normal * 0.3f;
                e.transform().scale = glm::vec3(0.6f);
                e.get<MeshComponent>().material.baseColor = {1.0f, 0.85f, 0.1f, 1.0f};
                e.add<ScoreTarget>();
                m_last = m_hover->point;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const auto painted = m_stroke.commit();
                NOVA_INFO("Painted {} score targets (Ctrl+Z undoes the stroke)", painted.size());
            }
        }

        ImGui::SetCursorScreenPos({vp.min.x + 8.0f, vp.min.y + 40.0f});
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Spacing", &spacing, 0.5f, 5.0f, "%.1f m");
    }

    void onDebugDraw(EditorContext&, DebugDraw& dd) override {
        if (!m_hover) return;
        const glm::vec4 color = m_stroke.active() ? glm::vec4(1.0f, 0.4f, 0.1f, 1.0f) : glm::vec4(1.0f, 0.85f, 0.1f, 1.0f);
        dd.circle(m_hover->point + m_hover->normal * 0.02f, m_hover->normal, spacing * 0.5f, color, 40, true);
        dd.arrow(m_hover->point, m_hover->point + m_hover->normal * 0.8f, color, true);
    }

    float spacing = 1.5f; ///< Minimum distance between painted targets.

private:
    CreationTransaction m_stroke;
    std::optional<PickHit> m_hover;
    std::optional<glm::vec3> m_last;
};

} // namespace

void ExamplePlugin::onRegister(Registry& r) {
    r.panel<ScoreTargetsPanel>();
    r.tool<PaintTargetsTool>();

    // Command: menu entry + shortcut + command palette, in one line.
    // recordCreation makes it undoable (Ctrl+Z removes the target again).
    r.command("Tools/Spawn Score Target", [](EditorContext& ctx) {
        ctx.recordCreation("Spawn Score Target", [&] {
            Entity e = builtin::createMeshEntity(ctx, "Target", ctx.renderer().sphere());
            e.get<MeshComponent>().material.baseColor = {1.0f, 0.85f, 0.1f, 1.0f};
            e.add<ScoreTarget>();
            e.addScript<Bob>();
        });
    }, "Ctrl+T");
}
