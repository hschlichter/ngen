#include "lodwindow.h"

#include <imgui.h>

#include <algorithm>
#include <format>

namespace {

enum Column : int {
    ColumnRoot,
    ColumnLevel,
    ColumnDrawn,
    ColumnLoaded,
};

auto levelName(const LodWindowRow& row, uint32_t level) -> std::string {
    if (level < row.levels.size()) {
        return std::format("{} ({})", level, row.levels[level]);
    }
    return std::format("{}", level);
}

auto loadedText(const LodWindowRow& row) -> std::string {
    std::string text;
    for (uint32_t level : row.loaded) {
        if (!text.empty()) {
            text += ", ";
        }
        text += std::format("{}", level);
    }
    return text;
}

// The level picker: the root's default, or one of its levels. A choice asks the application for it.
auto levelCombo(const LodWindowRow& row, LodWindowState& state) -> void {
    auto preview = row.setByHand ? levelName(row, row.wanted) : std::format("default: {}", levelName(row, row.defaultIndex));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::BeginCombo("##level", preview.c_str())) {
        return;
    }
    if (ImGui::Selectable(std::format("default: {}", levelName(row, row.defaultIndex)).c_str(), !row.setByHand)) {
        state.levelRequests.push_back({.root = row.path, .level = std::nullopt});
    }
    for (uint32_t level = 0; level < row.levels.size(); level++) {
        if (ImGui::Selectable(levelName(row, level).c_str(), row.setByHand && row.wanted == level)) {
            state.levelRequests.push_back({.root = row.path, .level = level});
        }
    }
    ImGui::EndCombo();
}

// One row: the root (click selects, double-click frames), its level picker, the level drawn and the levels loaded.
auto drawRow(const LodWindowRow& row, LodWindowState& state) -> void {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeightWithSpacing());
    ImGui::PushID(row.path.c_str());

    ImGui::TableSetColumnIndex(ColumnRoot);
    ImGui::AlignTextToFramePadding();
    bool selected = row.path == state.selectedRoot;
    auto selectableFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick;
    if (ImGui::Selectable(row.path.c_str(), selected, selectableFlags, ImVec2(0.0f, ImGui::GetFrameHeight()))) {
        state.selectRequested = row.path;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            state.frameRequested = row.path;
        }
    }
    if (ImGui::IsItemHovered()) {
        std::string levels;
        for (size_t level = 0; level < row.levels.size(); level++) {
            levels += std::format("\n  {}", levelName(row, (uint32_t) level));
        }
        ImGui::SetTooltip("%s\nlevels, finest first:%s", row.path.c_str(), levels.c_str());
    }

    ImGui::TableSetColumnIndex(ColumnLevel);
    levelCombo(row, state);

    ImGui::TableSetColumnIndex(ColumnDrawn);
    ImGui::AlignTextToFramePadding();
    if (row.drawn < 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "loading");
    } else if (row.drawn != (int32_t) row.wanted) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s, loading", levelName(row, (uint32_t) row.drawn).c_str());
    } else {
        ImGui::TextUnformatted(levelName(row, (uint32_t) row.drawn).c_str());
    }

    ImGui::TableSetColumnIndex(ColumnLoaded);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(loadedText(row).c_str());
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
        ImGui::SetTooltip("Sets every root the filter shows. Finer levels load what they need: narrow the filter first on a large scene.");
    }
}

} // namespace

auto drawLodWindow(bool& show, LodWindowState& state) -> void {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("LOD", &show)) {
        ImGui::End();
        return;
    }

    std::vector<const LodWindowRow*> rows;
    rows.reserve(state.rows.size());
    size_t mostLevels = 0;
    size_t loading = 0;
    for (const auto& row : state.rows) {
        if (state.filter[0] != '\0' && !row.path.contains(state.filter)) {
            continue;
        }
        rows.push_back(&row);
        mostLevels = std::max(mostLevels, row.levels.size());
        if (row.drawn != (int32_t) row.wanted) {
            loading++;
        }
    }

    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##filter", "filter by path", state.filter, sizeof(state.filter));
    ImGui::SameLine();
    ImGui::Text("%zu of %zu roots shown, %zu loading", rows.size(), state.rows.size(), loading);

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

    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##lod", 4, flags)) {
        ImGui::TableSetupColumn("Root", ImGuiTableColumnFlags_WidthStretch, 0.0f, ColumnRoot);
        ImGui::TableSetupColumn("Wants", ImGuiTableColumnFlags_WidthFixed, 150.0f, ColumnLevel);
        ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed, 110.0f, ColumnDrawn);
        ImGui::TableSetupColumn("Loaded", ImGuiTableColumnFlags_WidthFixed, 70.0f, ColumnLoaded);
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
    ImGui::End();
}
