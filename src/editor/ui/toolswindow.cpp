#include "toolswindow.h"

#include <imgui.h>

void drawToolsWindow(bool& show, EditorTool& activeTool) {
    if (!show) {
        return;
    }

    ImGui::Begin("Tools", &show);

    auto toolButton = [&](const char* shortcut, const char* label, EditorTool tool, bool implemented) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", shortcut);
        ImGui::SameLine();
        bool isActive = activeTool == tool;
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(label, ImVec2(80, 0))) {
            activeTool = tool;
        }
        if (isActive) {
            ImGui::PopStyleColor();
        }
        if (!implemented && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Not yet implemented");
        }
    };

    toolButton("1", "Select", EditorTool::Select, true);
    ImGui::SameLine();
    toolButton("2", "Translate", EditorTool::Translate, true);
    ImGui::SameLine();
    toolButton("3", "Rotate", EditorTool::Rotate, true);
    ImGui::SameLine();
    toolButton("4", "Scale", EditorTool::Scale, true);

    ImGui::End();
}
