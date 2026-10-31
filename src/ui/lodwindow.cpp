#include "lodwindow.h"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>

namespace {

enum Column : int {
    ColumnRoot,
    ColumnKind,
    ColumnLevel,
    ColumnDrawn,
    ColumnLoaded,
    ColumnScreen,
    ColumnWhy,
};

const ImVec4 warnColor = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);

auto toImVec4(const glm::vec4& color) -> ImVec4 {
    return ImVec4(color.r, color.g, color.b, color.a);
}

auto levelName(const std::vector<std::string>& levels, uint32_t level) -> std::string {
    if (level < levels.size()) {
        return std::format("{} ({})", level, levels[level]);
    }
    return std::format("{}", level);
}

auto loadedText(const LodWindowRow& row) -> std::string {
    if (row.group >= 0) {
        return "all";
    }
    std::string text;
    for (uint32_t level : row.loaded) {
        if (!text.empty()) {
            text += ", ";
        }
        text += std::format("{}", level);
    }
    return text;
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

// A colour swatch for a level, as the LOD Level view colours it.
auto levelSwatch(uint32_t level, uint32_t depth, bool byDepth) -> void {
    ImGui::ColorButton(std::format("##level{}.{}", depth, level).c_str(), toImVec4(lodLevelColor(level, depth, byDepth)), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(12.0f, 12.0f));
}

// The level picker: the root's default (or its heuristic's), or one of its levels. A choice asks the application for it.
auto levelCombo(const std::string& path, const std::vector<std::string>& levels, uint32_t defaultIndex, bool setByHand, uint32_t wanted, bool heuristic, LodWindowState& state) -> void {
    auto unset = heuristic ? std::string("heuristic") : std::format("default: {}", levelName(levels, defaultIndex));
    auto preview = setByHand ? levelName(levels, wanted) : unset;
    if (!ImGui::BeginCombo("##level", preview.c_str())) {
        return;
    }
    if (ImGui::Selectable(unset.c_str(), !setByHand)) {
        state.levelRequests.push_back({.root = path, .level = std::nullopt});
    }
    for (uint32_t level = 0; level < levels.size(); level++) {
        if (ImGui::Selectable(levelName(levels, level).c_str(), setByHand && wanted == level)) {
            state.levelRequests.push_back({.root = path, .level = level});
        }
    }
    ImGui::EndCombo();
}

auto drawnText(const LodWindowRow& row) -> std::string {
    if (row.drawn < 0) {
        return row.group >= 0 ? "-" : "loading";
    }
    auto text = levelName(row.levels, (uint32_t) row.drawn);
    if (row.fadeLevel >= 0) {
        text += std::format(" → {} ({:.0f}%)", row.fadeLevel, row.fade * 100.0f);
    }
    return text;
}

// One row: the root (click selects, double-click frames), its kind, its level picker, the level drawn and the levels loaded, the measure, why.
auto drawRow(const LodWindowRow& row, LodWindowState& state) -> void {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeightWithSpacing());
    ImGui::PushID(row.path.c_str());

    ImGui::TableSetColumnIndex(ColumnRoot);
    ImGui::AlignTextToFramePadding();
    bool selected = row.path == state.selectedRoot;
    auto selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick;
    auto label = row.overridden ? row.path + "  (tuned)" : row.path;
    if (ImGui::Selectable(label.c_str(), selected, selectableFlags, ImVec2(0.0f, ImGui::GetFrameHeight()))) {
        state.selectRequested = row.path;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            state.frameRequested = row.path;
        }
    }
    if (ImGui::IsItemHovered()) {
        std::string levels;
        for (size_t level = 0; level < row.levels.size(); level++) {
            bool loaded = row.group >= 0 || std::ranges::find(row.loaded, (uint32_t) level) != row.loaded.end();
            levels += std::format("\n  {}{}", levelName(row.levels, (uint32_t) level), loaded ? ", loaded" : "");
            if (level < row.levelPayloads.size()) {
                for (const auto& asset : row.levelPayloads[level]) {
                    levels += std::format("\n      payload {}", asset);
                }
            }
        }
        auto heuristic = row.heuristic.empty() ? std::string("none (default level only)") : row.heuristic;
        ImGui::SetTooltip("%s\nheuristic: %s%s\nlevels, finest first:%s", row.path.c_str(), heuristic.c_str(), row.overridden ? "\nthresholds tuned in this view" : "", levels.c_str());
    }

    ImGui::TableSetColumnIndex(ColumnKind);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(row.group >= 0 ? "render" : "stream");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(row.group >= 0 ? "Render LOD: every level held, the GPU picks one per placement each frame and cross-fades inside a blend band."
                                         : "Streaming: each level a payload, loaded when wanted; the CPU picks one per root.");
    }

    ImGui::TableSetColumnIndex(ColumnLevel);
    ImGui::SetNextItemWidth(-FLT_MIN);
    levelCombo(row.path, row.levels, row.defaultIndex, row.setByHand, row.wanted, state.heuristic && !row.heuristic.empty(), state);

    ImGui::TableSetColumnIndex(ColumnDrawn);
    ImGui::AlignTextToFramePadding();
    if (row.drawn >= 0) {
        levelSwatch((uint32_t) row.drawn, row.depth, state.colorByDepth);
        ImGui::SameLine();
    }
    if (row.drawn >= 0 && row.drawn != (int32_t) row.wanted) {
        ImGui::TextColored(warnColor, "%s, %s", levelName(row.levels, (uint32_t) row.drawn).c_str(), row.group >= 0 ? "falling back" : "loading");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Draws %s until %s is %s.\n%s",
                              levelName(row.levels, (uint32_t) row.drawn).c_str(),
                              levelName(row.levels, row.wanted).c_str(),
                              row.group >= 0 ? "on the GPU" : "complete",
                              row.waiting.empty() ? "Its level isn't loaded yet." : row.waiting.c_str());
        }
    } else if (row.drawn < 0 && row.group < 0) {
        ImGui::TextColored(warnColor, "loading");
    } else {
        ImGui::TextUnformatted(drawnText(row).c_str());
    }
    if (row.flashing) {
        ImGui::SameLine();
        ImGui::TextColored(warnColor, "pop");
    }

    ImGui::TableSetColumnIndex(ColumnLoaded);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(loadedText(row).c_str());
    if (row.state && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Resident: %s and every coarser level (%s).", levelName(row.levels, *row.state).c_str(), row.residentWhy.c_str());
    }

    ImGui::TableSetColumnIndex(ColumnScreen);
    ImGui::AlignTextToFramePadding();
    if (row.measure >= 0.0f) {
        if (row.measureUnit.empty()) {
            ImGui::Text("%.3f%%", row.measure * 100.0f);
        } else {
            ImGui::Text("%.1f %s", row.measure, row.measureUnit.c_str());
        }
    } else {
        ImGui::TextDisabled("-");
    }
    ImGui::TableSetColumnIndex(ColumnWhy);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(row.why.c_str());
    ImGui::PopID();
}

// "Set shown": every shown row to its default, its coarsest level or one level; a root with fewer levels takes its coarsest.
auto drawSetShown(const std::vector<const LodWindowRow*>& rows, size_t mostLevels, LodWindowState& state) -> void {
    std::vector<std::string> choices = {"default", "coarsest"};
    for (size_t level = 0; level < mostLevels; level++) {
        choices.push_back(std::format("level {}", level));
    }
    state.allChoice = std::clamp(state.allChoice, 0, (int) choices.size() - 1);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##allChoice", choices[(size_t) state.allChoice].c_str())) {
        for (size_t i = 0; i < choices.size(); i++) {
            if (ImGui::Selectable(choices[i].c_str(), state.allChoice == (int) i)) {
                state.allChoice = (int) i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(rows.empty());
    if (ImGui::Button(std::format("Set {} shown###setshown", rows.size()).c_str())) {
        for (const auto* row : rows) {
            if (state.allChoice == 0) {
                state.levelRequests.push_back({.root = row->path, .level = std::nullopt});
            } else if (state.allChoice == 1) {
                state.levelRequests.push_back({.root = row->path, .level = row->levels.empty() ? 0u : (uint32_t) row->levels.size() - 1});
            } else {
                state.levelRequests.push_back({.root = row->path, .level = (uint32_t) (state.allChoice - 2)});
            }
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Sets every root the filter shows (\"default\": back to the heuristic). Finer streaming levels load what they need:\nnarrow the filter first on a large scene.");
    }
}

// The global controls: the heuristic, the preference, the unload delay; the tuning.
auto drawControls(LodWindowState& state) -> void {
    bool heuristic = state.heuristic;
    if (ImGui::Checkbox("Heuristic", &heuristic)) {
        state.heuristicRequested = heuristic;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: every root not set by hand wants the level its heuristic picks from the camera: inside an authored blend band it\ncross-fades to the next (render LOD); without one, going coarser waits for the hysteresis margin.\nOff: every root wants its default.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    int preference = state.preference;
    if (ImGui::Combo("##prefer", &preference, "distance first\0screen size first\0authored order\0")) {
        state.preferenceRequested = preference;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Which heuristic a root naming several measures by. Distance: metres (the stage's units) to its center or bounding box.\nScreen size: the fraction of the view its extent's bounding sphere covers. Authored: the first it names.");
    }

    ImGui::SetNextItemWidth(70.0f);
    float bias = state.bias;
    if (ImGui::InputFloat("Bias", &bias, 0.0f, 0.0f, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue) && bias > 0.0f) {
        state.biasRequested = bias;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Every root picks its level as if it were this many times closer: 2 is finer everywhere, 0.5 coarser. 1 is as authored.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    float hysteresis = state.hysteresis * 100.0f;
    if (ImGui::InputFloat("Hysteresis (%)", &hysteresis, 0.0f, 0.0f, "%.0f", ImGuiInputTextFlags_EnterReturnsTrue) && hysteresis >= 0.0f) {
        state.hysteresisRequested = hysteresis / 100.0f;
    }
    if (ImGui::IsItemHovered()) {
        float farther = (1.0f / std::sqrt(std::max(0.1f, 1.0f - state.hysteresis)) - 1.0f) * 100.0f;
        ImGui::SetTooltip("Where a boundary has no blend band, a root goes coarser only once its screen size is this much below the threshold\n(now: about %.0f%% farther than a distance threshold), so a camera resting on a boundary doesn't flip levels each frame.", farther);
    }
    ImGui::SameLine();
    bool blending = state.blending;
    if (ImGui::Checkbox("Blending", &blending)) {
        state.blendingRequested = blending;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Off drops every blend band: render-LOD roots switch levels without a fade (and pop), with the hysteresis everywhere.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(state.overrides == 0);
    if (ImGui::Button(std::format("Reset {} tuned roots###resettuned", state.overrides).c_str())) {
        state.overrideResets.push_back({});
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Drops every threshold set in this view: each root picks with its authored thresholds again. Tuning is never saved.");
    }
}

// The viewport's layers, with the LOD Level view's legend.
auto drawLayers(LodWindowState& state) -> void {
    bool levelView = state.levelView;
    if (ImGui::Checkbox("LOD Level view", &levelView)) {
        state.levelViewRequested = levelView;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Over the lit image, at half strength: each mesh in its drawn level's colour, or in its LOD depth's (grey under no LOD root).\nA fade shows its two colours dithered together. Render-LOD roots at a forced level are striped, those drawing another level than they want\ncheckered.");
    }
    ImGui::SameLine();
    int colorBy = state.colorByDepth ? 1 : 0;
    ImGui::RadioButton("by level", &colorBy, 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The drawn level of the nearest LOD root. An index within that root: a region's level 1 and a tile's level 1 are different\nresolutions (on the streaming test world a region's medium and a tile's simplified are both 5 m terrain).");
    }
    ImGui::SameLine();
    ImGui::RadioButton("by depth", &colorBy, 1);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How many LOD roots deep the drawn level is: under a top-level root (a region) 1, a root in its level (a tile) 2, one\ndeeper (a ground chunk, an asset in a tile) 3.");
    }
    state.colorByDepth = colorBy == 1;
    ImGui::SameLine();
    ImGui::Checkbox("Labels", &state.labels);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Over each root the list shows: its name, drawn level (→ the level fading in and its share) and measure. Click one to select it.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Gizmo", &state.gizmo);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The selected root's heuristic in the viewport. Distance: its center or box, a ring at each threshold distance in the\ncolour of the level beyond it, a faint ring where each blend band ends. Screen size: the sphere it measures.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("of every shown root", &state.gizmoAll);
    ImGui::SameLine();
    ImGui::Checkbox("Pop flash", &state.flash);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Over any view, the lit one too: a render-LOD root flashes white for half a second when its drawn level changes\nwithout a fade (no band, a forced level, a fallback). Each one is a visible pop.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Triangle Size view")) {
        state.triangleSizeRequested = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The debug view of each triangle's size on screen in pixels: whether a level is too dense for how large it is drawn.");
    }

    if (state.colorByDepth) {
        ImGui::TextDisabled("Depth:");
        static constexpr std::array<const char*, 4> depthNames = {"1", "2", "3", "4+"};
        for (uint32_t depth = 1; depth <= 4; depth++) {
            ImGui::SameLine();
            levelSwatch(0, depth, true);
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextUnformatted(depthNames[depth - 1]);
        }
    } else {
        ImGui::TextDisabled("Levels:");
        for (uint32_t level = 0; level < 8; level++) {
            ImGui::SameLine();
            levelSwatch(level, 1, false);
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::Text("%u", level);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  grey: no LOD root; stripes: forced; checker: falling back");
}

// The selected root: its state, its heuristics (thresholds editable), its levels.
auto drawInspector(LodWindowState& state) -> void {
    if (!state.inspection) {
        ImGui::TextDisabled("Select a LOD root (in the list, the viewport or a label) to inspect it.");
        return;
    }
    const auto& in = *state.inspection;
    if (state.editedRoot != in.path || state.thresholdText.size() != in.heuristics.size()) {
        state.editedRoot = in.path;
        state.thresholdText.assign(in.heuristics.size(), {});
        state.blendText.assign(in.heuristics.size(), {});
        for (size_t h = 0; h < in.heuristics.size(); h++) {
            std::format_to_n(state.thresholdText[h].data(), state.thresholdText[h].size() - 1, "{}", listText(in.heuristics[h].thresholds));
            std::format_to_n(state.blendText[h].data(), state.blendText[h].size() - 1, "{}", listText(in.heuristics[h].blendThresholds));
        }
    }
    ImGui::TextUnformatted(in.path.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s, authored as %s", in.renderLod ? "render LOD" : "streaming", in.key.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Threshold edits apply to every placement of this authored root: every instance of an instanceable asset shares one.");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Wants %s, draws %s", in.wanted >= 0 ? std::format("{}", in.wanted).c_str() : "-", in.drawn >= 0 ? std::format("{}", in.drawn).c_str() : "-");
    if (in.fadeLevel >= 0) {
        ImGui::SameLine();
        ImGui::Text("fading in %d (%.0f%%)", in.fadeLevel, in.fade * 100.0f);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", in.why.c_str());
    ImGui::SameLine();
    std::vector<std::string> levelNames;
    for (const auto& level : in.levels) {
        levelNames.push_back(level.name);
    }
    ImGui::SetNextItemWidth(170.0f);
    ImGui::PushID("inspector");
    levelCombo(in.path, levelNames, in.defaultIndex, in.setByHand, in.wanted >= 0 ? (uint32_t) in.wanted : 0, state.heuristic, state);
    ImGui::PopID();

    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##heuristics", 6, flags)) {
        ImGui::TableSetupColumn("Heuristic", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Picks", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Thresholds", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Blend thresholds", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableHeadersRow();
        for (size_t h = 0; h < in.heuristics.size(); h++) {
            const auto& heuristic = in.heuristics[h];
            bool distance = heuristic.kind == "distance";
            ImGui::PushID((int) h);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (heuristic.used) {
                ImGui::Text("%s (used)", heuristic.kind.c_str());
            } else {
                ImGui::TextDisabled("%s", heuristic.kind.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s", heuristic.path.c_str(), heuristic.why.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            if (heuristic.measure < 0.0f) {
                ImGui::TextDisabled("-");
            } else if (distance) {
                ImGui::Text("%.1f", heuristic.measure);
            } else {
                ImGui::Text("%.3f%%", heuristic.measure * 100.0f);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(distance ? "The distance from the camera, in the stage's units (as the thresholds are authored)." : "The fraction of the view the extent's bounding sphere covers.");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            if (heuristic.picks >= 0) {
                levelSwatch((uint32_t) heuristic.picks, in.depth, false);
                ImGui::SameLine();
                if (heuristic.blend > 0.0f) {
                    ImGui::Text("%d+%.0f%%", heuristic.picks, heuristic.blend * 100.0f);
                } else {
                    ImGui::Text("%d", heuristic.picks);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("The level this heuristic would want from the camera now, under the tuning (bias, blending), coming from the finest level.");
                }
            } else {
                ImGui::TextDisabled("-");
            }
            for (int column = 3; column <= 4; column++) {
                bool blends = column == 4;
                ImGui::TableSetColumnIndex(column);
                auto& text = blends ? state.blendText[h] : state.thresholdText[h];
                ImGui::SetNextItemWidth(-FLT_MIN);
                bool edited = ImGui::InputText(blends ? "##blends" : "##thresholds", text.data(), text.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                if (ImGui::IsItemHovered()) {
                    const auto& authored = blends ? heuristic.authoredBlends : heuristic.authored;
                    ImGui::SetTooltip("Authored: %s\n%s, finest first, %s. Enter applies to every placement of this authored root.",
                                      authored.empty() ? "none" : listText(authored).c_str(),
                                      blends ? "Where each band ends, past its threshold" : "Where each level hands over to the next",
                                      distance ? "ascending, in the stage's units" : "descending, as a fraction of the view");
                }
                if (edited) {
                    if (auto values = parseList(text.data())) {
                        LodWindowState::OverrideRequest request{.root = in.path, .heuristic = (uint32_t) h};
                        if (blends) {
                            request.blendThresholds = *values;
                        } else {
                            request.thresholds = *values;
                        }
                        state.overrideRequests.push_back(std::move(request));
                        state.editedRoot.clear();
                    }
                }
            }
            ImGui::TableSetColumnIndex(5);
            if (heuristic.overridden) {
                if (ImGui::SmallButton("Reset")) {
                    state.overrideResets.push_back(in.path);
                    state.editedRoot.clear();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Back to the authored thresholds for every heuristic of this authored root.");
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (ImGui::BeginTable("##levels", 5, flags)) {
        ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("vs finest", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();
        uint64_t finest = !in.levels.empty() && in.levels[0].known ? in.levels[0].triangles : 0;
        for (size_t level = 0; level < in.levels.size(); level++) {
            const auto& info = in.levels[level];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            levelSwatch((uint32_t) level, in.depth, false);
            ImGui::SameLine();
            ImGui::Text("%zu  %s", level, info.name.c_str());
            ImGui::TableSetColumnIndex(1);
            if (info.known) {
                ImGui::Text("%llu", (unsigned long long) info.triangles);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableSetColumnIndex(2);
            if (info.known) {
                ImGui::Text("%u", info.draws);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableSetColumnIndex(3);
            if (info.known && finest > 0) {
                ImGui::Text("%.1f%%", 100.0 * (double) info.triangles / (double) finest);
            }
            ImGui::TableSetColumnIndex(4);
            if ((int32_t) level == in.drawn) {
                ImGui::TextUnformatted("drawn");
            } else if ((int32_t) level == in.fadeLevel) {
                ImGui::TextUnformatted("fading in");
            } else if (!in.renderLod && info.loaded) {
                ImGui::TextDisabled("loaded");
            }
        }
        ImGui::EndTable();
    }
    if (!in.renderLod) {
        ImGui::TextDisabled("A streaming root's triangles are counted for its drawn level only: the others aren't placed.");
    }
}

} // namespace

auto LodShape::anchor() const -> glm::vec3 {
    if (kind == Kind::Distance) {
        auto local = hasBox ? (boxMin + boxMax) * 0.5f : point;
        return glm::vec3(toWorld * glm::vec4(local, 1.0f));
    }
    if (kind == Kind::ScreenSize) {
        return center;
    }
    return glm::vec3(toWorld[3]);
}

auto lodLevelColor(uint32_t level, uint32_t depth, bool byDepth) -> glm::vec4 {
    static constexpr std::array<glm::vec4, 8> levelRamp = {
        glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
        glm::vec4(1.0f, 0.5f, 0.0f, 1.0f),
        glm::vec4(1.0f, 1.0f, 0.0f, 1.0f),
        glm::vec4(0.0f, 1.0f, 0.0f, 1.0f),
        glm::vec4(0.0f, 1.0f, 1.0f, 1.0f),
        glm::vec4(0.0f, 0.0f, 1.0f, 1.0f),
        glm::vec4(1.0f, 0.0f, 1.0f, 1.0f),
        glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
    };
    static constexpr std::array<glm::vec4, 4> depthHues = {
        glm::vec4(0.25f, 0.5f, 1.0f, 1.0f),
        glm::vec4(0.2f, 0.85f, 0.25f, 1.0f),
        glm::vec4(1.0f, 0.55f, 0.05f, 1.0f),
        glm::vec4(0.9f, 0.2f, 0.9f, 1.0f),
    };
    if (byDepth) {
        return depthHues[std::clamp<uint32_t>(depth, 1, 4) - 1];
    }
    return levelRamp[std::min<uint32_t>(level, 7)];
}

auto lodRowShown(const LodWindowState& state, const LodWindowRow& row) -> bool {
    if (state.filter[0] != '\0' && !row.path.contains(state.filter)) {
        return false;
    }
    if ((state.kindFilter == 1 && row.group < 0) || (state.kindFilter == 2 && row.group >= 0)) {
        return false;
    }
    if (state.onlyUnderSelection) {
        const auto& base = state.selectedPath;
        bool under = !base.empty() && (base == "/" || row.path == base || (row.path.size() > base.size() && row.path.starts_with(base) && row.path[base.size()] == '/'));
        if (!under) {
            return false;
        }
    }
    if (state.onlyNotDrawingWanted && row.drawn == (int32_t) row.wanted) {
        return false;
    }
    if (state.onlyFading && row.fadeLevel < 0) {
        return false;
    }
    return true;
}

auto drawLodWindow(bool& show, LodWindowState& state) -> void {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LOD", &show)) {
        ImGui::End();
        return;
    }

    drawControls(state);
    drawLayers(state);
    {
        const auto& lod = state.renderLod;
        std::string drawing;
        for (size_t level = 0; level < lod.drawing.size(); level++) {
            if (lod.drawing[level] > 0) {
                drawing += std::format("{}{}: {}", drawing.empty() ? "" : ", ", level, lod.drawing[level]);
            }
        }
        ImGui::TextDisabled("Render LOD: %u groups, drawing levels %s; %u forced, %u falling back, %u fading, %llu pops since the scene opened",
                            lod.groups,
                            drawing.empty() ? "-" : drawing.c_str(),
                            lod.forced,
                            lod.fallbacks,
                            lod.fading,
                            (unsigned long long) lod.pops);
    }
    ImGui::Separator();

    std::vector<const LodWindowRow*> rows;
    rows.reserve(state.rows.size());
    size_t mostLevels = 0;
    for (const auto& row : state.rows) {
        if (!lodRowShown(state, row)) {
            continue;
        }
        rows.push_back(&row);
        mostLevels = std::max(mostLevels, row.levels.size());
    }

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##filter", "filter by path", state.filter, sizeof(state.filter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::Combo("##kind", &state.kindFilter, "every kind\0render LOD\0streaming\0");
    ImGui::SameLine();
    ImGui::Checkbox("under selection", &state.onlyUnderSelection);
    ImGui::SameLine();
    ImGui::Checkbox("not drawing wanted", &state.onlyNotDrawingWanted);
    ImGui::SameLine();
    ImGui::Checkbox("fading", &state.onlyFading);
    ImGui::SameLine();
    ImGui::Text("%zu of %zu roots", rows.size(), state.rows.size());
    drawSetShown(rows, mostLevels, state);
    ImGui::SameLine();
    ImGui::TextDisabled("click selects, double-click frames");

    // Scroll to the selected root once each time the selection moves to another one.
    int scrollTo = -1;
    if (state.selectedRoot != state.scrolledTo) {
        state.scrolledTo = state.selectedRoot;
        for (size_t i = 0; i < rows.size(); i++) {
            if (rows[i]->path == state.selectedRoot) {
                scrollTo = (int) i;
            }
        }
    }

    // The list on top, the inspector below.
    float listHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y * 0.45f);
    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##lod", 7, flags, ImVec2(0.0f, listHeight))) {
        ImGui::TableSetupColumn("Root", ImGuiTableColumnFlags_WidthStretch, 0.0f, ColumnRoot);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 50.0f, ColumnKind);
        ImGui::TableSetupColumn("Wants", ImGuiTableColumnFlags_WidthFixed, 140.0f, ColumnLevel);
        ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed, 150.0f, ColumnDrawn);
        ImGui::TableSetupColumn("Loaded", ImGuiTableColumnFlags_WidthFixed, 60.0f, ColumnLoaded);
        ImGui::TableSetupColumn("Measure", ImGuiTableColumnFlags_WidthFixed, 70.0f, ColumnScreen);
        ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthFixed, 100.0f, ColumnWhy);
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
    ImGui::End();
}

auto drawLodLabels(LodWindowState& state, const glm::mat4& viewProj, glm::vec2 viewport) -> void {
    if (!state.labels) {
        return;
    }
    auto* drawList = ImGui::GetForegroundDrawList();
    const auto& io = ImGui::GetIO();
    // At most this many, so a large scene's labels don't cover the view.
    constexpr size_t maxLabels = 400;
    size_t drawnLabels = 0;
    for (const auto& row : state.rows) {
        if (drawnLabels >= maxLabels) {
            break;
        }
        if (row.shape.kind == LodShape::Kind::None || !lodRowShown(state, row)) {
            continue;
        }
        auto clip = viewProj * glm::vec4(row.shape.anchor(), 1.0f);
        if (clip.w <= 1e-4f) {
            continue;
        }
        auto ndc = glm::vec2(clip) / clip.w;
        if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f) {
            continue;
        }
        // Vulkan's clip space has y down.
        ImVec2 at((ndc.x * 0.5f + 0.5f) * viewport.x, (ndc.y * 0.5f + 0.5f) * viewport.y);
        auto slash = row.path.find_last_of('/');
        auto name = slash == std::string::npos ? row.path : row.path.substr(slash + 1);
        std::string text = std::format("{}  {}", name, row.drawn >= 0 ? std::format("L{}", row.drawn) : std::string("-"));
        if (row.fadeLevel >= 0) {
            text += std::format(" → L{} {:.0f}%", row.fadeLevel, row.fade * 100.0f);
        }
        if (row.measure >= 0.0f) {
            text += row.measureUnit.empty() ? std::format("  {:.3f}%", row.measure * 100.0f) : std::format("  {:.1f} {}", row.measure, row.measureUnit);
        }
        if (row.setByHand) {
            text += "  forced";
        }
        auto size = ImGui::CalcTextSize(text.c_str());
        ImVec2 min(at.x - (size.x * 0.5f) - 4.0f, at.y - (size.y * 0.5f) - 2.0f);
        ImVec2 max(at.x + (size.x * 0.5f) + 4.0f, at.y + (size.y * 0.5f) + 2.0f);
        bool hovered = io.MousePos.x >= min.x && io.MousePos.x <= max.x && io.MousePos.y >= min.y && io.MousePos.y <= max.y;
        bool selected = row.path == state.selectedRoot;
        drawList->AddRectFilled(min, max, hovered || selected ? IM_COL32(40, 40, 40, 230) : IM_COL32(0, 0, 0, 160), 3.0f);
        if (row.drawn >= 0) {
            auto color = lodLevelColor((uint32_t) row.drawn, row.depth, state.colorByDepth);
            drawList->AddRectFilled(ImVec2(min.x, max.y - 2.0f), max, ImGui::ColorConvertFloat4ToU32(toImVec4(color)));
        }
        drawList->AddText(ImVec2(min.x + 4.0f, min.y + 2.0f), row.flashing ? IM_COL32(255, 200, 80, 255) : IM_COL32(235, 235, 235, 255), text.c_str());
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
