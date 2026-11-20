#include "nova/editor/Widgets.h"

#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>

#include <cstdio>

namespace nova::editor::widgets {

bool vec3(const char* label, glm::vec3& value, float speed, float resetValue) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 90.0f);
    ImGui::TextUnformatted(label);
    ImGui::NextColumn();
    const float w = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
    const char* axes[3] = {"X", "Y", "Z"};
    const ImVec4 colors[3] = {{0.75f, 0.2f, 0.2f, 1}, {0.25f, 0.65f, 0.25f, 1}, {0.2f, 0.35f, 0.8f, 1}};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Border, colors[i]);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::SetNextItemWidth(w);
        char id[8];
        std::snprintf(id, sizeof(id), "##%s", axes[i]);
        changed |= ImGui::DragFloat(id, &value[i], speed, 0.0f, 0.0f, "%.2f");
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            value[i] = resetValue;
            changed = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    ImGui::Columns(1);
    ImGui::PopID();
    return changed;
}

bool material(Material& m) {
    bool changed = false;
    changed |= ImGui::ColorEdit4("Base color", glm::value_ptr(m.baseColor));
    changed |= ImGui::SliderFloat("Metallic", &m.metallic, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Roughness", &m.roughness, 0.0f, 1.0f);
    changed |= ImGui::ColorEdit3("Emissive", glm::value_ptr(m.emissive));
    changed |= ImGui::DragFloat("Emissive strength", &m.emissiveStrength, 0.1f, 0.0f, 100.0f);
    changed |= ImGui::Checkbox("Checker pattern", &m.checker);
    return changed;
}

bool componentHeader(const char* title, bool removable, bool& removeRequested, bool defaultOpen) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap;
    if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    const bool open = ImGui::CollapsingHeader(title, flags);
    if (removable) {
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 10.0f);
        ImGui::PushID(title);
        if (ImGui::SmallButton("x")) removeRequested = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove component");
        ImGui::PopID();
    }
    return open;
}

} // namespace nova::editor::widgets
