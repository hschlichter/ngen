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
    takeLog();
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
    if (ImGui::BeginTabBar("views")) {
        // Records: the processes and their records on the left, the selected record on the right.
        if (ImGui::BeginTabItem("Records")) {
            if (ImGui::BeginTable("records", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
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
            ImGui::EndTabItem();
        }
        // Log: every process merged by time, the full width.
        auto logFlags = openLogTab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        openLogTab = false;
        if (ImGui::BeginTabItem("Log", nullptr, logFlags)) {
            drawLog(processes);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

auto IntrospectWindow::selectWhenAvailable(std::string target, std::string record) -> void {
    pendingTarget = std::move(target);
    pendingRecord = std::move(record);
}

auto IntrospectWindow::showLog() -> void {
    openLogTab = true;
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

auto IntrospectWindow::takeLog() -> void {
    if (logPaused) {
        return;
    }
    incoming.clear();
    session.takeLogEvents(incoming);
    for (auto& event : incoming) {
        LogRow row;
        row.tsNs = event.tsNs;
        row.process = std::move(event.process);
        row.event = std::move(event.event);
        row.fieldsText = row.event->contains("fields") ? (*row.event)["fields"].dump() : "{}";
        auto severity = row.event->value("severity", "info");
        row.severity = severity == "error" ? 2 : (severity == "warning" ? 1 : 0);
        if (logOriginNs == 0) {
            logOriginNs = row.tsNs;
        }
        // Batches from different processes overlap in time; most events still land at the end.
        if (logRows.empty() || row.tsNs >= logRows.back().tsNs) {
            if (!logVisibleStale && logRowVisible(row)) {
                logVisible.push_back((int) logRows.size());
            }
            logRows.push_back(std::move(row));
        } else {
            auto at = std::ranges::upper_bound(logRows, row.tsNs, {}, &LogRow::tsNs);
            logRows.insert(at, std::move(row));
            logVisibleStale = true;
        }
    }
    if (logRows.size() > maxLogRows) {
        auto excess = logRows.size() - maxLogRows + maxLogRows / 10;
        logRows.erase(logRows.begin(), logRows.begin() + (std::ptrdiff_t) excess);
        logVisibleStale = true;
    }
}

auto IntrospectWindow::logRowVisible(const LogRow& row) const -> bool {
    auto contains = [](std::string_view text, const char* filter) {
        return filter[0] == '\0' || text.find(filter) != std::string_view::npos;
    };
    const auto& e = *row.event;
    if (row.severity < logMinSeverity) {
        return false;
    }
    if (!contains(row.process, logFilterProcess.data())) {
        return false;
    }
    if (!contains(e.value("category", ""), logFilterCategory.data())) {
        return false;
    }
    if (!contains(e.value("type", ""), logFilterType.data())) {
        return false;
    }
    if (logFilterText[0] != '\0') {
        return contains(e.value("name", ""), logFilterText.data()) || contains(e.value("text", ""), logFilterText.data()) || contains(row.fieldsText, logFilterText.data());
    }
    return true;
}

auto IntrospectWindow::drawLog(const std::vector<IntrospectSession::Process>& processes) -> void {
    ImGui::Checkbox("Pause", &logPaused);
    ImGui::SameLine();
    if (ImGui::Checkbox("Follow", &logFollow)) {
        logFollowScrollY = -1.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        logRows.clear();
        logVisible.clear();
        selectedEvent.reset();
        logFollowScrollY = -1.0f;
    }
    auto filter = [&](const char* label, char* buffer, size_t size, float width) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputTextWithHint(label, label + 2, buffer, size)) {
            logVisibleStale = true;
        }
    };
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::Combo("##severity", &logMinSeverity, "everything\0warnings, errors\0errors\0")) {
        logVisibleStale = true;
    }
    filter("##process", logFilterProcess.data(), logFilterProcess.size(), 110.0f);
    filter("##category", logFilterCategory.data(), logFilterCategory.size(), 110.0f);
    filter("##type", logFilterType.data(), logFilterType.size(), 140.0f);
    filter("##name, text or fields", logFilterText.data(), logFilterText.size(), 200.0f);
    if (logVisibleStale) {
        logVisible.clear();
        for (size_t i = 0; i < logRows.size(); i++) {
            if (logRowVisible(logRows[i])) {
                logVisible.push_back((int) i);
            }
        }
        logVisibleStale = false;
        // Fewer rows can move the scroll up on their own; that isn't the user scrolling.
        logFollowScrollY = -1.0f;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu of %zu events", logVisible.size(), logRows.size());
    for (const auto& [process, count] : session.logDropped()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s dropped %llu", process.c_str(), (unsigned long long) count);
    }
    // A process the tool can't reach sends no events; say so here, where its absence would otherwise go unnoticed.
    for (const auto& process : processes) {
        if (!process.error.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s: %s", process.name.c_str(), process.error.c_str());
        }
    }

    auto detailHeight = selectedEvent != nullptr ? ImGui::GetContentRegionAvail().y * 0.3f : 0.0f;
    auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                 ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("log", 8, flags, ImVec2(0.0f, -detailHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Fixed widths: the clipper only lays out visible rows, so fitting to contents would size from a handful.
        ImGui::TableSetupColumn("time s", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("process", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("thread", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("severity", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("category", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("text, fields", ImGuiTableColumnFlags_WidthFixed, 900.0f);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int) logVisible.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& row = logRows[(size_t) logVisible[(size_t) i]];
                const auto& e = *row.event;
                ImGui::TableNextRow();
                if (row.severity == 2) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                } else if (row.severity == 1) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
                }
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                auto time = std::format("{:.4f}", (double) (row.tsNs - logOriginNs) * 1e-9);
                bool selected = selectedEvent == row.event;
                if (ImGui::Selectable(time.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedEvent = row.event;
                    selectedEventProcess = row.process;
                    logFollow = false;
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(row.process.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(e.value("thread", "").c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(e.value("severity", "info").c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::TextUnformatted(e.value("category", "").c_str());
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(e.value("type", "").c_str());
                ImGui::TableSetColumnIndex(6);
                ImGui::TextUnformatted(e.value("name", "").c_str());
                ImGui::TableSetColumnIndex(7);
                // The message first; fields after it when there are any.
                auto message = e.value("text", "");
                if (row.fieldsText != "{}") {
                    message += message.empty() ? row.fieldsText : "  " + row.fieldsText;
                }
                std::ranges::replace(message, '\n', ' ');
                if (message.size() > 200) {
                    message = message.substr(0, 197) + "...";
                }
                ImGui::TextUnformatted(message.c_str());
                if (row.severity > 0) {
                    ImGui::PopStyleColor();
                }
            }
        }
        // Scrolling up by any means (wheel, scrollbar, keys) leaves the scroll above where following put it: stop
        // following, so the scroll stays where the user took it.
        if (logFollow && logFollowScrollY >= 0.0f && ImGui::GetScrollY() < logFollowScrollY - 1.0f) {
            logFollow = false;
        }
        if (logFollow && !logPaused) {
            ImGui::SetScrollY(ImGui::GetScrollMaxY());
            logFollowScrollY = ImGui::GetScrollMaxY();
        }
        ImGui::EndTable();
    }
    if (selectedEvent != nullptr) {
        ImGui::BeginChild("event", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextDisabled("%s", selectedEventProcess.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Close")) {
            selectedEvent.reset();
        } else {
            JsonView view(tableOrders);
            view.draw(selectedEvent->value("type", "event"), *selectedEvent, true);
        }
        ImGui::EndChild();
    }
}
