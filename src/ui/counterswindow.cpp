#include "counterswindow.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <vector>

namespace {

auto statsValue(const RhiPipelineStats& s, int metric) -> uint64_t {
    switch (metric) {
        case 1:
            return s.iaVertices;
        case 2:
            return s.iaPrimitives;
        case 3:
            return s.vertexInvocations;
        case 4:
            return s.clippingInvocations;
        case 5:
            return s.clippingPrimitives;
        case 6:
            return s.fragmentInvocations;
        case 7:
            return s.computeInvocations;
        default:
            return 0;
    }
}

auto findStats(const GpuCounters& frame, const std::string& name) -> const RhiPipelineStats* {
    for (const auto& p : frame.passes) {
        if (p.name == name) {
            return &p.stats;
        }
    }
    return nullptr;
}

auto formatCount(uint64_t value) -> std::string {
    if (value >= 10'000'000) {
        return std::format("{:.1f}M", (double) value * 1e-6);
    }
    if (value >= 10'000) {
        return std::format("{:.1f}k", (double) value * 1e-3);
    }
    return std::format("{}", value);
}

// One selectable cell; clicking plots that zone's metric.
auto metricCell(CountersWindowState& state, const std::string& zone, int metric, const std::string& text) -> void {
    bool selected = state.plotZone == zone && state.plotMetric == metric;
    if (ImGui::Selectable(std::format("{}##{}.{}", text, zone, metric).c_str(), selected)) {
        state.plotZone = zone;
        state.plotMetric = metric;
    }
}

} // namespace

auto countersMetricName(int metric) -> const char* {
    static constexpr const char* names[countersMetricCount] = {
        "GPU ms",
        "IA vertices",
        "IA primitives",
        "VS invocations",
        "Clip in",
        "Clip out",
        "FS invocations",
        "CS invocations",
    };
    return metric >= 0 && metric < countersMetricCount ? names[metric] : "?";
}

auto countersMetricValue(const GpuCounters& frame, const std::string& zone, int metric, double& out) -> bool {
    if (metric == 0) {
        // Nested zones are keyed "Pass/zone": region names repeat across passes.
        std::string pass;
        for (const auto& z : frame.zones) {
            if (z.depth == 0) {
                pass = z.name;
            }
            auto key = z.depth == 0 ? z.name : pass + "/" + z.name;
            if (key == zone) {
                out = z.ms;
                return true;
            }
        }
        return false;
    }
    const auto* stats = findStats(frame, zone);
    if (stats == nullptr) {
        return false;
    }
    out = (double) statsValue(*stats, metric);
    return true;
}

auto addCountersFrame(CountersWindowState& state, GpuCounters frame) -> void {
    if (state.paused) {
        return;
    }
    state.history.push_back(std::move(frame));
    while (state.history.size() > CountersWindowState::historySize) {
        state.history.pop_front();
    }
}

auto drawCountersWindow(bool& show, CountersWindowState& state) -> void {
    ImGui::SetNextWindowSize(ImVec2(1100, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Counters", &show)) {
        ImGui::End();
        return;
    }
    if (state.history.empty()) {
        ImGui::TextDisabled("Waiting for GPU counters...");
        ImGui::End();
        return;
    }
    const auto& frame = state.history.back();
    ImGui::Checkbox("Pause", &state.paused);
    ImGui::SameLine();
    ImGui::Text("Frame %llu   GPU %.3f ms   %ux%u", (unsigned long long) frame.frame, frame.gpuFrameMs, frame.width, frame.height);
    if (frame.passes.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(no pipeline statistics on this device)");
    }

    // History of the selected counter.
    std::vector<float> values;
    values.reserve(state.history.size());
    double minValue = 1e300;
    double maxValue = 0.0;
    double sum = 0.0;
    for (const auto& f : state.history) {
        double v = 0.0;
        if (countersMetricValue(f, state.plotZone, state.plotMetric, v)) {
            values.push_back((float) v);
            minValue = std::min(minValue, v);
            maxValue = std::max(maxValue, v);
            sum += v;
        }
    }
    auto label = std::format("{} / {}", state.plotZone, countersMetricName(state.plotMetric));
    if (!values.empty()) {
        auto overlay = std::format("{}: last {:.4g}  avg {:.4g}  min {:.4g}  max {:.4g}", label, values.back(), sum / (double) values.size(), minValue, maxValue);
        ImGui::PlotLines("##history", values.data(), (int) values.size(), 0, overlay.c_str(), 0.0f, (float) maxValue * 1.1f, ImVec2(-1.0f, 110.0f));
    } else {
        ImGui::TextDisabled("%s: no samples", label.c_str());
    }
    ImGui::TextDisabled("Click a cell to plot it.");

    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##counters", 1 + countersMetricCount + 1, flags)) {
        ImGui::TableSetupColumn("Pass / zone", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        for (int m = 0; m < countersMetricCount; m++) {
            ImGui::TableSetupColumn(countersMetricName(m));
        }
        ImGui::TableSetupColumn("Ratios", ImGuiTableColumnFlags_WidthFixed, 260.0f);
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableHeadersRow();
        auto pixels = (double) frame.width * (double) frame.height;
        for (size_t i = 0; i < frame.zones.size(); i++) {
            const auto& zone = frame.zones[i];
            if (zone.depth != 0) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool hasChildren = i + 1 < frame.zones.size() && frame.zones[i + 1].depth > 0;
            bool open = false;
            if (hasChildren) {
                open = ImGui::TreeNodeEx(zone.name.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
            } else {
                ImGui::TreeNodeEx(zone.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
            }
            ImGui::TableSetColumnIndex(1);
            metricCell(state, zone.name, 0, std::format("{:.3f}", zone.ms));
            const auto* stats = findStats(frame, zone.name);
            if (stats != nullptr) {
                for (int m = 1; m < countersMetricCount; m++) {
                    ImGui::TableSetColumnIndex(1 + m);
                    metricCell(state, zone.name, m, formatCount(statsValue(*stats, m)));
                }
                ImGui::TableSetColumnIndex(1 + countersMetricCount);
                std::string ratios;
                if (stats->fragmentInvocations > 0 && pixels > 0.0) {
                    ratios += std::format("{:.2f} FS/px  ", (double) stats->fragmentInvocations / pixels);
                }
                if (stats->clippingInvocations > 0) {
                    ratios += std::format("{:.0f}% survive clip  ", 100.0 * (double) stats->clippingPrimitives / (double) stats->clippingInvocations);
                }
                if (stats->iaPrimitives > 0) {
                    ratios += std::format("{:.2f} VS/prim", (double) stats->vertexInvocations / (double) stats->iaPrimitives);
                }
                ImGui::TextUnformatted(ratios.c_str());
            }
            if (open) {
                for (size_t j = i + 1; j < frame.zones.size() && frame.zones[j].depth > 0; j++) {
                    const auto& child = frame.zones[j];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Indent((float) (child.depth - 1) * 12.0f);
                    ImGui::TreeNodeEx(std::format("{}##{}", child.name, j).c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth);
                    ImGui::Unindent((float) (child.depth - 1) * 12.0f);
                    ImGui::TableSetColumnIndex(1);
                    metricCell(state, zone.name + "/" + child.name, 0, std::format("{:.3f}", child.ms));
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
