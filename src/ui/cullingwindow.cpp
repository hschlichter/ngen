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
    auto culledTriangles = in.trianglesScene > in.trianglesDrawn ? in.trianglesScene - in.trianglesDrawn : 0;
    float trianglePercent = in.trianglesScene > 0 ? 100.0f * (float) culledTriangles / (float) in.trianglesScene : 0.0f;
    if (ImGui::BeginTable("cullcounts", 3, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("instances");
        ImGui::TableSetupColumn("triangles");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Scene");
        ImGui::TableNextColumn();
        ImGui::Text("%u", in.instances);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long) in.trianglesScene);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Drawn");
        ImGui::TableNextColumn();
        ImGui::Text("%u", drawn);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long) in.trianglesDrawn);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Culled");
        ImGui::TableNextColumn();
        ImGui::Text("%u (%.0f%%)", in.culled, percent);
        ImGui::TableNextColumn();
        ImGui::Text("%llu (%.0f%%)", (unsigned long long) culledTriangles, trianglePercent);
        ImGui::EndTable();
    }
    if (!in.enabled) {
        ImGui::TextDisabled("Culling off: everything is drawn.");
    }

    ImGui::Separator();
    ImGui::Text("Shadow cascades: %u", in.cascades);
    if (in.cascades > 0 && ImGui::BeginTable("cascadecounts", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("cascade");
        ImGui::TableSetupColumn("drawn");
        ImGui::TableSetupColumn("culled");
        ImGui::TableSetupColumn("triangles");
        ImGui::TableHeadersRow();
        for (uint32_t c = 0; c < in.cascades && c < maxShadowCascades; c++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", c);
            ImGui::TableNextColumn();
            ImGui::Text("%u", in.shadowDrawn[c]);
            ImGui::TableNextColumn();
            ImGui::Text("%u", in.shadowCulled[c]);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long) in.shadowTriangles[c]);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
