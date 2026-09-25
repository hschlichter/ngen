#include "gpuscenewindow.h"

#include "drawlists.h"
#include "gpuscene.h"
#include "gpuschema.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <imgui.h>

namespace {

template <typename T>
auto readAt(const CaptureResult& r, size_t offset) -> T {
    T value{};
    if (r.bytes && offset + sizeof(T) <= r.bytes->size()) {
        std::memcpy(&value, r.bytes->data() + offset, sizeof(T));
    }
    return value;
}

auto find(const std::map<std::string, CaptureResult>& m, const char* resource) -> const CaptureResult* {
    auto it = m.find(resource);
    return it != m.end() && it->second.bytes && it->second.error.empty() ? &it->second : nullptr;
}

auto viewName(uint32_t v) -> std::string {
    return v == 0 ? std::string("camera") : std::format("cascade {}", v - 1);
}

auto flagText(uint32_t flags) -> std::string {
    std::string text;
    if ((flags & gpuInstancePrimFirst) != 0) {
        text += "primFirst ";
    }
    if ((flags & gpuInstanceDoubleSided) != 0) {
        text += "doubleSided ";
    }
    if ((flags & gpuInstanceBoundsValid) == 0) {
        text += "noBounds";
    }
    return text;
}

} // namespace

auto gpuSceneCaptures() -> const std::vector<std::pair<std::string, std::string>>& {
    // (pass, resource): each table where it is complete for the frame.
    static const std::vector<std::pair<std::string, std::string>> captures = {
        {"InstanceCullScatter", "instances"},
        {"", "gpuscene.meshtable"},
        {"", "gpuscene.materials"},
        {"InstanceCull", "cullVisibility"},
        {"InstanceCull", "cullPlanes"},
        {"InstanceCull", "cullGroupCounters"},
        {"InstanceCullScan", "cullGroupOffsets"},
        {"InstanceCullScan", "drawCounts"},
        {"InstanceCullScatter", "drawCommands"},
    };
    return captures;
}

auto primOfEachInstance(const RenderWorld& renderWorld) -> std::vector<uint32_t> {
    std::vector<uint32_t> prims(renderWorld.meshInstances.size(), 0);
    for (const auto& [prim, range] : renderWorld.primToInstance) {
        for (uint32_t i = range.first; i < range.first + range.count && i < prims.size(); i++) {
            prims[i] = prim;
        }
    }
    return prims;
}

auto gpuSceneResourceOfWatch(uint32_t id) -> const std::string* {
    const auto& captures = gpuSceneCaptures();
    if (id < gpuSceneWatchBase || id >= gpuSceneWatchBase + captures.size()) {
        return nullptr;
    }
    return &captures[id - gpuSceneWatchBase].second;
}

auto gpuSceneWatches(const GpuSceneWindowState& state) -> std::vector<CaptureWatch> {
    std::vector<CaptureWatch> watches;
    const auto& captures = gpuSceneCaptures();
    for (uint32_t i = 0; i < captures.size(); i++) {
        watches.push_back({.id = gpuSceneWatchBase + i, .pass = captures[i].first, .resource = captures[i].second, .live = state.live, .trigger = state.trigger});
    }
    return watches;
}

auto buildGpuSceneRows(const std::map<std::string, CaptureResult>& byResource, std::span<const uint32_t> primOfInstance, const std::function<std::string(uint32_t)>& primPath) -> std::vector<GpuSceneInstanceRow> {
    std::vector<GpuSceneInstanceRow> rows;
    const auto* instances = find(byResource, "instances");
    if (instances == nullptr) {
        return rows;
    }
    const auto* meshes = find(byResource, "gpuscene.meshtable");
    const auto* materials = find(byResource, "gpuscene.materials");
    const auto* visibility = find(byResource, "cullVisibility");
    const auto* planes = find(byResource, "cullPlanes");
    // The instance buffer is sized to its capacity; the visibility buffer's live part is the
    // instance count the culling saw, so rows stop where both agree.
    auto count = instances->bytes->size() / sizeof(GpuInstanceRecord);
    if (visibility != nullptr) {
        count = std::min(count, visibility->bytes->size() / sizeof(uint32_t));
    }
    count = std::min(count, primOfInstance.size());
    rows.reserve(count);
    for (uint32_t m = 0; m < count; m++) {
        auto record = readAt<GpuInstanceRecord>(*instances, (size_t) m * sizeof(GpuInstanceRecord));
        GpuSceneInstanceRow row = {
            .index = m,
            .prim = primOfInstance[m],
            .mesh = record.mesh,
            .material = record.material,
            .indexOffset = record.indexOffset,
            .indexCount = record.indexCount,
            .flags = record.flags,
            .boundsMin = record.boundsMin,
            .boundsMax = record.boundsMax,
        };
        row.primPath = primPath(row.prim);
        if (meshes != nullptr) {
            row.meshIndexCount = readAt<GpuMeshEntry>(*meshes, (size_t) record.mesh * sizeof(GpuMeshEntry)).indexCount;
        }
        if (materials != nullptr) {
            row.textureSlot = readAt<GpuMaterial>(*materials, (size_t) record.material * sizeof(GpuMaterial)).baseColorTexture;
        }
        if (visibility != nullptr) {
            row.visibleBits = readAt<uint32_t>(*visibility, (size_t) m * 4);
        }
        if (planes != nullptr) {
            row.cullPlanes = readAt<uint32_t>(*planes, (size_t) m * 4);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

auto writeGpuSceneJoinedJson(const std::string& path, const std::vector<GpuSceneInstanceRow>& rows, uint32_t viewCount) -> bool {
    auto* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "{\"views\": %u, \"instances\": [\n", viewCount);
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        std::fprintf(f, "  {\"instance\": %u, \"prim\": \"%s\", \"mesh\": %u, \"meshInPool\": %s, \"material\": %u, \"textureSlot\": %u, \"indexOffset\": %u, \"indexCount\": %u, \"flags\": \"%s\", \"boundsMin\": [%g, %g, %g], \"boundsMax\": [%g, %g, %g], \"views\": [", r.index, r.primPath.c_str(), r.mesh, r.meshIndexCount > 0 ? "true" : "false", r.material, r.textureSlot, r.indexOffset, r.indexCount, flagText(r.flags).c_str(), r.boundsMin.x, r.boundsMin.y, r.boundsMin.z, r.boundsMax.x, r.boundsMax.y, r.boundsMax.z);
        for (uint32_t v = 0; v < viewCount; v++) {
            std::fprintf(f, "%s{\"view\": \"%s\", \"visible\": %s, \"reason\": \"%s\"}", v > 0 ? ", " : "", viewName(v).c_str(), (r.visibleBits & (1u << v)) != 0 ? "true" : "false", cullPlaneName((r.cullPlanes >> (4 * v)) & 0xFu));
        }
        std::fprintf(f, "]}%s\n", i + 1 < rows.size() ? "," : "");
    }
    std::fprintf(f, "]}\n");
    std::fclose(f);
    return true;
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
                    ImGui::TableSetupColumn(viewName(v).c_str(), ImGuiTableColumnFlags_WidthFixed, 80.0f);
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
                    ImGui::Text("%u +%u", r.indexOffset, r.indexCount);
                    ImGui::TableSetColumnIndex(6);
                    ImGui::TextUnformatted(flagText(r.flags).c_str());
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
            if (const auto* meshes = find(state.results, "gpuscene.meshtable"); meshes != nullptr) {
                const auto& schema = schemaForResource("gpuscene.meshtable");
                auto count = meshes->bytes->size() / schema.stride;
                if (ImGui::BeginTable("##meshes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY)) {
                    ImGui::TableSetupColumn("Mesh");
                    ImGui::TableSetupColumn("firstIndex");
                    ImGui::TableSetupColumn("vertexOffset");
                    ImGui::TableSetupColumn("indexCount");
                    ImGui::TableSetupColumn("Triangles");
                    ImGui::TableHeadersRow();
                    for (size_t i = 0; i < count; i++) {
                        auto e = readAt<GpuMeshEntry>(*meshes, i * sizeof(GpuMeshEntry));
                        if (e.indexCount == 0) {
                            continue;
                        }
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%zu", i);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%u", e.firstIndex);
                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("%d", e.vertexOffset);
                        ImGui::TableSetColumnIndex(3);
                        ImGui::Text("%u", e.indexCount);
                        ImGui::TableSetColumnIndex(4);
                        ImGui::Text("%u", e.indexCount / 3);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Materials")) {
            if (const auto* materials = find(state.results, "gpuscene.materials"); materials != nullptr) {
                auto count = materials->bytes->size() / sizeof(GpuMaterial);
                ImGui::TextWrapped("Entry 0 is 'no material'. Texture slot 0 is the fallback texture; slots index the geometry set's texture array.");
                for (size_t i = 0; i < count; i++) {
                    auto e = readAt<GpuMaterial>(*materials, i * sizeof(GpuMaterial));
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
            ImGui::Text("%u of %zu instances visible to the %s.", visible, rows.size(), viewName(v).c_str());
            for (uint32_t code = 0; code < 6; code++) {
                ImGui::BulletText("culled by the %s plane: %u", cullPlaneName(code), byReason[code]);
            }
            ImGui::BulletText("not tested (culling off or no bounds): %u", byReason[0xF]);
            ImGui::SeparatorText("Culled instances");
            ImGui::BeginChild("##culled");
            for (const auto& r : rows) {
                auto code = (r.cullPlanes >> (4 * v)) & 0xFu;
                if (code < 6 && ImGui::Selectable(std::format("{:4}  {}  ({})", r.index, r.primPath, cullPlaneName(code)).c_str())) {
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
            ImGui::TextDisabled("view %u, %s", region / DrawLists::bucketCount, region % DrawLists::bucketCount == 0 ? "single-sided" : "double-sided");
            const auto* counters = find(state.results, "cullGroupCounters");
            const auto* offsets = find(state.results, "cullGroupOffsets");
            const auto* counts = find(state.results, "drawCounts");
            const auto* commands = find(state.results, "drawCommands");
            if (counters != nullptr && offsets != nullptr) {
                auto groups = std::min(counters->bytes->size() / (DrawLists::counterCount * 4), offsets->bytes->size() / (DrawLists::regionCount * 4));
                auto used = (rows.size() + DrawLists::groupSize - 1) / DrawLists::groupSize;
                for (size_t g = 0; g < std::min(groups, std::max<size_t>(used, 1)); g++) {
                    auto count = readAt<uint32_t>(*counters, (g * DrawLists::counterCount + DrawLists::counterDraws + region) * 4);
                    auto offset = readAt<uint32_t>(*offsets, (g * DrawLists::regionCount + region) * 4);
                    ImGui::BulletText("workgroup %zu (instances %zu..%zu): %u commands, written from offset %u", g, g * DrawLists::groupSize, (g + 1) * DrawLists::groupSize - 1, count, offset);
                }
            }
            if (counts != nullptr && commands != nullptr) {
                auto total = readAt<uint32_t>(*counts, DrawLists::countOffset(region));
                auto capacity = commands->bytes->size() / sizeof(RhiDrawIndexedIndirectCommand) / DrawLists::regionCount;
                ImGui::SeparatorText(std::format("Final order: {} commands", total).c_str());
                ImGui::BeginChild("##order");
                for (uint32_t i = 0; i < total && i < capacity; i++) {
                    auto command = readAt<RhiDrawIndexedIndirectCommand>(*commands, (region * capacity + i) * sizeof(RhiDrawIndexedIndirectCommand));
                    auto path = command.firstInstance < primOfInstance.size() ? primPath(primOfInstance[command.firstInstance]) : std::string("(instance out of range)");
                    ImGui::Text("%4u  instance %4u  %s  (%u triangles)", i, command.firstInstance, path.c_str(), command.indexCount / 3);
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
