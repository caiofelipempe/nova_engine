#include "nova/ui/ImGuiTheme.h"

#include <imgui.h>

#include <filesystem>

namespace nova::ui {

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.PopupRounding = 3.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.FramePadding = {6.0f, 4.0f};
    s.ItemSpacing = {8.0f, 5.0f};

    ImVec4* c = s.Colors;
    const ImVec4 bg{0.11f, 0.115f, 0.125f, 1.0f};
    const ImVec4 panel{0.15f, 0.155f, 0.17f, 1.0f};
    const ImVec4 accent{0.26f, 0.52f, 0.96f, 1.0f};
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg;
    c[ImGuiCol_PopupBg] = {0.09f, 0.095f, 0.10f, 0.98f};
    c[ImGuiCol_Border] = {0.22f, 0.23f, 0.25f, 1.0f};
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = {0.20f, 0.21f, 0.23f, 1.0f};
    c[ImGuiCol_FrameBgActive] = {0.24f, 0.25f, 0.28f, 1.0f};
    c[ImGuiCol_TitleBg] = {0.08f, 0.085f, 0.09f, 1.0f};
    c[ImGuiCol_TitleBgActive] = {0.10f, 0.105f, 0.115f, 1.0f};
    c[ImGuiCol_MenuBarBg] = {0.09f, 0.095f, 0.10f, 1.0f};
    c[ImGuiCol_Header] = {0.20f, 0.22f, 0.26f, 1.0f};
    c[ImGuiCol_HeaderHovered] = {0.24f, 0.30f, 0.40f, 1.0f};
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Button] = {0.20f, 0.21f, 0.24f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.26f, 0.32f, 0.44f, 1.0f};
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = {0.36f, 0.60f, 1.0f, 1.0f};
    c[ImGuiCol_Tab] = {0.12f, 0.125f, 0.135f, 1.0f};
    c[ImGuiCol_TabHovered] = {0.26f, 0.32f, 0.44f, 1.0f};
    c[ImGuiCol_TabSelected] = {0.17f, 0.20f, 0.26f, 1.0f};
    c[ImGuiCol_TabDimmed] = {0.10f, 0.105f, 0.115f, 1.0f};
    c[ImGuiCol_TabDimmedSelected] = {0.14f, 0.15f, 0.17f, 1.0f};
    c[ImGuiCol_DockingPreview] = {0.26f, 0.52f, 0.96f, 0.6f};
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_ResizeGripHovered] = accent;
}

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
#if defined(_WIN32)
    const char* path = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(path)) {
        io.Fonts->AddFontFromFileTTF(path, 17.0f);
        return;
    }
#endif
    io.Fonts->AddFontDefault();
}

} // namespace nova::ui
