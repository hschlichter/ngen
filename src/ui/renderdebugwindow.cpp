#include "renderdebugwindow.h"

#include "framegraphdebug.h"
#include "lightingpass.h"
#include "usdscene.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

auto formatBytes(uint64_t bytes, char* out, size_t size) -> const char* {
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(out, size, "%.2f GiB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(out, size, "%.2f MiB", (double) bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(out, size, "%.1f KiB", (double) bytes / 1024.0);
    } else {
        std::snprintf(out, size, "%llu B", (unsigned long long) bytes);
    }
    return out;
}

auto drawTextureInspector(const RenderDebugSnapshot& s, const USDScene& scene, RenderDebugDrawState& drawState) -> void {
    auto& req = drawState.inspect;
    if (!req.enabled) {
        return;
    }
    ImGui::Separator();
    ImGui::Text("Texture inspector: material %u", req.material);
    ImGui::SameLine();
    if (ImGui::SmallButton("Close")) {
        req.enabled = false;
        drawState.inspectChanged = true;
        return;
    }
    const auto& insp = s.inspect;
    if (!insp.valid || insp.material != req.material) {
        ImGui::TextDisabled("(waiting for the render thread)");
        return;
    }
    int level = (int) req.level;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("Level", &level, 0, (int) insp.mipLevels - 1)) {
        req.level = (uint32_t) level;
        drawState.inspectChanged = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Dump level")) {
        drawState.dumpRequested = true;
    }
    char buf[32];
    ImGui::Text("%ux%u, %s, %u levels", insp.levelWidth, insp.levelHeight, formatBytes(insp.levelBytes, buf, sizeof(buf)), insp.mipLevels);
    if (insp.previewTextureId != 0) {
        ImGui::Image((ImTextureID) insp.previewTextureId, ImVec2((float) insp.previewWidth, (float) insp.previewHeight));
    }
    // Who binds it: unique prims from the draw log.
    ImGui::TextUnformatted("Bound by");
    std::vector<uint32_t> prims;
    for (const auto& d : s.draws) {
        if (d.material == req.material && d.prim != 0 && std::find(prims.begin(), prims.end(), d.prim) == prims.end()) {
            prims.push_back(d.prim);
        }
    }
    if (prims.empty()) {
        ImGui::TextDisabled("(no draws logged this frame)");
    }
    for (auto prim : prims) {
        const auto* rec = scene.isOpen() ? scene.getPrimRecord(PrimHandle{prim}) : nullptr;
        ImGui::BulletText("%s", rec != nullptr ? rec->path.c_str() : "(unknown prim)");
    }
}

auto drawSceneTab(const RenderDebugSnapshot& s, const USDScene& scene, RenderDebugDrawState& drawState) -> void {
    uint64_t triangles = 0;
    uint64_t vertices = 0;
    uint64_t meshBytes = 0;
    for (const auto& m : s.meshes) {
        triangles += m.indexCount / 3;
        vertices += m.vertexCount;
        meshBytes += m.vertexBytes + m.indexBytes;
    }
    uint64_t textureBytes = 0;
    for (const auto& t : s.textures) {
        textureBytes += t.bytes;
    }
    char buf[32];
    char buf2[32];
    ImGui::Text("%u instances (%u culled, %u drawn by the shadow pass), %zu meshes, %llu triangles, %llu vertices, %s", s.instanceCount, s.culledInstances, s.primFirstInstances, s.meshes.size(), (unsigned long long) triangles, (unsigned long long) vertices, formatBytes(meshBytes, buf, sizeof(buf)));
    ImGui::Text("%u materials, %u with a texture, %zu textures, %s", s.materialCount, s.materialsWithTexture, s.textures.size(), formatBytes(textureBytes, buf2, sizeof(buf2)));
    if (s.hasSun) {
        ImGui::Text("%u lights; sun direction (%.2f %.2f %.2f) radiance (%.2f %.2f %.2f) shadow colour (%.2f %.2f %.2f); shadow map %ux%u", s.lightCount, s.sunDirection.x, s.sunDirection.y, s.sunDirection.z, s.sunRadiance.x, s.sunRadiance.y, s.sunRadiance.z, s.sunShadowColor.x, s.sunShadowColor.y, s.sunShadowColor.z, s.shadowMapExtent.width, s.shadowMapExtent.height);
    } else {
        ImGui::Text("%u lights; no directional light, sun defaults to world up", s.lightCount);
    }
    ImGui::Separator();

    // Meshes, sortable.
    ImGui::TextUnformatted("Meshes");
    if (ImGui::BeginTable("##meshes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 220.0f))) {
        ImGui::TableSetupColumn("Mesh", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Vertices");
        ImGui::TableSetupColumn("Vertex bytes");
        ImGui::TableSetupColumn("Index bytes");
        ImGui::TableSetupColumn("Instances");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        std::vector<const RenderDebugMesh*> rows;
        rows.reserve(s.meshes.size());
        for (const auto& m : s.meshes) {
            rows.push_back(&m);
        }
        if (auto* sort = ImGui::TableGetSortSpecs(); sort != nullptr && sort->SpecsCount > 0) {
            auto spec = sort->Specs[0];
            auto asc = spec.SortDirection == ImGuiSortDirection_Ascending;
            std::sort(rows.begin(), rows.end(), [&](const RenderDebugMesh* a, const RenderDebugMesh* b) {
                auto key = [&](const RenderDebugMesh* m) -> uint64_t {
                    switch (spec.ColumnIndex) {
                        case 0:
                            return m->meshIndex;
                        case 1:
                            return m->indexCount / 3;
                        case 2:
                            return m->vertexCount;
                        case 3:
                            return m->vertexBytes;
                        case 4:
                            return m->indexBytes;
                        default:
                            return m->instances;
                    }
                };
                return asc ? key(a) < key(b) : key(a) > key(b);
            });
        }
        for (const auto* m : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%u", m->meshIndex);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", m->indexCount / 3);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", m->vertexCount);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(formatBytes(m->vertexBytes, buf, sizeof(buf)));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(formatBytes(m->indexBytes, buf, sizeof(buf)));
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%u", m->instances);
        }
        ImGui::EndTable();
    }

    ImGui::TextUnformatted("Textures");
    if (ImGui::BeginTable("##textures", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 180.0f))) {
        ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Format");
        ImGui::TableSetupColumn("Mips");
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const auto& t : s.textures) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID((int) t.materialIndex);
            char label[32];
            std::snprintf(label, sizeof(label), "%u", t.materialIndex);
            bool selected = drawState.inspect.enabled && drawState.inspect.material == t.materialIndex;
            if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                drawState.inspect = {.enabled = true, .material = t.materialIndex, .level = 0};
                drawState.inspectChanged = true;
            }
            ImGui::PopID();
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%ux%u", t.width, t.height);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(toString(t.format));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", t.mipLevels);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(formatBytes(t.bytes, buf, sizeof(buf)));
        }
        ImGui::EndTable();
    }
    drawTextureInspector(s, scene, drawState);
}

auto drawDrawList(const RenderDebugSnapshot& s, const USDScene& scene, PrimHandle& selectedPrim, RenderDebugDrawState& drawState) -> void {
    ImGui::Separator();
    ImGui::Text("Draws (%zu logged)", s.draws.size());

    // Timing window controls. The render thread wraps the chosen draws in GPU zones.
    auto& timing = drawState.timing;
    if (ImGui::Checkbox("Time draws", &timing.enabled)) {
        drawState.timingChanged = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##timing_pass", timing.pass.empty() ? "(pass)" : timing.pass.c_str())) {
        for (const auto& p : s.passes) {
            if (p.stats.draws == 0) {
                continue;
            }
            if (ImGui::Selectable(p.name.c_str(), p.name == timing.pass)) {
                timing.pass = p.name;
                timing.first = 0;
                drawState.timingChanged = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    int first = (int) timing.first;
    if (ImGui::InputInt("first", &first, (int) timing.count, (int) timing.count)) {
        timing.first = (uint32_t) std::max(0, first);
        drawState.timingChanged = true;
    }
    ImGui::SameLine();
    ImGui::Text("%u per page", timing.count);

    // One collapsible table per pass with draws.
    const char* currentPass = nullptr;
    bool open = false;
    for (size_t i = 0; i < s.draws.size(); i++) {
        const auto& d = s.draws[i];
        if (currentPass == nullptr || std::strcmp(currentPass, d.pass) != 0) {
            if (open) {
                ImGui::EndTable();
                ImGui::TreePop();
            }
            currentPass = d.pass;
            uint32_t passDraws = 0;
            for (size_t j = i; j < s.draws.size() && std::strcmp(s.draws[j].pass, d.pass) == 0; j++) {
                passDraws++;
            }
            char label[96];
            std::snprintf(label, sizeof(label), "%s: %u draws###%s", d.pass, passDraws, d.pass);
            open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth);
            if (open) {
                open = ImGui::BeginTable("##draws", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 240.0f));
                if (open) {
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupColumn("Prim");
                    ImGui::TableSetupColumn("Mesh", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Index range", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                    ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, 64.0f);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();
                } else {
                    ImGui::TreePop();
                }
            }
        }
        if (!open) {
            continue;
        }
        ImGui::TableNextRow();
        ImGui::PushID((int) i);
        ImGui::TableSetColumnIndex(0);
        ImGui::Text("%u", d.drawIndex);
        ImGui::TableSetColumnIndex(1);
        const auto* rec = scene.isOpen() ? scene.getPrimRecord(PrimHandle{d.prim}) : nullptr;
        bool selected = selectedPrim.index == d.prim && d.prim != 0;
        if (ImGui::Selectable(rec != nullptr ? rec->path.c_str() : "(unknown prim)", selected, ImGuiSelectableFlags_SpanAllColumns)) {
            selectedPrim = PrimHandle{d.prim};
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%u", d.mesh);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%u", d.material);
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%u", d.indexCount / 3);
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%u .. %u", d.indexOffset, d.indexOffset + d.indexCount);
        ImGui::TableSetColumnIndex(6);
        if (d.gpuMs >= 0.0) {
            ImGui::Text("%.3f", d.gpuMs);
        } else if (d.timed) {
            ImGui::TextDisabled("...");
        } else {
            ImGui::TextDisabled("-");
        }
        ImGui::PopID();
    }
    if (open) {
        ImGui::EndTable();
        ImGui::TreePop();
    }
}

auto drawFrameTab(const RenderDebugSnapshot& s) -> void {
    const auto& t = s.frameTotals;
    ImGui::Text("Frame #%llu: %u draws, %u dispatches, %u barriers, %u pipeline binds, %u descriptor binds, %u copies, %llu primitives", (unsigned long long) s.frameIndex, t.draws, t.dispatches, t.barriers, t.pipelineBinds, t.descriptorBinds, t.copies, (unsigned long long) t.primitives);
    ImGui::Text("Swapchain %ux%u %s, %u images, slot %u", s.swapchainExtent.width, s.swapchainExtent.height, toString(s.swapchainFormat), s.swapchainImages, s.currentSlot);
    ImGui::Separator();

    if (ImGui::BeginTable("##passes", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Dispatches", ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn("Primitives", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Barriers", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Binds", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Copies", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableHeadersRow();
        for (const auto& p : s.passes) {
            ImGui::TableNextRow();
            if (p.culled) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableSetColumnIndex(1);
            if (p.gpuTimeMs >= 0.0) {
                ImGui::Text("%.3f", p.gpuTimeMs);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u", p.stats.draws);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", p.stats.dispatches);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", (unsigned long long) p.stats.primitives);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%u", p.stats.barriers);
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%u", p.stats.pipelineBinds + p.stats.descriptorBinds);
            ImGui::TableSetColumnIndex(7);
            ImGui::Text("%u", p.stats.copies);
            if (p.culled) {
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::Text("Transient pool: %zu textures, %u allocations since start", s.poolTextures.size(), s.poolAllocationsTotal);
    if (ImGui::BeginTable("##pool", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Format");
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (const auto& t : s.poolTextures) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%ux%u", t.width, t.height);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(toString(t.format));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(t.inUse ? "in use" : "available");
        }
        ImGui::EndTable();
    }
}

auto drawViewTab(RenderDebugViewFlags& view) -> void {
    ImGui::TextUnformatted("Buffer view");
    static const char* modes[] = {"Lit", "Albedo", "Normals", "Depth", "Shadow factor", "Shadow map", "Shadow UV", "World position", "Mip level"};
    for (int i = 0; i < (int) (sizeof(modes) / sizeof(modes[0])); i++) {
        if (i % 4 != 0) {
            ImGui::SameLine();
        }
        ImGui::RadioButton(modes[i], &view.gbufferView, i);
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Overlays");
    ImGui::Checkbox("Buffer overlay", &view.showBufferOverlay);
    ImGui::SameLine();
    ImGui::Checkbox("Shadow overlay", &view.showShadowOverlay);
    ImGui::SameLine();
    ImGui::Checkbox("Anti-aliasing (FXAA)", &view.antiAliasing);
    ImGui::Checkbox("Grid", &view.showGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Origin", &view.showOrigin);
    ImGui::SameLine();
    ImGui::Checkbox("Gizmos", &view.showGizmo);
    ImGui::SameLine();
    ImGui::Checkbox("AABBs", &view.showAABBs);
    ImGui::SameLine();
    ImGui::Checkbox("Light gizmos", &view.showLightGizmos);

    ImGui::Separator();
    ImGui::TextUnformatted("Texture sampling");
    static const float anisoSteps[] = {0.0f, 2.0f, 4.0f, 8.0f, 16.0f};
    int anisoIndex = 0;
    for (int i = 0; i < 5; i++) {
        if (view.sampler.maxAnisotropy >= anisoSteps[i]) {
            anisoIndex = i;
        }
    }
    static const char* anisoNames[] = {"off", "2x", "4x", "8x", "16x"};
    if (ImGui::Combo("Anisotropy", &anisoIndex, anisoNames, 5)) {
        view.sampler.maxAnisotropy = anisoSteps[anisoIndex];
    }
    ImGui::SliderFloat("LOD bias", &view.sampler.lodBias, -2.0f, 4.0f, "%.2f");
    ImGui::SliderFloat("Min LOD", &view.sampler.minLod, 0.0f, 12.0f, "%.0f");
    ImGui::Checkbox("Nearest mip", &view.sampler.nearestMip);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Material sampler. Min LOD forces coarser levels; the Mip level view shows what the sampler picks.");
    }
}

auto drawDeviceTab(const RenderDebugSnapshot& s) -> void {
    const auto& l = s.limits;
    ImGui::Text("%s", l.deviceName);
    ImGui::Text("Driver: %s", l.driverName);
    ImGui::Text("Validation: %s", s.validation ? "on" : "off");
    ImGui::Separator();
    ImGui::Text("Uniform buffer offset alignment: %llu", (unsigned long long) l.minUniformBufferOffsetAlignment);
    ImGui::Text("Max push constant size: %u", l.maxPushConstantSize);
    ImGui::Text("Wide lines: %s (max %.1f)", l.wideLines ? "yes" : "no", l.maxLineWidth);
    ImGui::Text("Sampler anisotropy: %s", l.samplerAnisotropy ? "yes" : "no");
    ImGui::Text("Timestamps: %s, %.3f ns per tick, calibrated: %s", l.timestamps ? "yes" : "no", l.timestampPeriodNs, l.calibratedTimestamps ? "yes" : "no");
}

} // namespace

auto drawRenderDebugWindow(bool& show, const std::optional<RenderDebugSnapshot>& snap, RenderDebugViewFlags& view, const USDScene& scene, PrimHandle& selectedPrim, RenderDebugDrawState& drawState) -> bool {
    bool screenshot = false;
    if (!show) {
        return false;
    }
    ImGui::SetNextWindowSize(ImVec2(820, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render Debug", &show)) {
        ImGui::End();
        return false;
    }
    if (ImGui::BeginTabBar("##rd_tabs")) {
        if (ImGui::BeginTabItem("Scene")) {
            if (snap.has_value()) {
                drawSceneTab(*snap, scene, drawState);
            } else {
                ImGui::TextDisabled("Waiting for render thread snapshot...");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Frame")) {
            if (snap.has_value()) {
                drawFrameTab(*snap);
                drawDrawList(*snap, scene, selectedPrim, drawState);
            } else {
                ImGui::TextDisabled("Waiting for render thread snapshot...");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("View")) {
            drawViewTab(view);
            ImGui::Separator();
            if (ImGui::Button("Screenshot (F12)")) {
                screenshot = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("writes screenshot_<frame>.png in the working directory");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Device")) {
            if (snap.has_value()) {
                drawDeviceTab(*snap);
            } else {
                ImGui::TextDisabled("Waiting for render thread snapshot...");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    return screenshot;
}
