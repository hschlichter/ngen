#include "memorywindow.h"

#include "framegraphdebug.h"
#include "uiformat.h"

#include <algorithm>
#include <imgui.h>
#include <map>
#include <vector>

auto allocationCategory(const std::string& name) -> std::string {
    if (name.empty()) {
        return "(unnamed)";
    }
    auto end = name.find_first_of(".:");
    return name.substr(0, end);
}

namespace {

auto memoryUsageName(RhiMemoryUsage memory) -> const char* {
    return memory == RhiMemoryUsage::GpuOnly ? "GpuOnly" : "CpuToGpu";
}

auto drawHeaps(const RenderDebugSnapshot& s) -> void {
    char a[32];
    char b[32];
    char c[32];
    for (size_t i = 0; i < s.heaps.size(); i++) {
        const auto& h = s.heaps[i];
        ImGui::Text("Heap %zu (%s): size %s", i, h.deviceLocal ? "device-local" : "host", formatByteCount(h.size, a, sizeof(a)));
        if (h.budget > 0) {
            auto fraction = (float) ((double) h.usage / (double) h.budget);
            char label[96];
            std::snprintf(label, sizeof(label), "driver usage %s of budget %s", formatByteCount(h.usage, a, sizeof(a)), formatByteCount(h.budget, b, sizeof(b)));
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), label);
        }
        ImGui::TextDisabled("  allocated through the RHI: %s", formatByteCount(h.allocated, c, sizeof(c)));
    }
}

} // namespace

auto drawMemoryWindow(bool& show, const std::optional<RenderDebugSnapshot>& snap, MemoryWindowState& state) -> void {
    if (!ImGui::Begin("Memory", &show)) {
        ImGui::End();
        return;
    }
    if (!snap.has_value()) {
        ImGui::TextDisabled("Waiting for the render thread...");
        ImGui::End();
        return;
    }
    const auto& s = *snap;
    char buf[32];

    uint64_t total = 0;
    uint64_t gpuOnly = 0;
    uint64_t cpuToGpu = 0;
    std::map<std::string, std::pair<uint64_t, uint32_t>> categories;
    for (const auto& a : s.allocations) {
        total += a.bytes;
        if (a.memory == RhiMemoryUsage::GpuOnly) {
            gpuOnly += a.bytes;
        } else {
            cpuToGpu += a.bytes;
        }
        auto& cat = categories[allocationCategory(a.name)];
        cat.first += a.bytes;
        cat.second++;
    }
    ImGui::Text("Frame #%llu: %zu allocations, %s", (unsigned long long) s.frameIndex, s.allocations.size(), formatByteCount(total, buf, sizeof(buf)));
    char b2[32];
    ImGui::Text("GpuOnly %s   CpuToGpu %s", formatByteCount(gpuOnly, buf, sizeof(buf)), formatByteCount(cpuToGpu, b2, sizeof(b2)));
    if (ImGui::CollapsingHeader("Heaps", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawHeaps(s);
    }

    if (ImGui::CollapsingHeader("Categories", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::vector<std::pair<std::string, std::pair<uint64_t, uint32_t>>> sorted(categories.begin(), categories.end());
        std::ranges::sort(sorted, [](const auto& x, const auto& y) { return x.second.first > y.second.first; });
        if (ImGui::BeginTable("##categories", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Allocations", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();
            for (const auto& [name, value] : sorted) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    std::snprintf(state.filter, sizeof(state.filter), "%s", name.c_str());
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u", value.second);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(formatByteCount(value.first, buf, sizeof(buf)));
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Allocations", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Filter", state.filter, sizeof(state.filter));
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            state.filter[0] = '\0';
        }
        std::vector<const RhiAllocationInfo*> rows;
        for (const auto& a : s.allocations) {
            if (state.filter[0] == '\0' || a.name.find(state.filter) != std::string::npos) {
                rows.push_back(&a);
            }
        }
        auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("##allocations", 6, flags, ImVec2(0.0f, 320.0f))) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 50.0f);
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 100.0f);
            ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_NoSort);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            if (auto* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsCount > 0) {
                auto column = specs->Specs[0].ColumnIndex;
                bool ascending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                auto compare = [column](const RhiAllocationInfo* x, const RhiAllocationInfo* y) -> int {
                    auto order = [](auto a, auto b) -> int {
                        if (a < b) {
                            return -1;
                        }
                        if (b < a) {
                            return 1;
                        }
                        return 0;
                    };
                    if (column == 1) {
                        return order(x->name, y->name);
                    }
                    if (column == 2) {
                        return order(x->kind, y->kind);
                    }
                    if (column == 3) {
                        return order(x->memory, y->memory);
                    }
                    if (column == 4) {
                        return order(x->bytes, y->bytes);
                    }
                    return order(x->sequence, y->sequence);
                };
                std::ranges::stable_sort(rows, [&](const RhiAllocationInfo* x, const RhiAllocationInfo* y) {
                    auto c = compare(x, y);
                    return ascending ? c < 0 : c > 0;
                });
            }
            for (const auto* a : rows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%llu", (unsigned long long) a->sequence);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(a->name.empty() ? "(unnamed)" : a->name.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(a->kind == RhiAllocationInfo::Kind::Buffer ? "buffer" : "texture");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(memoryUsageName(a->memory));
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(formatByteCount(a->bytes, buf, sizeof(buf)));
                ImGui::TableSetColumnIndex(5);
                if (a->kind == RhiAllocationInfo::Kind::Texture) {
                    ImGui::Text("%ux%u %s, %u mips, %u layers", a->width, a->height, toString(a->format), a->mipLevels, a->arrayLayers);
                } else {
                    ImGui::Text("requested %llu B, usage 0x%x", (unsigned long long) a->requested, a->usageBits);
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
