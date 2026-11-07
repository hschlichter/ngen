#include "gpuscenewindow.h"

#include "drawlists.h"
#include "gpuscene.h"
#include "gpuscenecapture.h"
#include "gpuschema.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <format>
#include <imgui.h>

auto gpuSceneResourceOfWatch(uint32_t id) -> const std::string* {
    const auto& captures = gpuSceneCaptures();
    if (id < gpuSceneWatchBase || id >= gpuSceneWatchBase + captures.size()) {
        return nullptr;
    }
    return &captures[id - gpuSceneWatchBase].resource;
}

auto gpuSceneWatches(const GpuSceneWindowState& state) -> std::vector<CaptureWatch> {
    std::vector<CaptureWatch> watches;
    const auto& captures = gpuSceneCaptures();
    for (uint32_t i = 0; i < captures.size(); i++) {
        auto watch = captures[i];
        watch.id = gpuSceneWatchBase + i;
        watch.live = state.live;
        watch.trigger = state.trigger;
        watches.push_back(std::move(watch));
    }
    return watches;
}

auto drawGpuSceneWindow(bool& show,
                        GpuSceneWindowState& state,
                        uint32_t viewCount,
                        std::span<const uint32_t> primOfInstance,
                        const std::function<std::string(uint32_t)>& primPath,
                        const std::function<void(uint32_t)>& selectPrim) -> void {
    ImGui::SetNextWindowSize(ImVec2(1000, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("GPU Scene", &show)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Refresh")) {
        state.trigger++;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Live", &state.live);
    ImGui::SameLine();
    const auto* any = state.results.empty() ? nullptr : &state.results.begin()->second;
    if (any != nullptr) {
        ImGui::TextDisabled("captured on frame %llu", (unsigned long long) any->frame);
    }
    auto rows = buildGpuSceneRows(state.results, primOfInstance, primPath);
    if (ImGui::BeginTabBar("##gpuscene")) {
        if (ImGui::BeginTabItem(std::format("Instances ({})###instances", rows.size()).c_str())) {
            ImGui::SetNextItemWidth(240.0f);
            ImGui::InputText("Filter prim", state.filter, sizeof(state.filter));
            auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("##instances", 8 + (int) viewCount, flags)) {
                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("Prim", ImGuiTableColumnFlags_WidthFixed, 260.0f);
                ImGui::TableSetupColumn("Mesh", ImGuiTableColumnFlags_WidthFixed, 45.0f);
                ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Tex slot", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Indices", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Bounds", ImGuiTableColumnFlags_WidthFixed, 280.0f);
                for (uint32_t v = 0; v < viewCount; v++) {
                    ImGui::TableSetupColumn(gpuSceneViewName(v).c_str(), ImGuiTableColumnFlags_WidthFixed, 80.0f);
                }
                ImGui::TableSetupScrollFreeze(2, 1);
                ImGui::TableHeadersRow();
                for (const auto& r : rows) {
                    if (state.filter[0] != '\0' && r.primPath.find(state.filter) == std::string::npos) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%u", r.index);
                    ImGui::TableSetColumnIndex(1);
                    if (ImGui::Selectable(std::format("{}##{}", r.primPath, r.index).c_str(), false, ImGuiSelectableFlags_None)) {
                        selectPrim(r.prim);
                    }
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", r.mesh);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%u", r.material);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%u", r.textureSlot);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%u +%u", r.meshletOffset, r.meshletCount);
                    ImGui::TableSetColumnIndex(6);
                    ImGui::TextUnformatted(gpuInstanceFlagText(r.flags).c_str());
                    ImGui::TableSetColumnIndex(7);
                    ImGui::Text("(%.2f %.2f %.2f)-(%.2f %.2f %.2f)", r.boundsMin.x, r.boundsMin.y, r.boundsMin.z, r.boundsMax.x, r.boundsMax.y, r.boundsMax.z);
                    for (uint32_t v = 0; v < viewCount; v++) {
                        ImGui::TableSetColumnIndex(8 + (int) v);
                        const char* reason = cullPlaneName((r.cullPlanes >> (4 * v)) & 0xFu);
                        if ((r.visibleBits & (1u << v)) != 0) {
                            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", reason);
                        } else {
                            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", reason);
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Meshes")) {
            if (const auto* meshes = capturedTable(state.results, "gpuscene.meshtable"); meshes != nullptr) {
                const auto& schema = schemaForResource("gpuscene.meshtable");
                auto count = meshes->bytes->size() / schema.stride;
                if (ImGui::BeginTable("##meshes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
                    ImGui::TableSetupColumn("Mesh");
                    ImGui::TableSetupColumn("vertexOffset");
                    ImGui::TableSetupColumn("meshletOffset");
                    ImGui::TableSetupColumn("meshletCount");
                    ImGui::TableSetupColumn("Triangles");
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < count; i++) {
                        auto e = capturedValue<GpuMeshEntry>(*meshes, i * sizeof(GpuMeshEntry));
                        if (e.meshletCount == 0) {
                            continue;
                        }
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%zu", i);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%d", e.vertexOffset);
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%u", e.meshletOffset);
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%u", e.meshletCount);
                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%u", e.triangleCount);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Materials")) {
            if (const auto* materials = capturedTable(state.results, "gpuscene.materials"); materials != nullptr) {
                auto count = materials->bytes->size() / sizeof(GpuMaterial);
                ImGui::TextWrapped("Entry 0 is 'no material'. Texture slot 0 is the fallback texture; slots index the geometry set's texture array.");
                for (size_t i = 0; i < count; i++) {
                    auto e = capturedValue<GpuMaterial>(*materials, i * sizeof(GpuMaterial));
                    ImGui::BulletText("entry %zu: texture slot %u", i, e.baseColorTexture);
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Culling")) {
            std::vector<const char*> views = {"camera", "cascade 0", "cascade 1", "cascade 2", "cascade 3"};
            ImGui::SetNextItemWidth(140.0f);
            ImGui::Combo("View", &state.cullView, views.data(), (int) viewCount);
            auto v = (uint32_t) state.cullView;
            uint32_t visible = 0;
            std::array<uint32_t, 16> byReason = {};
            for (const auto& r : rows) {
                if ((r.visibleBits & (1u << v)) != 0) {
                    visible++;
                }
                byReason[(r.cullPlanes >> (4 * v)) & 0xFu]++;
            }
            ImGui::Text("%u of %zu instances visible to the %s.", visible, rows.size(), gpuSceneViewName(v).c_str());
            for (uint32_t code = 0; code < 6; code++) {
                ImGui::BulletText("culled by the %s plane: %u", cullPlaneName(code), byReason[code]);
            }
            ImGui::BulletText("hidden by the depth pyramid (occlusion, camera): %u", byReason[0xC]);
            ImGui::BulletText("not tested (culling off or no bounds): %u", byReason[0xF]);
            ImGui::BulletText("a LOD level its group doesn't draw: %u", byReason[0xD]);
            ImGui::SeparatorText("Culled instances");
            ImGui::BeginChild("##culled");
            for (const auto& r : rows) {
                auto code = (r.cullPlanes >> (4 * v)) & 0xFu;
                // A plane, or occluded (0xC): every reason an instance isn't drawn in the view.
                if ((code < 6 || code == 0xC) && ImGui::Selectable(std::format("{:4}  {}  ({})", r.index, r.primPath, cullPlaneName(code)).c_str())) {
                    selectPrim(r.prim);
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compaction")) {
            ImGui::TextWrapped("Reduce-then-scan: each workgroup of %u instances counts its commands per region (InstanceCull); one scan turns the counts into "
                               "start offsets (InstanceCullScan); each command is written at its workgroup's offset plus its place in the workgroup "
                               "(InstanceCullScatter), so commands stay in instance order.",
                               DrawLists::groupSize);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderInt("Region", &state.compactionRegion, 0, (int) DrawLists::regionCount - 1);
            auto region = (uint32_t) state.compactionRegion;
            ImGui::SameLine();
            ImGui::TextDisabled("%s", DrawLists::regionName(region));
            const auto* counters = capturedTable(state.results, "cullGroupCounters");
            const auto* offsets = capturedTable(state.results, "cullGroupOffsets");
            const auto* counts = capturedTable(state.results, "drawCounts");
            const auto* commands = capturedTable(state.results, "drawCommands");
            const auto* params = capturedTable(state.results, "cullParams");
            // Per workgroup: each region's draw record offset, then its work item offset. The first phase's: the second overwrites them.
            if (region / DrawLists::bucketCount == DrawLists::lateView) {
                ImGui::TextDisabled("Per workgroup: the counters and offsets captured are the first phase's; the second phase's aren't kept.");
            } else if (counters != nullptr && offsets != nullptr) {
                auto groups = std::min(counters->bytes->size() / (DrawLists::counterCount * 4), offsets->bytes->size() / (DrawLists::regionCount * 2 * 4));
                auto used = (rows.size() + DrawLists::groupSize - 1) / DrawLists::groupSize;
                for (size_t g = 0; g < std::min(groups, std::max<size_t>(used, 1)); g++) {
                    auto count = capturedValue<uint32_t>(*counters, (g * DrawLists::counterCount + DrawLists::counterDraws + region) * 4);
                    auto offset = capturedValue<uint32_t>(*offsets, (g * DrawLists::regionCount * 2 + region) * 4);
                    ImGui::BulletText("workgroup %zu (instances %zu..%zu): %u commands, written from offset %u", g, g * DrawLists::groupSize, (g + 1) * DrawLists::groupSize - 1, count, offset);
                }
            }
            if (counts != nullptr && commands != nullptr && params != nullptr) {
                // The region's records as the frame packed them: from its offset to the next region's (the last ends at commandsUsed).
                using P = DrawLists::CullParams;
                auto total = capturedValue<uint32_t>(*counts, DrawLists::countOffset(region));
                auto first = capturedValue<uint32_t>(*params, offsetof(P, regionOffsets) + (region * 4));
                auto end = region + 1 < DrawLists::regionCount ? capturedValue<uint32_t>(*params, offsetof(P, regionOffsets) + ((region + 1) * 4)) : capturedValue<uint32_t>(*params, offsetof(P, commandsUsed));
                auto inBuffer = (uint32_t) (commands->bytes->size() / sizeof(DrawRecord));
                auto capacity = std::min(end, inBuffer) > first ? std::min(end, inBuffer) - first : 0u;
                ImGui::SeparatorText(std::format("Final order: {} commands, from record {}, room for {}", total, first, capacity).c_str());
                ImGui::BeginChild("##order");
                for (uint32_t i = 0; i < total && i < capacity; i++) {
                    auto command = capturedValue<DrawRecord>(*commands, (first + i) * sizeof(DrawRecord));
                    auto path = command.instance < primOfInstance.size() ? primPath(primOfInstance[command.instance]) : std::string("(instance out of range)");
                    ImGui::Text("%4u  instance %4u  %s  (%u meshlets from work item %u)", i, command.instance, path.c_str(), command.meshletCount, command.firstItem);
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
