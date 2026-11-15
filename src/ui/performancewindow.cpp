#include "performancewindow.h"

#include "trace.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <vector>

namespace {

constexpr uint32_t gpuTrack = UINT32_MAX;

auto isWorkerTrack(const profile::TrackInfo& track) -> bool;
auto maxDepth(const std::vector<profile::Zone>& zones) -> uint32_t;
auto keepGuideZones(std::vector<profile::Zone>& zones) -> void;
auto drawOverBudget() -> void;
constexpr double minSpanNs = 0.2e6; // 0.2 ms

// Reads everything the view needs. History and the view position refresh only while
// live; paused, the view stays where the user put it and the data underneath is frozen.
auto gather(PerformanceWindowState& state) -> void {
    if (!state.paused) {
        profile::frameHistory(state.history);
    }
    auto newest = profile::lastFrame();
    if (!newest.has_value()) {
        state.valid = false;
        return;
    }
    if (state.following) {
        // Right edge slightly behind the newest completed frame so the GPU track, which lags
        // by the frames in flight, is visible for most of the view.
        state.viewEndNs = newest->endNs;
        state.viewStartNs = state.viewEndNs > (uint64_t) state.viewSpanNs ? state.viewEndNs - (uint64_t) state.viewSpanNs : 0;
    }
    profile::tracks(state.tracks);
    state.trackZones.resize(state.tracks.size());
    for (size_t i = 0; i < state.tracks.size(); i++) {
        profile::zonesIn(state.tracks[i].index, state.viewStartNs, state.viewEndNs, state.trackZones[i]);
        if (!state.showDetail) {
            keepGuideZones(state.trackZones[i]);
        }
    }
    profile::gpuZonesIn(state.viewStartNs, state.viewEndNs, state.gpuZones);
    if (!state.showDetail) {
        keepGuideZones(state.gpuZones);
    }
    profile::framesIn(state.viewStartNs, state.viewEndNs, state.frames);

    // Workers are interchangeable, so they share one track. Each worker's zones form a
    // block (maxDepth + 1 rows) placed on the first rows where it does not overlap another
    // worker's block in time.
    state.workerZones.clear();
    state.workerZoneTracks.clear();
    state.workerCount = 0;
    struct RowUse {
        uint64_t busyUntilNs = 0;
    };
    std::vector<RowUse> rows;
    for (size_t i = 0; i < state.tracks.size(); i++) {
        if (!isWorkerTrack(state.tracks[i])) {
            continue;
        }
        state.workerCount++;
        const auto& zones = state.trackZones[i];
        if (zones.empty()) {
            continue;
        }
        uint64_t blockStart = UINT64_MAX;
        uint64_t blockEnd = 0;
        for (const auto& z : zones) {
            blockStart = std::min(blockStart, z.startNs);
            blockEnd = std::max(blockEnd, z.endNs);
        }
        auto height = maxDepth(zones) + 1;
        uint32_t row = 0;
        while (true) {
            bool free = true;
            for (uint32_t r = row; r < row + height; r++) {
                if (r < rows.size() && rows[r].busyUntilNs > blockStart) {
                    free = false;
                    break;
                }
            }
            if (free) {
                break;
            }
            row++;
        }
        if (rows.size() < row + height) {
            rows.resize(row + height);
        }
        for (uint32_t r = row; r < row + height; r++) {
            rows[r].busyUntilNs = blockEnd;
        }
        for (const auto& z : zones) {
            auto packed = z;
            packed.depth = (uint16_t) (row + z.depth);
            state.workerZones.push_back(packed);
            state.workerZoneTracks.push_back(state.tracks[i].index);
        }
    }
    state.valid = true;
}

auto isWorkerTrack(const profile::TrackInfo& track) -> bool {
    return track.name.rfind("Worker ", 0) == 0;
}

// The zone's group colour, as in Tracy.
auto zoneColor(uint32_t locationId) -> ImU32 {
    auto rgb = profile::groupColor(profile::groupOf(locationId), locationId);
    return IM_COL32((int) ((rgb >> 16) & 0xFF), (int) ((rgb >> 8) & 0xFF), (int) (rgb & 0xFF), 255);
}

// Drops detail zones and nests the guide zones among themselves: a guide zone inside a detail zone moves up a row.
// Under the frame times: per thread that marks frames, the frames whose own work (less waits) was over the budget since start or the
// last reset, and the worst with the zone that held it; the profile record's overBudget.
auto drawOverBudget() -> void {
    profile::OverBudget budget;
    profile::overBudget(budget);
    auto t = profile::now();
    if (budget.resetNs == 0) {
        ImGui::Text("Own work over %.1f ms since start:", budget.budgetMs);
    } else {
        ImGui::Text("Own work over %.1f ms since reset %.0f s ago:", budget.budgetMs, (double) (t - std::min(t, budget.resetNs)) * 1e-9);
    }
    for (const auto& track : budget.tracks) {
        ImGui::SameLine(0.0f, 16.0f);
        auto zone = track.worstLocationId != 0 ? std::string(profile::nameOf(track.worstLocationId)) : std::string("no zone named");
        auto text = std::format("{} {} of {} frames, worst {:.1f} ms ({})", track.track, track.over, track.frames, track.worstMs, zone);
        if (track.over > 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s", text.c_str());
        } else {
            ImGui::TextUnformatted(text.c_str());
        }
        if (ImGui::IsItemHovered() && track.frames > 0) {
            auto ago = (double) (t - std::min(t, track.worstEndNs)) * 1e-9;
            if (track.worstLocationId != 0) {
                auto source = profile::sourceLocationOf(track.worstLocationId);
                ImGui::SetTooltip("Worst frame %.0f s ago: %.1f ms of its own work, %.1f ms of it in %s\n%s:%u",
                                  ago,
                                  track.worstMs,
                                  track.worstZoneMs,
                                  source.name,
                                  source.file,
                                  source.line);
            } else {
                ImGui::SetTooltip("Worst frame %.0f s ago: %.1f ms of its own work", ago, track.worstMs);
            }
        }
    }
    ImGui::SameLine(0.0f, 16.0f);
    if (ImGui::SmallButton("Reset##overbudget")) {
        profile::resetOverBudget();
        TRACE_EVENT("Engine", "ProfileReset", "view").text("frames over budget counted from now");
    }
}

auto keepGuideZones(std::vector<profile::Zone>& zones) -> void {
    std::erase_if(zones, [](const profile::Zone& z) { return profile::levelOf(z.locationId) == profile::Level::Detail; });
    std::sort(zones.begin(), zones.end(), [](const profile::Zone& a, const profile::Zone& b) {
        if (a.startNs != b.startNs) {
            return a.startNs < b.startNs;
        }
        return a.depth < b.depth;
    });
    std::vector<uint64_t> openEnds;
    for (auto& z : zones) {
        while (!openEnds.empty() && openEnds.back() <= z.startNs) {
            openEnds.pop_back();
        }
        z.depth = (uint16_t) openEnds.size();
        openEnds.push_back(z.endNs);
    }
}

auto maxDepth(const std::vector<profile::Zone>& zones) -> uint32_t {
    uint32_t depth = 0;
    for (const auto& z : zones) {
        depth = std::max<uint32_t>(depth, z.depth);
    }
    return depth;
}

struct Axis {
    float left = 0.0f;  // pixel x of viewStartNs
    float width = 0.0f; // pixels for the whole view
    uint64_t startNs = 0;
    double nsPerPixel = 1.0;

    auto x(uint64_t ns) const -> float {
        double rel = (double) ((int64_t) ns - (int64_t) startNs);
        return left + (float) (rel / nsPerPixel);
    }
};

// Frame ruler: one tick and number per main-thread frame in view.
auto drawRuler(const PerformanceWindowState& state, const Axis& axis, float height) -> void {
    auto* draw = ImGui::GetWindowDrawList();
    auto origin = ImGui::GetCursorScreenPos();
    draw->AddRectFilled(origin, ImVec2(origin.x + axis.width + (axis.left - origin.x), origin.y + height), IM_COL32(24, 24, 28, 255));
    for (const auto& f : state.frames) {
        auto x0 = std::clamp(axis.x(f.startNs), axis.left, axis.left + axis.width);
        auto x1 = std::clamp(axis.x(f.endNs), axis.left, axis.left + axis.width);
        draw->AddLine(ImVec2(x0, origin.y), ImVec2(x0, origin.y + height), IM_COL32(120, 120, 130, 255));
        char text[48];
        std::snprintf(text, sizeof(text), "#%llu %.2f ms", (unsigned long long) f.frameIndex, (double) (f.endNs - f.startNs) * 1e-6);
        if (x1 - x0 > ImGui::CalcTextSize(text).x + 8.0f) {
            draw->AddText(ImVec2(x0 + 4.0f, origin.y + 2.0f), IM_COL32(190, 190, 200, 255), text);
        }
    }
    ImGui::Dummy(ImVec2(axis.left - origin.x + axis.width, height + 2.0f));
}

// One track: nested bars on the shared axis. A click selects the bar.
auto drawTrack(const char* label, uint32_t trackKey, const std::vector<profile::Zone>& zones, const Axis& axis, float rowHeight, PerformanceWindowState& state, const std::vector<uint32_t>* zoneTracks = nullptr) -> void {
    auto* draw = ImGui::GetWindowDrawList();
    auto depth = maxDepth(zones) + 1;
    auto trackHeight = rowHeight * (float) depth;
    auto origin = ImGui::GetCursorScreenPos();
    auto totalWidth = axis.left - origin.x + axis.width;

    draw->AddRectFilled(origin, ImVec2(origin.x + totalWidth, origin.y + trackHeight), IM_COL32(30, 30, 34, 255));
    draw->AddText(ImVec2(origin.x + 4.0f, origin.y + 2.0f), IM_COL32(200, 200, 200, 255), label);

    ImVec2 mouse = ImGui::GetIO().MousePos;
    bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    auto right = axis.left + axis.width;

    for (size_t zi = 0; zi < zones.size(); zi++) {
        const auto& z = zones[zi];
        auto zoneTrack = zoneTracks != nullptr ? (*zoneTracks)[zi] : trackKey;
        auto x0 = std::clamp(axis.x(z.startNs), axis.left, right);
        auto x1 = std::clamp(axis.x(z.endNs), axis.left, right);
        if (x1 - x0 < 1.0f) {
            x1 = x0 + 1.0f;
        }
        auto y0 = origin.y + rowHeight * (float) z.depth;
        auto y1 = y0 + rowHeight - 1.0f;
        bool selected = state.hasSelection && state.selectedTrack == zoneTrack && state.selectedZone.startNs == z.startNs && state.selectedZone.locationId == z.locationId;
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), zoneColor(z.locationId));
        if (selected) {
            draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
        }

        auto ms = (double) (z.endNs - z.startNs) * 1e-6;
        char text[96];
        std::snprintf(text, sizeof(text), "%s %.2f ms", profile::nameOf(z.locationId), ms);
        auto textWidth = ImGui::CalcTextSize(text).x;
        if (x1 - x0 > textWidth + 6.0f) {
            draw->AddText(ImVec2(x0 + 3.0f, y0 + 1.0f), IM_COL32(15, 15, 15, 255), text);
        } else if (x1 - x0 > 24.0f) {
            draw->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
            draw->AddText(ImVec2(x0 + 3.0f, y0 + 1.0f), IM_COL32(15, 15, 15, 255), profile::nameOf(z.locationId));
            draw->PopClipRect();
        }
        bool hovered = mouse.x >= x0 && mouse.x <= x1 && mouse.y >= y0 && mouse.y <= y1;
        if (hovered) {
            double startMs = (double) ((int64_t) z.startNs - (int64_t) axis.startNs) * 1e-6;
            ImGui::SetTooltip("%s\n%.3f ms\nat %.3f ms in view\nclick for details", profile::nameOf(z.locationId), ms, startMs);
            if (clicked) {
                state.hasSelection = true;
                state.selectedTrack = zoneTrack;
                state.selectedZone = z;
                state.selectedZone.depth = zoneTracks != nullptr ? 0 : z.depth; // packed row is not the real depth
            }
        }
    }

    ImGui::Dummy(ImVec2(totalWidth, trackHeight + 4.0f));
}

// Detail pane for the selected zone: self time, children, ancestry, history statistics.
auto drawZoneDetails(const PerformanceWindowState& state) -> void {
    const auto& zone = state.selectedZone;
    const std::vector<profile::Zone>* trackZones = nullptr;
    const char* trackName = "GPU";
    if (state.selectedTrack == gpuTrack) {
        trackZones = &state.gpuZones;
    } else {
        for (size_t i = 0; i < state.tracks.size(); i++) {
            if (state.tracks[i].index == state.selectedTrack) {
                trackZones = &state.trackZones[i];
                trackName = state.tracks[i].name.c_str();
            }
        }
    }
    if (trackZones == nullptr) {
        ImGui::TextDisabled("Selected zone is no longer in view.");
        return;
    }

    auto durationMs = (double) (zone.endNs - zone.startNs) * 1e-6;
    double childrenMs = 0.0;
    std::vector<const profile::Zone*> children;
    std::vector<const profile::Zone*> ancestors;
    for (const auto& z : *trackZones) {
        bool inside = z.startNs >= zone.startNs && z.endNs <= zone.endNs;
        if (inside && z.depth == zone.depth + 1) {
            children.push_back(&z);
            childrenMs += (double) (z.endNs - z.startNs) * 1e-6;
        }
        bool contains = z.startNs <= zone.startNs && z.endNs >= zone.endNs && z.depth < zone.depth;
        if (contains) {
            ancestors.push_back(&z);
        }
    }
    std::sort(ancestors.begin(), ancestors.end(), [](const profile::Zone* a, const profile::Zone* b) { return a->depth < b->depth; });
    std::sort(children.begin(), children.end(), [](const profile::Zone* a, const profile::Zone* b) { return a->startNs < b->startNs; });

    // Frame containing the zone, for the share figure.
    double frameMs = 0.0;
    for (const auto& f : state.frames) {
        if (zone.startNs >= f.startNs && zone.startNs < f.endNs) {
            frameMs = (double) (f.endNs - f.startNs) * 1e-6;
        }
    }

    ImGui::Text("%s", profile::nameOf(zone.locationId));
    ImGui::SameLine();
    ImGui::TextDisabled("on %s", trackName);
    auto source = profile::sourceLocationOf(zone.locationId);
    if (source.line != 0) {
        ImGui::TextDisabled("%s:%u   %s", source.file, source.line, source.function);
    }
    if (!ancestors.empty()) {
        std::string chain;
        for (const auto* a : ancestors) {
            chain += profile::nameOf(a->locationId);
            chain += " > ";
        }
        chain += profile::nameOf(zone.locationId);
        ImGui::TextDisabled("%s", chain.c_str());
    }
    if (frameMs > 0.0) {
        ImGui::Text("Duration %.3f ms   self %.3f ms   children %.3f ms   %.1f%% of its frame", durationMs, durationMs - childrenMs, childrenMs, durationMs / frameMs * 100.0);
    } else {
        ImGui::Text("Duration %.3f ms   self %.3f ms   children %.3f ms", durationMs, durationMs - childrenMs, childrenMs);
    }
    if (zone.hasValue) {
        ImGui::Text("Value %llu", (unsigned long long) zone.value);
    }

    auto stats = profile::zoneStats(state.selectedTrack, zone.locationId);
    if (stats.count > 0) {
        ImGui::Text("History (%u samples): min %.3f   avg %.3f   p99 %.3f   max %.3f ms", stats.count, stats.minMs, stats.avgMs, stats.p99Ms, stats.maxMs);
    }

    if (!children.empty()) {
        if (ImGui::BeginTable("##children", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Child");
            ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("% of parent", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();
            for (const auto* c : children) {
                auto ms = (double) (c->endNs - c->startNs) * 1e-6;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(profile::nameOf(c->locationId));
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.3f", ms);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.1f", durationMs > 0.0 ? ms / durationMs * 100.0 : 0.0);
            }
            ImGui::EndTable();
        }
    }
}

// A plot's latest value as text in its format.
auto plotValueText(const profile::PlotInfo& info, double value) -> std::string {
    if (info.format == profile::PlotFormat::Bytes) {
        return std::format("{:.1f} MiB", value / (1024.0 * 1024.0));
    }
    if (info.format == profile::PlotFormat::Percent) {
        return std::format("{:.0f}%", value);
    }
    return std::format("{:.0f}", value);
}

// The guide plots under the frame times: GPU busy, memory, uploads. Every plot is in Tracy and the plots record; these are the few that
// say where a frame stands.
auto drawGuidePlots() -> void {
    std::vector<profile::PlotInfo> list;
    profile::plots(list);
    auto wanted = [](const std::string& name) {
        return name == "GPU busy" || name == "GPU busy (driver)" || name == "Resident memory" || name == "Upload bytes" ||
               (name.starts_with("GPU heap") && name.contains("(device)") && !name.ends_with("allocated"));
    };
    std::vector<profile::PlotSample> samples;
    std::vector<float> values;
    if (!ImGui::BeginTable("##guideplots", 3, ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("plot", ImGuiTableColumnFlags_WidthFixed, 190.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("samples");
    for (const auto& info : list) {
        if (!wanted(info.name) || info.samples == 0) {
            continue;
        }
        profile::plotSamplesIn(info.id, 0, UINT64_MAX, samples);
        values.clear();
        for (const auto& sample : samples) {
            values.push_back((float) sample.value);
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(info.name.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(plotValueText(info, info.latest).c_str());
        ImGui::TableSetColumnIndex(2);
        auto scaleMax = info.format == profile::PlotFormat::Percent ? 100.0f : (float) std::max(info.max, 1.0);
        ImGui::PushID((int) info.id);
        ImGui::PlotLines("##plot", values.data(), (int) values.size(), 0, nullptr, 0.0f, scaleMax, ImVec2(ImGui::GetContentRegionAvail().x, 18.0f));
        ImGui::PopID();
    }
    ImGui::EndTable();
}

auto setPaused(PerformanceWindowState& state, bool paused) -> void {
    state.paused = paused;
    profile::setPaused(paused);
    if (paused) {
        state.following = false;
    }
}

} // namespace

void drawPerformanceWindow(bool& show, PerformanceWindowState& state) {
    if (!show) {
        if (state.paused) {
            setPaused(state, false);
            state.following = true;
        }
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(900, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Performance", &show)) {
        ImGui::End();
        return;
    }

    gather(state);
    if (!state.valid) {
        ImGui::TextDisabled("No frames recorded yet.");
        ImGui::End();
        return;
    }

    // Header: averages over the history window plus the latest frame.
    double sumCpu = 0.0;
    double maxCpu = 0.0;
    for (const auto& f : state.history) {
        sumCpu += f.cpuMs;
        maxCpu = std::max(maxCpu, f.cpuMs);
    }
    auto count = std::max<size_t>(1, state.history.size());
    auto avgCpu = sumCpu / (double) count;
    auto latest = state.history.empty() ? profile::FrameStats{} : state.history.back();
    auto fps = avgCpu > 0.0 ? 1000.0 / avgCpu : 0.0;

    ImGui::Text("%.1f FPS   frame %.2f ms (avg %.2f, max %.2f)   GPU %.2f ms   history %.0f s%s",
                fps,
                latest.cpuMs,
                avgCpu,
                maxCpu,
                latest.gpuMs,
                profile::historySeconds(),
                state.paused ? "   [paused]" : "");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200.0f);
    ImGui::Checkbox("Detail", &state.showDetail);
    ImGui::SameLine();
    if (ImGui::Button(state.paused ? "Live" : "Pause")) {
        if (state.paused) {
            setPaused(state, false);
            state.following = true;
        } else {
            setPaused(state, true);
        }
    }
    if (!state.following) {
        ImGui::SameLine();
        if (ImGui::Button("Follow")) {
            state.following = true;
        }
    }

    // Frame time plot; clicking a frame centres the view on it and pauses.
    {
        std::vector<float> values;
        values.reserve(state.history.size());
        for (const auto& f : state.history) {
            values.push_back((float) f.cpuMs);
        }
        auto scaleMax = std::max(20.0f, (float) maxCpu * 1.1f);
        auto plotPos = ImGui::GetCursorScreenPos();
        auto plotSize = ImVec2(ImGui::GetContentRegionAvail().x, 70.0f);
        ImGui::PlotLines("##frametimes", values.data(), (int) values.size(), 0, nullptr, 0.0f, scaleMax, plotSize);
        auto* draw = ImGui::GetWindowDrawList();
        auto guideY = plotPos.y + plotSize.y * (1.0f - 16.667f / scaleMax);
        draw->AddLine(ImVec2(plotPos.x, guideY), ImVec2(plotPos.x + plotSize.x, guideY), IM_COL32(220, 160, 60, 160));
        if (!values.empty() && ImGui::IsItemHovered()) {
            auto rel = (ImGui::GetIO().MousePos.x - plotPos.x) / plotSize.x;
            auto index = (size_t) std::clamp(rel * (float) (values.size() - 1) + 0.5f, 0.0f, (float) values.size() - 1.0f);
            const auto& f = state.history[index];
            ImGui::SetTooltip("frame #%llu: %.2f ms", (unsigned long long) f.frameIndex, f.cpuMs);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                auto interval = profile::frameByIndex(f.frameIndex);
                if (interval.has_value()) {
                    setPaused(state, true);
                    auto frameSpan = (double) (interval->endNs - interval->startNs);
                    state.viewSpanNs = std::clamp(frameSpan * 3.0, minSpanNs, std::max(minSpanNs, profile::historySeconds() * 1e9));
                    auto centre = (double) interval->startNs + frameSpan * 0.5;
                    state.viewStartNs = (uint64_t) std::max(0.0, centre - state.viewSpanNs * 0.5);
                    state.viewEndNs = state.viewStartNs + (uint64_t) state.viewSpanNs;
                }
            }
        }
        // Mark the part of the history that is in view.
        if (!state.history.empty()) {
            auto firstIndex = state.history.front().frameIndex;
            auto lastIndex = state.history.back().frameIndex;
            auto span = (double) std::max<uint64_t>(1, lastIndex - firstIndex);
            for (const auto& f : state.frames) {
                if (f.frameIndex < firstIndex || f.frameIndex > lastIndex) {
                    continue;
                }
                auto x = plotPos.x + plotSize.x * (float) ((double) (f.frameIndex - firstIndex) / span);
                draw->AddLine(ImVec2(x, plotPos.y + plotSize.y - 6.0f), ImVec2(x, plotPos.y + plotSize.y), IM_COL32(255, 255, 255, 160));
            }
        }
    }

    drawOverBudget();
    drawGuidePlots();

    ImGui::Separator();
    ImGui::TextDisabled("Drag to pan, wheel to zoom, click a bar for details. All tracks share the CPU clock; the GPU track is calibrated when the device allows, else anchored at submit.");

    // Timeline area: ruler + tracks. Pan and zoom act on the view when the mouse is over it.
    auto labelWidth = 90.0f;
    auto areaPos = ImGui::GetCursorScreenPos();
    auto areaWidth = std::max(200.0f, ImGui::GetContentRegionAvail().x);
    Axis axis;
    axis.left = areaPos.x + labelWidth;
    axis.width = areaWidth - labelWidth;
    axis.startNs = state.viewStartNs;
    axis.nsPerPixel = (double) (state.viewEndNs - state.viewStartNs) / (double) axis.width;

    float detailHeight = state.hasSelection ? 170.0f : 0.0f;
    ImGui::BeginChild("##timeline", ImVec2(0, -detailHeight), ImGuiChildFlags_Borders);
    auto rowHeight = ImGui::GetTextLineHeight() + 4.0f;

    // An invisible button under the whole timeline owns the mouse, so dragging pans the
    // view instead of moving the window. Drawing happens on top of it.
    float contentHeight = rowHeight + 2.0f;
    for (size_t i = 0; i < state.trackZones.size(); i++) {
        if (isWorkerTrack(state.tracks[i])) {
            continue;
        }
        contentHeight += rowHeight * (float) (maxDepth(state.trackZones[i]) + 1) + 4.0f;
    }
    contentHeight += rowHeight * (float) (maxDepth(state.gpuZones) + 1) + 4.0f;
    contentHeight += rowHeight * (float) (maxDepth(state.workerZones) + 1) + 4.0f;
    auto contentPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##timeline_area", ImVec2(areaWidth, std::max(contentHeight, ImGui::GetContentRegionAvail().y)), ImGuiButtonFlags_MouseButtonLeft);
    bool areaHovered = ImGui::IsItemHovered();
    bool areaActive = ImGui::IsItemActive();
    ImGui::SetCursorScreenPos(contentPos);

    drawRuler(state, axis, rowHeight);
    for (size_t i = 0; i < state.tracks.size(); i++) {
        if (isWorkerTrack(state.tracks[i])) {
            continue;
        }
        drawTrack(state.tracks[i].name.c_str(), state.tracks[i].index, state.trackZones[i], axis, rowHeight, state);
    }
    drawTrack("GPU", gpuTrack, state.gpuZones, axis, rowHeight, state);
    char workersLabel[32];
    std::snprintf(workersLabel, sizeof(workersLabel), "Workers (%u)", state.workerCount);
    drawTrack(workersLabel, 0, state.workerZones, axis, rowHeight, state, &state.workerZoneTracks);

    if (areaHovered || areaActive) {
        auto& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            // Zoom around the cursor; zooming pauses so the view does not run away.
            auto mouseNs = (double) axis.startNs + (double) (io.MousePos.x - axis.left) * axis.nsPerPixel;
            auto factor = io.MouseWheel > 0.0f ? 0.8 : 1.25;
            auto newSpan = std::clamp((double) (state.viewEndNs - state.viewStartNs) * factor, minSpanNs, std::max(minSpanNs, profile::historySeconds() * 1e9));
            auto frac = (double) (io.MousePos.x - axis.left) / (double) axis.width;
            auto newStart = mouseNs - newSpan * frac;
            state.viewStartNs = (uint64_t) std::max(0.0, newStart);
            state.viewEndNs = state.viewStartNs + (uint64_t) newSpan;
            state.viewSpanNs = newSpan;
            if (!state.paused) {
                setPaused(state, true);
            }
        }
        if (areaActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            auto deltaNs = (double) io.MouseDelta.x * axis.nsPerPixel;
            auto span = state.viewEndNs - state.viewStartNs;
            double newStart = (double) state.viewStartNs - deltaNs;
            state.viewStartNs = (uint64_t) std::max(0.0, newStart);
            state.viewEndNs = state.viewStartNs + span;
            if (!state.paused) {
                setPaused(state, true);
            }
        }
    }
    ImGui::EndChild();

    if (state.hasSelection) {
        ImGui::BeginChild("##zone_detail", ImVec2(0, 0), ImGuiChildFlags_Borders);
        drawZoneDetails(state);
        ImGui::EndChild();
    }

    ImGui::End();
}
