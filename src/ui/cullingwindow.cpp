#include "cullingwindow.h"

#include <algorithm>
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

    const char* views[] = {"camera", "cascade 0", "cascade 1", "cascade 2", "cascade 3"};
    ImGui::SetNextItemWidth(140.0f);
    ImGui::Combo("Overlay view", &in.overlayView, views, 1 + (int) std::min(in.cascades, maxShadowCascades));
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Which view's culling colours the AABB overlay: the camera, or one shadow cascade.\nRead back from the GPU culling, a few frames late.");
    }
    ImGui::Checkbox("Cascade frusta", &in.showCascadeFrusta);

    ImGui::Separator();
    uint32_t drawn = in.instances - in.culled;
    float percent = in.instances > 0 ? 100.0f * (float) in.culled / (float) in.instances : 0.0f;
    ImGui::Text("Instances: %u", in.instances);
    ImGui::Text("Drawn: %u", drawn);
    ImGui::Text("Culled: %u (%.0f%%)", in.culled, percent);
    if (!in.enabled) {
        ImGui::TextDisabled("Culling off: everything is drawn.");
    }

    ImGui::Separator();
    ImGui::Text("Shadow cascades: %u", in.cascades);
    for (uint32_t c = 0; c < in.cascades && c < maxShadowCascades; c++) {
        ImGui::Text("  cascade %u: %u drawn, %u culled", c, in.shadowDrawn[c], in.shadowCulled[c]);
    }

    ImGui::End();
}
