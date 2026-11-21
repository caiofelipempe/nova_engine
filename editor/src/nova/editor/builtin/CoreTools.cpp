#include "nova/editor/builtin/CoreTools.h"

#include "nova/editor/EditorContext.h"

#include <imgui.h> // must precede ImGuizmo.h
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace nova::editor::builtin {

bool TransformTool::localSpace = false;

namespace {
// `m` has its translation measured from `origin` (the camera).
void applyMatrix(TransformComponent& t, const glm::mat4& m, const GlobalPosition& origin) {
    glm::vec3 scale, translation, skew;
    glm::vec4 persp;
    glm::quat rot;
    if (!glm::decompose(m, scale, rot, translation, skew, persp)) return;
    // Keep the exact position when the gizmo did not move it (rotate/scale): no float round trip.
    if (translation != glm::vec3((t.position - origin).toVec3())) t.position = origin + translation;
    t.rotation = glm::eulerAngles(rot);
    t.scale = scale;
}
} // namespace

TransformTool::TransformTool(Mode mode, std::string name, std::string shortcut)
    : Tool(std::move(name), std::move(shortcut)), m_mode(mode) {}

bool TransformTool::isInteracting() const { return ImGuizmo::IsUsing(); }

void TransformTool::onViewport(EditorContext& ctx, const ViewportState& vp) {
    Entity primary = ctx.selected();
    if (!primary.valid()) return;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(vp.min.x, vp.min.y, vp.size.x, vp.size.y);
    glm::mat4 view = vp.camera.view;
    glm::mat4 proj = vp.camera.projection;
    proj[1][1] *= -1.0f; // ImGuizmo expects GL-style Y-up clip space

    // Camera-relative matrices: the view is relative to vp.camera.origin, so are the models.
    const GlobalPosition& origin = vp.camera.origin;
    const glm::mat4 before = primary.transform().matrixRelativeTo(origin);
    glm::mat4 model = before;
    const ImGuizmo::OPERATION op = m_mode == Mode::Translate ? ImGuizmo::TRANSLATE
                                   : m_mode == Mode::Rotate  ? ImGuizmo::ROTATE
                                                             : ImGuizmo::SCALE;
    const bool snap = ImGui::GetIO().KeyCtrl;
    const float s = m_mode == Mode::Rotate ? 15.0f : 0.5f;
    const float snaps[3] = {s, s, s};
    if (!ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op,
                              localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD, glm::value_ptr(model), nullptr,
                              snap ? snaps : nullptr))
        return;

    // Same world-space delta for every selected entity (pivot = primary).
    const glm::mat4 delta = model * glm::inverse(before);
    Scene& scene = ctx.scene();
    for (Entity e : ctx.selection()) {
        if (!e.valid()) continue;
        auto& t = e.transform();
        applyMatrix(t, e == primary ? model : delta * t.matrixRelativeTo(origin), origin);
        if (scene.isSimulating()) scene.syncTransformToPhysics(e);
    }
}

void CoreToolsPlugin::onRegister(Registry& r) {
    r.tool<TransformTool>(TransformTool::Mode::Translate, "Move", "W");
    r.tool<TransformTool>(TransformTool::Mode::Rotate, "Rotate", "E");
    r.tool<TransformTool>(TransformTool::Mode::Scale, "Scale", "R");

    Command local;
    local.id = "tool.localSpace";
    local.menuPath = "View/Tools/Local Space";
    local.separatorBefore = true;
    local.order = 100;
    local.execute = [](EditorContext&) { TransformTool::localSpace = !TransformTool::localSpace; };
    local.isChecked = [](EditorContext&) { return TransformTool::localSpace; };
    r.command(std::move(local));
}

} // namespace nova::editor::builtin
