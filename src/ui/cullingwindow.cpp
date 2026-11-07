#include "cullingwindow.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <imgui.h>

namespace {

auto helpMarker(const char* text) -> void {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

auto percentOf(uint64_t part, uint64_t whole) -> float {
    return whole > 0 ? 100.0f * (float) part / (float) whole : 0.0f;
}

} // namespace

void drawCullingWindow(bool& show, CullingWindowInputs in) {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Culling", &show)) {
        ImGui::End();
        return;
    }
    const auto& cull = in.cull;

    ImGui::Checkbox("Frustum culling", &in.enabled);
    ImGui::Checkbox("Freeze frustum", &in.frozen);
    helpMarker("Keeps culling against the camera pose at the moment of freezing while the view moves on.\nThe frozen frustum is drawn in yellow.");
    ImGui::Checkbox("Occlusion culling", &in.occlusion);
    helpMarker("Two phases: what was visible last frame is drawn, a depth pyramid is built from its depth, and everything else\n"
               "is tested against it and drawn if it shows. By default the geometry pass draws both phases; with the depth\n"
               "prepass on, the prepass does, and the geometry pass tests every meshlet against a pyramid of its whole depth.\n"
               "While the culling camera isn't the rendered one, the phases draw from it into a depth of their own.\n"
               "Off while culling is off.");
    ImGui::Checkbox("Show culled", &in.showCulled);
    helpMarker("Every instance AABB: green drawn, red culled (frustum or occlusion) in the overlay view.");

    const char* views[] = {"camera", "cascade 0", "cascade 1", "cascade 2", "cascade 3"};
    ImGui::SetNextItemWidth(140.0f);
    ImGui::Combo("Overlay view", &in.overlayView, views, 1 + (int) std::min(cull.cascadeCount, maxShadowCascades));
    helpMarker("Which view's culling colours the AABB overlay: the camera, or one shadow cascade.\nRead back from the GPU culling, a few frames late.");
    ImGui::Checkbox("Cascade frusta", &in.showCascadeFrusta);
    ImGui::Checkbox("Colour meshlets by culling", &in.meshletCullView);
    helpMarker("The meshletcull debug view: every meshlet of the camera's drawn instances, green kept, red outside the frustum,\n"
               "blue facing away (normal cone), magenta hidden by depth, also seen through what hides it.\n"
               "Freeze the frustum and move to see frustum and cone culling from outside. Instances culled whole aren't drawn.");

    // The camera, step by step: the scene, what instance culling keeps (the meshlets the task shaders test), what meshlet culling draws.
    ImGui::SeparatorText("Camera");
    const auto& meshlets = cull.cameraMeshlets;
    if (ImGui::BeginTable("camerasteps", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("instances");
        ImGui::TableSetupColumn("meshlets");
        ImGui::TableSetupColumn("triangles");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Scene");
        ImGui::TableNextColumn();
        ImGui::Text("%u", cull.instances);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long) cull.sceneMeshlets);
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long) cull.sceneTriangles);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Instance culling kept");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Instances drawn (both of occlusion's phases), the meshlets their task shaders test, and their triangles.");
        }
        ImGui::TableNextColumn();
        ImGui::Text("%u", cull.cameraDrawn);
        ImGui::TableNextColumn();
        ImGui::Text("%u", meshlets.tested);
        ImGui::TableNextColumn();
        ImGui::Text("%llu (%.0f%%)", (unsigned long long) cull.cameraInstanceTriangles, percentOf(cull.cameraInstanceTriangles, cull.sceneTriangles));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Drawn");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("What the geometry pass's task shaders kept of those meshlets, and their triangles.");
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        ImGui::Text("%u", meshlets.drawn);
        ImGui::TableNextColumn();
        ImGui::Text("%llu (%.0f%%)", (unsigned long long) cull.cameraTriangles, percentOf(cull.cameraTriangles, cull.sceneTriangles));
        ImGui::EndTable();
    }
    ImGui::Text("Instances culled: %u frustum, %u occluded, %u LOD level not drawn", cull.cameraCulled, cull.occluded, cull.lodDropped);
    helpMarker("Frustum: outside the camera's frustum. Occluded: hidden by the depth pyramid in occlusion's second phase (one drawn\n"
               "in the first phase, visible last frame, counts as drawn too). LOD level not drawn: slots of render-LOD levels\n"
               "their group doesn't draw this frame (the GPU picked another level); in no view.");
    ImGui::Text("Meshlets culled: %u frustum, %u cone, %u depth", meshlets.frustumCulled, meshlets.coneCulled, meshlets.occluded);
    helpMarker("Up to 64 vertices and 124 triangles each, culled by the geometry pass's task shaders: the meshlet's sphere against\n"
               "the frustum; the normal cone, when every triangle faces away (single-sided surfaces); and with occlusion on, the\n"
               "sphere's box against the depth pyramid (the second phase's meshlets, or every meshlet after a depth prepass).");
    if (!in.enabled) {
        ImGui::TextDisabled("Culling off: everything is drawn.");
    }
    if (cull.occlusion) {
        ImGui::Text("Occlusion: %u instances drawn first (visible last frame), %u second", cull.earlyDrawn, cull.lateDrawn);
        const char* depthFrom = "the rendered camera's geometry pass";
        if (cull.occlusionCullCamera) {
            depthFrom = "the culling camera's own";
        } else if (in.depthPrepass) {
            depthFrom = "the rendered camera's depth prepass";
        }
        ImGui::Text("Occlusion depth: %s", depthFrom);
        helpMarker("While the culling camera isn't the rendered one (a debug camera, a frozen frustum), occlusion's depth is drawn from\n"
                   "the culling camera into a depth buffer of its own (depth.cull), so what is culled is what that camera would cull;\n"
                   "the rendered view then draws without a depth prepass.");
    } else {
        ImGui::TextDisabled("Occlusion: off this frame");
    }

    // The cascades: every instance (a prim's submesh), culled by its box, its meshlets frustum-tested by the shadow pass.
    ImGui::SeparatorText("Shadow cascades");
    if (cull.cascadeCount == 0) {
        ImGui::TextDisabled("No cascades this frame.");
    } else if (ImGui::BeginTable("cascadesteps", 7, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("cascade");
        ImGui::TableSetupColumn("instances");
        ImGui::TableSetupColumn("culled");
        ImGui::TableSetupColumn("meshlets");
        ImGui::TableSetupColumn("culled");
        ImGui::TableSetupColumn("kept tris");
        ImGui::TableSetupColumn("drawn tris");
        ImGui::TableHeadersRow();
        for (uint32_t c = 0; c < cull.cascadeCount && c < maxShadowCascades; c++) {
            const auto& m = cull.cascadeMeshlets[c];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%u", c);
            ImGui::TableNextColumn();
            ImGui::Text("%u", cull.cascadeDrawn[c]);
            ImGui::TableNextColumn();
            ImGui::Text("%u", cull.cascadeCulled[c]);
            ImGui::TableNextColumn();
            ImGui::Text("%u", m.drawn);
            ImGui::TableNextColumn();
            ImGui::Text("%u", m.frustumCulled);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long) cull.cascadeInstanceTriangles[c]);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long) cull.cascadeTriangles[c]);
        }
        ImGui::EndTable();
    }
    helpMarker("Instances: the submesh instances the shadow pass draws, culled against the cascade's frustum. Meshlets: those\n"
               "the shadow pass's task shaders drew and culled by the cascade's frustum (no cone: both faces cast; no occlusion).\n"
               "Kept tris: the drawn instances' triangles; drawn tris: what meshlet culling kept of them.");

    ImGui::End();
}
