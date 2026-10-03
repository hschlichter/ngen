#include "introspectwindow.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <numeric>
#include <set>

namespace {

using Json = nlohmann::json;

// Columns past this many are left out of a table; the row's tree view still has every field.
constexpr size_t maxTableColumns = 48;

auto scalarText(const Json& value) -> std::string {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_float()) {
        return std::format("{:.4g}", value.get<double>());
    }
    return value.dump();
}

// An array is a table when it is non-empty and every element is an object.
auto isTable(const Json& value) -> bool {
    if (!value.is_array() || value.empty()) {
        return false;
    }
    return std::ranges::all_of(value, [](const Json& element) { return element.is_object(); });
}

// The table's columns: every key of every row, in first-seen order.
auto tableColumns(const Json& rows) -> std::vector<std::string> {
    std::vector<std::string> columns;
    std::set<std::string> seen;
    for (const auto& row : rows) {
        for (const auto& [key, value] : row.items()) {
            if (seen.insert(key).second) {
                columns.push_back(key);
            }
        }
        if (columns.size() >= maxTableColumns) {
            break;
        }
    }
    return columns;
}

auto cellText(const Json& row, const std::string& column) -> std::string {
    auto it = row.find(column);
    if (it == row.end()) {
        return "";
    }
    if (it->is_object() || it->is_array()) {
        auto text = it->dump();
        if (text.size() > 80) {
            text = text.substr(0, 77) + "...";
        }
        return text;
    }
    return scalarText(*it);
}

class JsonView {
public:
    explicit JsonView(std::unordered_map<uint32_t, std::vector<int>>& tableOrders) : tableOrders(tableOrders) {
    }

    auto draw(const std::string& label, const Json& value, bool open) -> void {
        if (value.is_object()) {
            drawObject(label, value, open);
        } else if (isTable(value)) {
            drawTable(label, value, open);
        } else if (value.is_array()) {
            drawArray(label, value, open);
        } else {
            ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_Bullet, "%s: %s", label.c_str(), scalarText(value).c_str());
        }
    }

private:
    auto drawObject(const std::string& label, const Json& value, bool open) -> void {
        ImGui::SetNextItemOpen(open, ImGuiCond_Once);
        if (!ImGui::TreeNode(label.c_str(), "%s {%zu}", label.c_str(), value.size())) {
            return;
        }
        // A record's own tables open with it; everything deeper starts closed.
        for (const auto& [key, child] : value.items()) {
            draw(key, child, open && isTable(child));
        }
        ImGui::TreePop();
    }

    auto drawArray(const std::string& label, const Json& value, bool open) -> void {
        ImGui::SetNextItemOpen(open, ImGuiCond_Once);
        if (!ImGui::TreeNode(label.c_str(), "%s [%zu]", label.c_str(), value.size())) {
            return;
        }
        ImGuiListClipper clipper;
        clipper.Begin((int) value.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                ImGui::PushID(i);
                draw(std::format("[{}]", i), value[(size_t) i], false);
                ImGui::PopID();
            }
        }
        ImGui::TreePop();
    }

    auto drawTable(const std::string& label, const Json& rows, bool open) -> void {
        ImGui::SetNextItemOpen(open, ImGuiCond_Once);
        if (!ImGui::TreeNode(label.c_str(), "%s [%zu]", label.c_str(), rows.size())) {
            return;
        }
        auto columns = tableColumns(rows);
        auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate | ImGuiTableFlags_ScrollX |
                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        auto visibleRows = std::min<float>((float) rows.size() + 1.5f, 24.0f);
        auto height = ImGui::GetTextLineHeightWithSpacing() * visibleRows;
        if (ImGui::BeginTable("table", (int) columns.size(), flags, ImVec2(0.0f, height))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            for (const auto& column : columns) {
                ImGui::TableSetupColumn(column.c_str());
            }
            ImGui::TableHeadersRow();
            const auto& order = sortedOrder(ImGui::GetID("table"), rows, columns);
            ImGuiListClipper clipper;
            clipper.Begin((int) order.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    const auto& row = rows[(size_t) order[(size_t) i]];
                    ImGui::TableNextRow();
                    for (size_t c = 0; c < columns.size(); c++) {
                        ImGui::TableSetColumnIndex((int) c);
                        ImGui::TextUnformatted(cellText(row, columns[c]).c_str());
                    }
                }
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }

    // The row order for the table's current sort. Rebuilt when the sort changes or the rows were replaced.
    auto sortedOrder(uint32_t id, const Json& rows, const std::vector<std::string>& columns) -> const std::vector<int>& {
        auto& order = tableOrders[id];
        auto* specs = ImGui::TableGetSortSpecs();
        bool stale = order.size() != rows.size() || (specs != nullptr && specs->SpecsDirty);
        if (!stale) {
            return order;
        }
        order.resize(rows.size());
        std::iota(order.begin(), order.end(), 0);
        if (specs != nullptr && specs->SpecsCount > 0) {
            const auto& spec = specs->Specs[0];
            const auto& column = columns[(size_t) spec.ColumnIndex];
            bool ascending = spec.SortDirection != ImGuiSortDirection_Descending;
            std::ranges::stable_sort(order, [&](int a, int b) {
                const auto& left = rows[(size_t) a].value(column, Json());
                const auto& right = rows[(size_t) b].value(column, Json());
                if (ascending) {
                    return left < right;
                }
                return right < left;
            });
        }
        if (specs != nullptr) {
            specs->SpecsDirty = false;
        }
        return order;
    }

    std::unordered_map<uint32_t, std::vector<int>>& tableOrders;
};

} // namespace

IntrospectWindow::IntrospectWindow(IntrospectSession& session) : session(session) {
}

auto IntrospectWindow::draw() -> void {
    session.poll();
    auto processes = session.processes();

    if (!pendingTarget.empty()) {
        for (const auto& process : processes) {
            if (process.name == pendingTarget || process.endpoint.kind == pendingTarget) {
                selectedProcess = process.name;
                selectedRecord = pendingRecord;
                session.fetch(selectedProcess, selectedRecord);
                lastFetch = std::chrono::steady_clock::now();
                pendingTarget.clear();
                break;
            }
        }
    }

    // A selection whose process is gone is dropped.
    bool selectionLive = std::ranges::any_of(processes, [&](const IntrospectSession::Process& p) { return p.name == selectedProcess; });
    if (!selectionLive) {
        selectedProcess.clear();
        selectedRecord.clear();
    }

    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("ngen-introspect", nullptr, flags);
    if (ImGui::BeginTable("layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("processes", ImGuiTableColumnFlags_WidthFixed, 320.0f);
        ImGui::TableSetupColumn("record", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("processes");
        drawProcesses(processes);
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("record", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        drawRecord();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

auto IntrospectWindow::selectWhenAvailable(std::string target, std::string record) -> void {
    pendingTarget = std::move(target);
    pendingRecord = std::move(record);
}

auto IntrospectWindow::drawProcesses(const std::vector<IntrospectSession::Process>& processes) -> void {
    ImGui::TextDisabled("Processes");
    ImGui::Separator();
    if (processes.empty()) {
        ImGui::TextWrapped("No processes. Tools announce themselves in .ngen-discovery/ in this working directory.");
        return;
    }
    for (const auto& process : processes) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        bool open = ImGui::TreeNode(process.name.c_str(), "%s", process.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", process.endpoint.label.c_str());
        if (!open) {
            continue;
        }
        if (!process.error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", process.error.c_str());
        }
        for (const auto& record : process.records) {
            bool selected = process.name == selectedProcess && record.name == selectedRecord;
            ImGui::PushID(record.name.c_str());
            if (ImGui::Selectable(record.name.c_str(), selected)) {
                selectedProcess = process.name;
                selectedRecord = record.name;
                session.fetch(selectedProcess, selectedRecord);
                lastFetch = std::chrono::steady_clock::now();
            }
            if (ImGui::IsItemHovered() && !record.description.empty()) {
                ImGui::SetTooltip("%s", record.description.c_str());
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

auto IntrospectWindow::drawRecord() -> void {
    if (selectedProcess.empty()) {
        ImGui::TextDisabled("Pick a record on the left.");
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (autoRefresh && now - lastFetch >= std::chrono::milliseconds(refreshIntervalMs)) {
        session.fetch(selectedProcess, selectedRecord);
        lastFetch = now;
    }
    auto value = session.record(selectedProcess, selectedRecord);

    ImGui::Text("%s  %s", selectedProcess.c_str(), selectedRecord.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        session.fetch(selectedProcess, selectedRecord);
        lastFetch = now;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &autoRefresh);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt("ms", &refreshIntervalMs, 100, 5000);
    ImGui::SameLine();
    ImGui::BeginDisabled(value.value == nullptr);
    if (ImGui::Button("Copy JSON")) {
        ImGui::SetClipboardText(value.value->dump(2).c_str());
    }
    ImGui::EndDisabled();
    if (value.pending) {
        ImGui::SameLine();
        ImGui::TextDisabled("fetching...");
    } else if (value.value != nullptr) {
        auto age = std::chrono::duration<double>(now - value.received).count();
        ImGui::SameLine();
        ImGui::TextDisabled("%.0f ms round trip, %.1f s ago", value.roundTripMs, age);
    }
    if (!value.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", value.error.c_str());
    }
    ImGui::Separator();
    if (value.value == nullptr) {
        return;
    }
    ImGui::PushID(selectedProcess.c_str());
    ImGui::PushID(selectedRecord.c_str());
    JsonView view(tableOrders);
    view.draw(selectedRecord, *value.value, true);
    ImGui::PopID();
    ImGui::PopID();
}
