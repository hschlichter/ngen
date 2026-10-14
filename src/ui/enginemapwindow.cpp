#include "enginemapwindow.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace {

constexpr float laneWidth = 230.0f;
constexpr float rowHeight = 86.0f;
constexpr float nodeWidth = 196.0f;
constexpr float nodeHeight = 64.0f;
constexpr float laneHeader = 28.0f;
constexpr float margin = 12.0f;

auto nodeMin(const EngineMapNode& node, ImVec2 origin) -> ImVec2 {
    return ImVec2(origin.x + margin + ((float) node.lane * laneWidth), origin.y + laneHeader + margin + ((float) node.row * rowHeight));
}

auto nodeCenter(const EngineMapNode& node, ImVec2 origin) -> ImVec2 {
    auto min = nodeMin(node, origin);
    return ImVec2(min.x + (nodeWidth * 0.5f), min.y + (nodeHeight * 0.5f));
}

// Where the segment from the box's centre towards `toward` leaves the box.
auto boxExit(ImVec2 center, ImVec2 toward) -> ImVec2 {
    float dx = toward.x - center.x;
    float dy = toward.y - center.y;
    float halfW = nodeWidth * 0.5f;
    float halfH = nodeHeight * 0.5f;
    float scaleX = std::abs(dx) > 0.001f ? halfW / std::abs(dx) : 1e9f;
    float scaleY = std::abs(dy) > 0.001f ? halfH / std::abs(dy) : 1e9f;
    float scale = std::min({scaleX, scaleY, 1.0f});
    return ImVec2(center.x + (dx * scale), center.y + (dy * scale));
}

auto drawArrow(ImDrawList* draw, ImVec2 from, ImVec2 to, ImU32 color, float thickness) -> void {
    draw->AddLine(from, to, color, thickness);
    float dx = to.x - from.x;
    float dy = to.y - from.y;
    float length = std::sqrt((dx * dx) + (dy * dy));
    if (length < 1.0f) {
        return;
    }
    dx /= length;
    dy /= length;
    float size = 8.0f;
    ImVec2 left(to.x - (dx * size) - (dy * size * 0.5f), to.y - (dy * size) + (dx * size * 0.5f));
    ImVec2 right(to.x - (dx * size) + (dy * size * 0.5f), to.y - (dy * size) - (dx * size * 0.5f));
    draw->AddTriangleFilled(to, left, right, color);
}

auto drawLanes(ImDrawList* draw, ImVec2 origin, float height) -> void {
    auto lanes = engineMapLanes();
    auto textColor = ImGui::GetColorU32(ImGuiCol_Text);
    for (size_t lane = 0; lane < lanes.size(); lane++) {
        ImVec2 min(origin.x + margin + ((float) lane * laneWidth) - 8.0f, origin.y);
        ImVec2 max(min.x + laneWidth - 8.0f, origin.y + height);
        float alpha = lane % 2 == 0 ? 0.35f : 0.18f;
        draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg, alpha), 4.0f);
        draw->AddText(ImVec2(min.x + 8.0f, origin.y + 6.0f), textColor, lanes[lane]);
    }
}

// Every edge faint; the selected node's highlighted and labelled with what flows.
auto drawEdges(ImDrawList* draw, ImVec2 origin, const std::string& selected) -> void {
    auto textColor = ImGui::GetColorU32(ImGuiCol_Text);
    for (const auto& edge : engineMapEdges()) {
        const auto* from = engineMapNode(edge.from);
        const auto* to = engineMapNode(edge.to);
        if (from == nullptr || to == nullptr) {
            continue;
        }
        bool lit = selected == edge.from || selected == edge.to;
        auto fromCenter = nodeCenter(*from, origin);
        auto toCenter = nodeCenter(*to, origin);
        auto start = boxExit(fromCenter, toCenter);
        auto end = boxExit(toCenter, fromCenter);
        auto color = lit ? ImGui::GetColorU32(ImGuiCol_PlotHistogram) : ImGui::GetColorU32(ImGuiCol_Border);
        drawArrow(draw, start, end, color, lit ? 2.5f : 1.0f);
        if (!lit) {
            continue;
        }
        ImVec2 mid((start.x + end.x) * 0.5f, (start.y + end.y) * 0.5f);
        auto size = ImGui::CalcTextSize(edge.what);
        ImVec2 textMin(mid.x - (size.x * 0.5f), mid.y - (size.y * 0.5f));
        draw->AddRectFilled(ImVec2(textMin.x - 3.0f, textMin.y - 1.0f), ImVec2(textMin.x + size.x + 3.0f, textMin.y + size.y + 1.0f), ImGui::GetColorU32(ImGuiCol_PopupBg), 3.0f);
        draw->AddText(textMin, textColor, edge.what);
    }
}

// One node: a box with its label and its first live numbers; a click selects it.
auto drawNode(ImDrawList* draw, ImVec2 origin, const EngineMapNode& node, EngineMapWindowState& state) -> void {
    auto min = nodeMin(node, origin);
    ImVec2 max(min.x + nodeWidth, min.y + nodeHeight);
    bool selected = state.selected == node.id;
    ImGui::SetCursorScreenPos(min);
    ImGui::PushID(node.id);
    if (ImGui::InvisibleButton("##node", ImVec2(nodeWidth, nodeHeight))) {
        state.selected = node.id;
    }
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    auto fillColor = hovered ? ImGuiCol_HeaderHovered : ImGuiCol_Header;
    if (selected) {
        fillColor = ImGuiCol_HeaderActive;
    }
    draw->AddRectFilled(min, max, ImGui::GetColorU32(fillColor), 6.0f);
    draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), 6.0f, 0, selected ? 2.0f : 1.0f);
    draw->AddText(ImVec2(min.x + 8.0f, min.y + 6.0f), ImGui::GetColorU32(ImGuiCol_Text), node.label);
    if (auto stats = state.stats.find(node.id); stats != state.stats.end()) {
        float y = min.y + 8.0f + ImGui::GetTextLineHeight();
        draw->PushClipRect(min, max, true);
        for (size_t i = 0; i < stats->second.size() && i < 2; i++) {
            draw->AddText(ImVec2(min.x + 8.0f, y), ImGui::GetColorU32(ImGuiCol_Text, 0.8f), stats->second[i].c_str());
            y += ImGui::GetTextLineHeight();
        }
        draw->PopClipRect();
    }
    if (selected && state.scrollToSelected) {
        state.scrollToSelected = false;
        ImGui::SetScrollHereY(0.5f);
        ImGui::SetScrollFromPosX(min.x - ImGui::GetWindowPos().x, 0.5f);
    }
}

auto drawMap(EngineMapWindowState& state, float mapHeight) -> void {
    int rows = 0;
    for (const auto& node : engineMapNodes()) {
        rows = std::max(rows, node.row + 1);
    }
    ImVec2 canvas((laneWidth * (float) engineMapLanes().size()) + (margin * 2.0f), laneHeader + (rowHeight * (float) rows) + (margin * 2.0f));
    ImGui::BeginChild("##map", ImVec2(0.0f, mapHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    auto origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    drawLanes(draw, origin, canvas.y);
    drawEdges(draw, origin, state.selected);
    for (const auto& node : engineMapNodes()) {
        drawNode(draw, origin, node, state);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(canvas);
    ImGui::EndChild();
}

auto drawDetails(const EngineMapWindowState& state, const std::function<void(const char*)>& openWindow) -> void {
    const auto* node = engineMapNode(state.selected);
    if (node == nullptr) {
        ImGui::TextDisabled("Click a part of the engine.");
        return;
    }
    auto lanes = engineMapLanes();
    ImGui::Text("%s  (%s)", node->label, lanes[(size_t) node->lane]);
    ImGui::TextWrapped("%s", node->description);
    for (const auto& edge : engineMapEdges()) {
        if (state.selected == edge.to) {
            const auto* from = engineMapNode(edge.from);
            ImGui::BulletText("receives from %s: %s", from != nullptr ? from->label : edge.from, edge.what);
        }
    }
    for (const auto& edge : engineMapEdges()) {
        if (state.selected == edge.from) {
            const auto* to = engineMapNode(edge.to);
            ImGui::BulletText("hands to %s: %s", to != nullptr ? to->label : edge.to, edge.what);
        }
    }
    if (auto stats = state.stats.find(node->id); stats != state.stats.end() && !stats->second.empty()) {
        ImGui::SeparatorText("Now");
        for (const auto& line : stats->second) {
            ImGui::TextUnformatted(line.c_str());
        }
    }
    ImGui::SeparatorText("Look at it");
    if (!node->records.empty()) {
        std::string records;
        for (const auto* record : node->records) {
            records += (records.empty() ? "" : ", ") + std::string(record);
        }
        bool server = node->lane == 0;
        ImGui::Text("records: %s%s", records.c_str(), server ? " (on ngen-asset-server: ngen-cli introspect get asset:<pid> <record>)" : "");
    }
    for (const auto* window : node->windows) {
        if (ImGui::SmallButton(std::format("Open {}###open{}", window, window).c_str())) {
            openWindow(window);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Text("README: %s", node->readme);
}

} // namespace

auto drawEngineMapWindow(bool& show, EngineMapWindowState& state, const std::function<void(const char*)>& openWindow) -> void {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(1180, 900), ImGuiCond_FirstUseEver);
    if (std::exchange(state.focusWindow, false)) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("Engine Map", &show)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Where each part of the engine runs and what flows between them. Click a part; the `map` record has the same.");
    auto available = ImGui::GetContentRegionAvail().y;
    drawMap(state, std::max(200.0f, available * 0.68f));
    drawDetails(state, openWindow);
    ImGui::End();
}
