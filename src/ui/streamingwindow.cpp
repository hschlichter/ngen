#include "streamingwindow.h"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace {

auto toImVec4(const glm::vec4& color) -> ImVec4 {
    return ImVec4(color.r, color.g, color.b, color.a);
}

auto toColor(const glm::vec4& color) -> ImU32 {
    return ImGui::ColorConvertFloat4ToU32(toImVec4(color));
}

auto listText(const std::vector<float>& values) -> std::string {
    std::string text;
    for (float value : values) {
        text += std::format("{}{:g}", text.empty() ? "" : ", ", value);
    }
    return text;
}

// "1, 2.5, 4" to its numbers; none for text that isn't a list of numbers. An empty text is an empty list.
auto parseList(const char* text) -> std::optional<std::vector<float>> {
    std::vector<float> values;
    const char* at = text;
    const char* end = text + std::strlen(text);
    while (at < end) {
        while (at < end && (*at == ' ' || *at == ',')) {
            at++;
        }
        if (at == end) {
            break;
        }
        float value = 0.0f;
        auto [next, error] = std::from_chars(at, end, value);
        if (error != std::errc()) {
            return std::nullopt;
        }
        values.push_back(value);
        at = next;
    }
    return values;
}

auto levelName(const StreamingWindowRow& row, int32_t level) -> std::string {
    if (level < 0) {
        return "-";
    }
    if ((size_t) level < row.levels.size()) {
        return std::format("{} {}", level, row.levels[(size_t) level].name);
    }
    return std::format("{}", level);
}

auto residentText(const StreamingWindowRow& row) -> std::string {
    std::string text;
    for (auto level : row.resident) {
        text += std::format("{}{}", text.empty() ? "" : ", ", level);
    }
    return text.empty() ? std::string("-") : text;
}

auto stateSwatch(StreamingRootState state, const char* id) -> void {
    ImGui::ColorButton(id, toImVec4(streamingStateColor(state)), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(12.0f, 12.0f));
}

// Under the selection: the root at or under the view's selected prim.
auto underPath(const std::string& path, const std::string& base) -> bool {
    return !base.empty() && (base == "/" || path == base || (path.size() > base.size() && path.starts_with(base) && path[base.size()] == '/'));
}

// The overview: the measured counters, the legend, and the strip chart of the last frames.
auto drawOverview(StreamingWindowState& state) -> void {
    const auto& t = state.totals;
    ImGui::Text("%u roots, %u levels resident; %u assets in flight", t.roots, t.residentLevels, t.inFlight);
    ImGui::SameLine();
    if (t.lateNow > 0 || t.holesNow > 0) {
        ImGui::TextColored(toImVec4(streamingStateColor(StreamingRootState::Late)), "%u late, %u drawing nothing now", t.lateNow, t.holesNow);
    } else {
        ImGui::TextDisabled("none late, none drawing nothing");
    }
    ImGui::Text("Since reset: %llu late switches (%.1f root-seconds coarser), %llu holes; %llu loads, %llu reloads, %llu released",
                (unsigned long long) t.lateSwitches,
                t.lateSeconds,
                (unsigned long long) t.holes,
                (unsigned long long) t.loads,
                (unsigned long long) t.reloads,
                (unsigned long long) t.released);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A late switch: a root selected a level that wasn't ready, and drew a coarser one meanwhile. A hole: a root drew nothing.\nA reload: a level loaded again after its root released it. Measured on the scene thread; nothing acts on them.");
    }
    ImGui::Text("Lead: %llu switches, shortest %.1f s, median %.1f s; %llu mesh packs arrived unwanted; %u authoring findings",
                (unsigned long long) t.leads,
                t.leadMin,
                t.leadMedian,
                (unsigned long long) t.arrivedUnwanted,
                t.findings);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lead: seconds from a level being ready to it being selected, 0 when it wasn't ready. Both directions count.\nArrived unwanted: asked for by a level no longer resident when it arrived (requests aren't cancelled).\nFindings: roots breaking the streaming authoring contract, with the radii in force.");
    }
    ImGui::TextDisabled("States:");
    for (auto s : {StreamingRootState::Drawn, StreamingRootState::Prefetched, StreamingRootState::Requested, StreamingRootState::Refused, StreamingRootState::Late, StreamingRootState::Hole, StreamingRootState::None}) {
        ImGui::SameLine();
        stateSwatch(s, std::format("##legend{}", (int) s).c_str());
        ImGui::SameLine(0.0f, 3.0f);
        ImGui::TextUnformatted(streamingStateName(s));
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  yellow stripes: authoring findings");

    // The chart: oldest on the left.
    auto plot = [&](const char* label, const std::array<float, StreamingWindowState::historyLength>& values, float now) {
        float most = std::max(1.0f, *std::ranges::max_element(values));
        ImGui::PlotLines(label, values.data(), (int) values.size(), (int) state.historyAt, std::format("{:.0f} now, at most {:.0f}", now, most).c_str(), 0.0f, most, ImVec2(0.0f, 36.0f));
    };
    float width = std::max(120.0f, (ImGui::GetContentRegionAvail().x / 3.0f) - 90.0f);
    ImGui::PushItemWidth(width);
    plot("in flight", state.inFlightHistory, (float) t.inFlight);
    ImGui::SameLine();
    plot("late", state.lateHistory, (float) t.lateNow);
    ImGui::SameLine();
    plot("holes", state.holeHistory, (float) t.holesNow);
    ImGui::PopItemWidth();
}

// The measurement controls and the viewport's layers.
auto drawControls(StreamingWindowState& state) -> void {
    if (ImGui::Button("Reset counters")) {
        state.resetRequested = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Zeroes the counters above, for a run measured from here (view.streaming.reset, streaming-reset).");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    ImGui::InputText("##flightprefix", state.flightPrefix, sizeof(state.flightPrefix));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The cameras to fly through: every camera the scene authors whose path starts with this, in path order.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::InputFloat("m/s", &state.flightSpeed, 0.0f, 0.0f, "%.0f");
    ImGui::SameLine();
    if (state.flying) {
        if (ImGui::Button("Stop flight")) {
            state.flyStopRequested = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.flightText.c_str());
    } else if (ImGui::Button("Fly")) {
        state.flyRequested = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Flies the camera through the cameras at the speed, by each frame's time (view.camera.fly, fly). A test driver:\nit moves the camera; what loads follows from where the camera is. Its numbers depend on the build, the cache and the machine.");
    }

    bool view = state.streamingView;
    if (ImGui::Checkbox("Streaming view", &view)) {
        state.streamingViewRequested = view;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Replaces the lit image: each mesh in the colour of its nearest streaming root's state (debugview streaming).");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Bounds", &state.bounds);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Each root's streaming bounds in the viewport, in its state's colour.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Rings", &state.rings);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The selected root's load radii (solid) and unload radii (faint), per boundary, around its bounds:\nthe camera inside a load ring makes that level resident; outside its unload ring lets it go.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("of every root", &state.ringsAll);
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &state.labels);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Over each root the list shows: its name, drawn / selected level, state and the state's name. Click one to select it.");
    }
}

// One root of the list: click selects, double-click frames.
auto drawRow(const StreamingWindowRow& row, StreamingWindowState& state) -> void {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeightWithSpacing());
    ImGui::PushID(row.path.c_str());
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    stateSwatch(row.rootState, "##state");
    ImGui::SameLine();
    bool selected = row.path == state.selectedRoot;
    auto flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick;
    auto label = row.tuned ? row.path + "  (tuned)" : row.path;
    if (ImGui::Selectable(label.c_str(), selected, flags, ImVec2(0.0f, ImGui::GetFrameHeight()))) {
        state.selectRequested = row.path;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            state.frameRequested = row.path;
        }
    }
    if (ImGui::IsItemHovered() && !row.findings.empty()) {
        std::string text;
        for (const auto& finding : row.findings) {
            text += "\n" + finding;
        }
        ImGui::SetTooltip("Authoring findings:%s", text.c_str());
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(streamingStateName(row.rootState));
    ImGui::TableSetColumnIndex(2);
    ImGui::TextUnformatted(levelName(row, row.selected).c_str());
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(levelName(row, row.drawn).c_str());
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%s", row.state >= 0 ? std::format("{}", row.state).c_str() : "-");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The finest level that must be resident: %s", row.residentWhy.c_str());
    }
    ImGui::TableSetColumnIndex(5);
    ImGui::TextUnformatted(residentText(row).c_str());
    ImGui::TableSetColumnIndex(6);
    if (row.measure >= 0.0f) {
        ImGui::Text(row.kind == "screen" ? "%.3f" : "%.0f m", row.measure);
    } else {
        ImGui::TextDisabled("-");
    }
    ImGui::TableSetColumnIndex(7);
    if (!row.load.empty()) {
        ImGui::Text("%s / %s", listText(row.load).c_str(), row.unload.empty() ? "-" : listText(row.unload).c_str());
    } else {
        ImGui::TextDisabled("no streaming heuristic");
    }
    ImGui::TableSetColumnIndex(8);
    ImGui::Text("%u", row.reloads);
    ImGui::PopID();
}

// The selected root: its heuristics by domain, the streaming radii (editable), its levels, its state and why, findings.
auto drawInspector(StreamingWindowState& state) -> void {
    if (!state.inspection) {
        ImGui::TextDisabled("Select a streaming root (in the list, the map, the viewport or a label) to inspect it.");
        return;
    }
    const auto& in = *state.inspection;
    const auto& row = in.row;
    if (state.editedRoot != row.path) {
        state.editedRoot = row.path;
        state.loadText = {};
        state.unloadText = {};
        std::format_to_n(state.loadText.data(), state.loadText.size() - 1, "{}", listText(row.load));
        std::format_to_n(state.unloadText.data(), state.unloadText.size() - 1, "{}", listText(row.unload));
    }
    stateSwatch(row.rootState, "##inspected");
    ImGui::SameLine();
    ImGui::TextUnformatted(row.path.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s, authored as %s", streamingStateName(row.rootState), row.key.c_str());
    ImGui::Text("Selects %s (%s), draws %s", levelName(row, row.selected).c_str(), row.why.c_str(), levelName(row, row.drawn).c_str());
    ImGui::Text("State %d: %s", row.state, in.stateWhy.c_str());
    ImGui::Text("Resident: %s (%s)", residentText(row).c_str(), row.residentWhy.c_str());
    if (!in.parent.empty()) {
        ImGui::TextDisabled("Inside %s", in.parent.c_str());
    }
    if (!in.children.empty()) {
        ImGui::TextDisabled("%zu streaming roots placed in it", in.children.size());
    }
    for (const auto& finding : row.findings) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "Finding: %s", finding.c_str());
    }

    // The streaming radii, editable: every placement of this authored root takes them.
    if (row.streamingHeuristic >= 0) {
        bool distance = row.kind == "distance";
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Load");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        bool loadEdited = ImGui::InputText("##load", state.loadText.data(), state.loadText.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Load radii, finest boundary first, %s: the camera this close makes the finer level resident.", distance ? "metres, ascending" : "screen sizes, descending");
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Unload");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        bool unloadEdited = ImGui::InputText("##unload", state.unloadText.data(), state.unloadText.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Unload radii, one per load radius, each at or past it: the level stays resident until the camera is this far. Empty: the load radii.");
        }
        if (loadEdited || unloadEdited) {
            auto load = parseList(state.loadText.data());
            auto unload = parseList(state.unloadText.data());
            if (load && unload) {
                StreamingWindowState::RadiiRequest request{.root = row.path, .heuristic = (uint32_t) row.streamingHeuristic};
                if (loadEdited) {
                    request.load = *load;
                }
                if (unloadEdited) {
                    request.unload = *unload;
                }
                state.radiiRequests.push_back(std::move(request));
                state.editedRoot.clear();
            }
        }
        if (row.tuned) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                state.radiiResets.push_back(row.path);
                state.editedRoot.clear();
            }
        }
    }

    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##heuristics", 5, flags)) {
        ImGui::TableSetupColumn("Heuristic", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Thresholds in force", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Unload in force", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Authored", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& heuristic : in.heuristics) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            auto name = std::format("{} {}", heuristic.domain, heuristic.kind);
            if (heuristic.used) {
                ImGui::Text("%s (used)", name.c_str());
            } else {
                ImGui::TextDisabled("%s", name.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s", heuristic.path.c_str(), heuristic.why.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            if (heuristic.measure >= 0.0f) {
                ImGui::Text(heuristic.kind == "screen" ? "%.3f" : "%.0f", heuristic.measure);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(listText(heuristic.thresholds).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(listText(heuristic.unload).c_str());
            ImGui::TableSetColumnIndex(4);
            auto authored = listText(heuristic.authored);
            if (!heuristic.authoredUnload.empty()) {
                authored += " / " + listText(heuristic.authoredUnload);
            }
            ImGui::TextDisabled("%s", authored.c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::BeginTable("##levels", 6, flags)) {
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Requested", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Loaded", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Ready", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Drawn", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Lead", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();
        for (size_t level = 0; level < row.levels.size(); level++) {
            const auto& info = row.levels[level];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%zu  %s", level, info.name.c_str());
            int column = 1;
            for (bool on : {info.requested, info.loaded, info.ready, info.drawn}) {
                ImGui::TableSetColumnIndex(column++);
                if (on) {
                    ImGui::TextUnformatted("yes");
                } else {
                    ImGui::TextDisabled("-");
                }
            }
            ImGui::TableSetColumnIndex(5);
            if (info.lead >= 0.0) {
                ImGui::Text("%.1f s", info.lead);
            } else {
                ImGui::TextDisabled("-");
            }
        }
        ImGui::EndTable();
    }
}

// The plane the map looks at: two axes across the up axis, x to the right and y up the map.
auto mapAxes(const glm::vec3& up) -> std::pair<glm::vec3, glm::vec3> {
    if (std::abs(up.z) > 0.5f) {
        return {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)};
    }
    return {glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
}

// The map: the roots' bounds from above in their state's colour, finer roots over coarser ones, the camera and where it looks. Hover shows a
// root's state and its next boundary's load (solid) and unload (faint) rings around its bounds; click selects, double-click frames; drag pans,
// the wheel zooms.
auto drawMap(StreamingWindowState& state) -> void {
    ImGui::Checkbox("Follow the camera", &state.mapFollow);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("m per pixel", &state.mapMetresPerPixel, 0.25f, 40.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    auto [ax, ay] = mapAxes(state.up);
    glm::vec2 camera(glm::dot(state.cameraPosition, ax), glm::dot(state.cameraPosition, ay));
    if (state.mapFollow) {
        state.mapCenter = camera;
    }
    auto size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 100.0f);
    size.y = std::max(size.y, 100.0f);
    auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##map", size);
    bool hovered = ImGui::IsItemHovered();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(18, 20, 22, 255));
    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    float scale = 1.0f / state.mapMetresPerPixel;
    auto toScreen = [&](glm::vec2 world) {
        return ImVec2(origin.x + (size.x * 0.5f) + ((world.x - state.mapCenter.x) * scale), origin.y + (size.y * 0.5f) - ((world.y - state.mapCenter.y) * scale));
    };
    auto toWorld = [&](ImVec2 screen) {
        return glm::vec2(state.mapCenter.x + ((screen.x - origin.x - (size.x * 0.5f)) / scale), state.mapCenter.y - ((screen.y - origin.y - (size.y * 0.5f)) / scale));
    };
    // Coarser roots first (shorter paths), so a tile draws over its region.
    std::vector<const StreamingWindowRow*> rows;
    for (const auto& row : state.rows) {
        if (row.hasBounds && streamingRowShown(state, row)) {
            rows.push_back(&row);
        }
    }
    std::ranges::stable_sort(rows, {}, [](const StreamingWindowRow* row) { return row->path.size(); });
    auto mouse = ImGui::GetIO().MousePos;
    auto mouseWorld = toWorld(mouse);
    const StreamingWindowRow* under = nullptr;
    for (const auto* row : rows) {
        glm::vec2 lo(glm::dot(row->boundsMin, ax), glm::dot(row->boundsMin, ay));
        glm::vec2 hi(glm::dot(row->boundsMax, ax), glm::dot(row->boundsMax, ay));
        auto a = toScreen(glm::min(lo, hi));
        auto b = toScreen(glm::max(lo, hi));
        ImVec2 min(std::min(a.x, b.x), std::min(a.y, b.y));
        ImVec2 max(std::max(a.x, b.x), std::max(a.y, b.y));
        auto color = streamingStateColor(row->rootState);
        draw->AddRectFilled(min, max, toColor(glm::vec4(glm::vec3(color) * 0.55f, 0.85f)));
        bool selected = row->path == state.selectedRoot;
        draw->AddRect(min, max, selected ? IM_COL32(255, 255, 255, 255) : (!row->findings.empty() ? IM_COL32(255, 220, 60, 255) : IM_COL32(0, 0, 0, 140)), 0.0f, 0, selected ? 2.0f : 1.0f);
        if (mouseWorld.x >= std::min(lo.x, hi.x) && mouseWorld.x <= std::max(lo.x, hi.x) && mouseWorld.y >= std::min(lo.y, hi.y) && mouseWorld.y <= std::max(lo.y, hi.y)) {
            under = row;
        }
    }
    // The hovered root's next boundary: where the camera makes the finer level resident (load) and lets it go (unload), measured to its bounds.
    if (hovered && under != nullptr && under->kind == "distance" && under->state >= 0) {
        auto boundary = (size_t) std::max(0, under->state - 1);
        if (boundary < under->load.size()) {
            glm::vec2 lo(glm::dot(under->boundsMin, ax), glm::dot(under->boundsMin, ay));
            glm::vec2 hi(glm::dot(under->boundsMax, ax), glm::dot(under->boundsMax, ay));
            auto ring = [&](float radius, ImU32 color) {
                auto a = toScreen(glm::min(lo, hi) - glm::vec2(radius));
                auto b = toScreen(glm::max(lo, hi) + glm::vec2(radius));
                draw->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), color, radius * scale, 0, 1.5f);
            };
            ring(under->load[boundary], IM_COL32(255, 190, 80, 255));
            if (boundary < under->unload.size()) {
                ring(under->unload[boundary], IM_COL32(255, 190, 80, 110));
            }
        }
    }
    // The camera and where it looks.
    auto at = toScreen(camera);
    glm::vec2 look(glm::dot(state.cameraForward, ax), glm::dot(state.cameraForward, ay));
    if (glm::length(look) > 1e-4f) {
        look = glm::normalize(look);
        draw->AddLine(at, ImVec2(at.x + (look.x * 18.0f), at.y - (look.y * 18.0f)), IM_COL32(255, 255, 255, 255), 2.0f);
    }
    draw->AddCircleFilled(at, 4.0f, IM_COL32(255, 255, 255, 255));
    // The debug camera, hollow, where it looks from.
    if (state.debugCameraPosition) {
        glm::vec2 debugAt(glm::dot(*state.debugCameraPosition, ax), glm::dot(*state.debugCameraPosition, ay));
        draw->AddCircle(toScreen(debugAt), 5.0f, IM_COL32(255, 140, 25, 255), 0, 2.0f);
    }
    // A scale bar.
    float bar = std::pow(10.0f, std::floor(std::log10(state.mapMetresPerPixel * 120.0f)));
    ImVec2 barAt(origin.x + 10.0f, origin.y + size.y - 14.0f);
    draw->AddLine(barAt, ImVec2(barAt.x + (bar * scale), barAt.y), IM_COL32(220, 220, 220, 255), 2.0f);
    draw->AddText(ImVec2(barAt.x, barAt.y - 16.0f), IM_COL32(220, 220, 220, 255), std::format("{:g} m", bar).c_str());
    draw->PopClipRect();

    if (hovered) {
        auto& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            state.mapMetresPerPixel = std::clamp(state.mapMetresPerPixel * std::pow(0.85f, io.MouseWheel), 0.25f, 40.0f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            state.mapFollow = false;
            state.mapCenter -= glm::vec2(io.MouseDelta.x, -io.MouseDelta.y) / scale;
        }
        if (under != nullptr) {
            ImGui::SetTooltip("%s\n%s: selects %s, draws %s, state %d, resident %s", under->path.c_str(), streamingStateName(under->rootState), levelName(*under, under->selected).c_str(), levelName(*under, under->drawn).c_str(), under->state, residentText(*under).c_str());
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x == 0.0f) {
                state.selectRequested = under->path;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                state.frameRequested = under->path;
            }
        }
    }
}

auto drawRoots(StreamingWindowState& state) -> void {
    std::vector<const StreamingWindowRow*> rows;
    rows.reserve(state.rows.size());
    for (const auto& row : state.rows) {
        if (streamingRowShown(state, row)) {
            rows.push_back(&row);
        }
    }
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##filter", "filter by path", state.filter, sizeof(state.filter));
    ImGui::SameLine();
    ImGui::Checkbox("late", &state.onlyLate);
    ImGui::SameLine();
    ImGui::Checkbox("holes", &state.onlyHoles);
    ImGui::SameLine();
    ImGui::Checkbox("prefetching", &state.onlyPrefetching);
    ImGui::SameLine();
    ImGui::Checkbox("findings", &state.onlyFindings);
    ImGui::SameLine();
    ImGui::Checkbox("under selection", &state.onlyUnderSelection);
    ImGui::SameLine();
    ImGui::Text("%zu of %zu roots", rows.size(), state.rows.size());

    int scrollTo = -1;
    if (state.selectedRoot != state.scrolledTo) {
        state.scrolledTo = state.selectedRoot;
        for (size_t i = 0; i < rows.size(); i++) {
            if (rows[i]->path == state.selectedRoot) {
                scrollTo = (int) i;
            }
        }
    }
    float listHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.5f);
    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##roots", 9, flags, ImVec2(0.0f, listHeight))) {
        ImGui::TableSetupColumn("Root", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Selected", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Drawn", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Finest", ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Resident", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Measure", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Load / unload", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Reloads", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        if (scrollTo >= 0) {
            ImGui::SetScrollY((float) scrollTo * ImGui::GetFrameHeightWithSpacing());
        }
        ImGuiListClipper clipper;
        clipper.Begin((int) rows.size(), ImGui::GetFrameHeightWithSpacing());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                drawRow(*rows[(size_t) i], state);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (ImGui::BeginChild("##inspector")) {
        drawInspector(state);
    }
    ImGui::EndChild();
}

} // namespace

auto streamingStateColor(StreamingRootState state) -> glm::vec4 {
    switch (state) {
        case StreamingRootState::Drawn:
            return {0.30f, 0.75f, 0.35f, 1.0f};
        case StreamingRootState::Prefetched:
            return {0.20f, 0.70f, 0.70f, 1.0f};
        case StreamingRootState::Requested:
            return {0.30f, 0.45f, 0.95f, 1.0f};
        case StreamingRootState::Refused:
            return {0.60f, 0.40f, 0.85f, 1.0f};
        case StreamingRootState::Late:
            return {0.95f, 0.60f, 0.15f, 1.0f};
        case StreamingRootState::Hole:
            return {0.90f, 0.20f, 0.20f, 1.0f};
        case StreamingRootState::None:
            break;
    }
    return {0.35f, 0.35f, 0.35f, 1.0f};
}

auto streamingStateName(StreamingRootState state) -> const char* {
    switch (state) {
        case StreamingRootState::Drawn:
            return "drawn";
        case StreamingRootState::Prefetched:
            return "prefetched";
        case StreamingRootState::Requested:
            return "requested";
        case StreamingRootState::Refused:
            return "refused";
        case StreamingRootState::Late:
            return "late";
        case StreamingRootState::Hole:
            return "hole";
        case StreamingRootState::None:
            break;
    }
    return "no root";
}

auto streamingRowShown(const StreamingWindowState& state, const StreamingWindowRow& row) -> bool {
    if (state.filter[0] != '\0' && !row.path.contains(state.filter)) {
        return false;
    }
    if (state.onlyLate && !row.late) {
        return false;
    }
    if (state.onlyHoles && !row.hole) {
        return false;
    }
    if (state.onlyPrefetching && row.rootState != StreamingRootState::Prefetched) {
        return false;
    }
    if (state.onlyFindings && row.findings.empty()) {
        return false;
    }
    if (state.onlyUnderSelection && !underPath(row.path, state.selectedPath)) {
        return false;
    }
    return true;
}

auto drawStreamingWindow(bool& show, StreamingWindowState& state) -> void {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Streaming", &show)) {
        ImGui::End();
        return;
    }
    drawOverview(state);
    drawControls(state);
    ImGui::Separator();
    if (ImGui::BeginTabBar("##streamingtabs")) {
        if (ImGui::BeginTabItem("Roots")) {
            drawRoots(state);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Map")) {
            drawMap(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

auto drawStreamingLabels(StreamingWindowState& state, const glm::mat4& viewProj, glm::vec2 viewport) -> void {
    if (!state.labels) {
        return;
    }
    // Under the windows, so a label never covers a window.
    auto* drawList = ImGui::GetBackgroundDrawList();
    const auto& io = ImGui::GetIO();
    constexpr size_t maxLabels = 400;
    size_t drawnLabels = 0;
    for (const auto& row : state.rows) {
        if (drawnLabels >= maxLabels) {
            break;
        }
        if (!row.hasBounds || !streamingRowShown(state, row)) {
            continue;
        }
        auto clip = viewProj * glm::vec4((row.boundsMin + row.boundsMax) * 0.5f, 1.0f);
        if (clip.w <= 1e-4f) {
            continue;
        }
        auto ndc = glm::vec2(clip) / clip.w;
        if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f) {
            continue;
        }
        // Vulkan's clip space has y down.
        ImVec2 at((ndc.x * 0.5f + 0.5f) * viewport.x, (ndc.y * 0.5f + 0.5f) * viewport.y);
        auto text = std::format("{}  {}/{}  s{}  {}", row.name, row.drawn >= 0 ? std::format("{}", row.drawn) : std::string("-"), row.selected, row.state, streamingStateName(row.rootState));
        auto size = ImGui::CalcTextSize(text.c_str());
        ImVec2 min(at.x - (size.x * 0.5f) - 4.0f, at.y - (size.y * 0.5f) - 2.0f);
        ImVec2 max(at.x + (size.x * 0.5f) + 4.0f, at.y + (size.y * 0.5f) + 2.0f);
        bool hovered = io.MousePos.x >= min.x && io.MousePos.x <= max.x && io.MousePos.y >= min.y && io.MousePos.y <= max.y;
        bool selected = row.path == state.selectedRoot;
        drawList->AddRectFilled(min, max, hovered || selected ? IM_COL32(40, 40, 40, 230) : IM_COL32(0, 0, 0, 160), 3.0f);
        drawList->AddRectFilled(ImVec2(min.x, max.y - 2.0f), max, toColor(streamingStateColor(row.rootState)));
        drawList->AddText(ImVec2(min.x + 4.0f, min.y + 2.0f), IM_COL32(235, 235, 235, 255), text.c_str());
        // The viewport's click picks what is under the cursor: over a label, ImGui takes the click instead, and the label selects its root.
        if (hovered) {
            ImGui::SetNextFrameWantCaptureMouse(true);
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
            state.selectRequested = row.path;
        }
        drawnLabels++;
    }
}
