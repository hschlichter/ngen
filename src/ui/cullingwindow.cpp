#include "cullingwindow.h"

#include <imgui.h>

void drawCullingWindow(bool& show, CullingWindowInputs in) {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(320, 200), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Culling", &show)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("Frustum culling", &in.enabled);
    ImGui::Checkbox("Freeze frustum", &in.frozen);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Keeps culling against the camera pose at the moment of freezing while the view moves on.\nThe frozen frustum is drawn in yellow.");
    }
    ImGui::Checkbox("Show culled", &in.showCulled);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Every instance AABB: green drawn, red culled.");
    }

    ImGui::Separator();
    uint32_t drawn = in.instances - in.culled;
    float percent = in.instances > 0 ? 100.0f * (float) in.culled / (float) in.instances : 0.0f;
    ImGui::Text("Instances: %u", in.instances);
    ImGui::Text("Drawn: %u", drawn);
    ImGui::Text("Culled: %u (%.0f%%)", in.culled, percent);
    if (!in.enabled) {
        ImGui::TextDisabled("Culling off: everything is drawn.");
    }

    ImGui::End();
}
