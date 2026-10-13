#include "assetswindow.h"

#include "uiformat.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace {

auto stateName(AssetLogState state) -> const char* {
    switch (state) {
        case AssetLogState::InFlight:
            return "in flight";
        case AssetLogState::Received:
            return "received";
        case AssetLogState::Failed:
            return "failed";
    }
    return "in flight";
}

// From request to arrival, or failure; -1 while in flight.
auto totalMs(const AssetLogEntry& entry) -> double {
    if (entry.doneMs < 0 || entry.requestedMs < 0) {
        return -1;
    }
    return entry.doneMs - entry.requestedMs;
}

// The total less the server's time: waiting in the server's queue and streaming. -1 when either is unknown.
auto restMs(const AssetLogEntry& entry) -> double {
    auto total = totalMs(entry);
    if (total < 0 || entry.serverMs < 0) {
        return -1;
    }
    return std::max(0.0, total - entry.serverMs);
}

auto msCell(double ms) -> void {
    if (ms < 0) {
        ImGui::TextDisabled("-");
    } else {
        ImGui::Text("%.1f", ms);
    }
}

enum Column : int {
    ColumnAsset,
    ColumnState,
    ColumnSource,
    ColumnDraws,
    ColumnSize,
    ColumnTotal,
    ColumnServer,
    ColumnRest,
    ColumnRequested,
    ColumnRequests,
};

auto compare(const AssetLogEntry& a, const AssetLogEntry& b, int column, const std::function<size_t(const std::string&)>& drawCount) -> int {
    auto order = [](auto x, auto y) {
        return x < y ? -1 : (y < x ? 1 : 0);
    };
    switch (column) {
        case ColumnState:
            return order((int) a.state, (int) b.state);
        case ColumnSource:
            return order(a.packed, b.packed);
        case ColumnDraws:
            return drawCount ? order(drawCount(a.id), drawCount(b.id)) : 0;
        case ColumnSize:
            return order(a.bytes, b.bytes);
        case ColumnTotal:
            return order(totalMs(a), totalMs(b));
        case ColumnServer:
            return order(a.serverMs, b.serverMs);
        case ColumnRest:
            return order(restMs(a), restMs(b));
        case ColumnRequested:
            return order(a.requestedMs, b.requestedMs);
        case ColumnRequests:
            return order(a.requests, b.requests);
        default:
            return a.id.compare(b.id);
    }
}

} // namespace

auto drawAssetsWindow(bool& show, const std::vector<AssetLogEntry>& log, AssetsWindowState& state) -> void {
    if (!show) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(980, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Assets", &show)) {
        ImGui::End();
        return;
    }

    size_t inFlight = 0;
    size_t received = 0;
    size_t packed = 0;
    size_t failed = 0;
    uint64_t bytes = 0;
    double slowest = 0;
    for (const auto& entry : log) {
        if (entry.state == AssetLogState::InFlight) {
            inFlight++;
        } else if (entry.state == AssetLogState::Received) {
            received++;
            bytes += entry.bytes;
            if (entry.packed) {
                packed++;
            }
        } else {
            failed++;
        }
        slowest = std::max(slowest, totalMs(entry));
    }
    char total[32];
    ImGui::Text("%zu assets: %zu in flight, %zu received (%zu packed, %zu from the cache), %zu failed; %s received; slowest %.0f ms",
                log.size(),
                inFlight,
                received,
                packed,
                received - packed,
                failed,
                formatByteCount(bytes, total, sizeof(total)),
                slowest);
    ImGui::TextDisabled("Total: request to arrival. Server: its cache check and packing. Rest: waiting in the server's queue and streaming.");

    if (ImGui::Button("Repack scene")) {
        state.repackSceneRequested = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Has the asset server repack every asset of the scene (layer packs, meshes, materials, textures), and rebuilds the scene");
    }
    ImGui::TextDisabled("Repacking a shader refreshes only the server's cache: the view takes new shaders at its next start.");

    ImGui::SetNextItemWidth(320.0f);
    ImGui::InputTextWithHint("##filter", "filter by id", state.filter, sizeof(state.filter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    const char* states[] = {"all", "in flight", "received", "failed"};
    ImGui::Combo("##state", &state.stateFilter, states, 4);
    ImGui::SameLine();
    ImGui::Checkbox("Show selected only", &state.showSelectedOnly);
    ImGui::SameLine();
    ImGui::BeginDisabled(!state.hasPrimSelected);
    if (ImGui::Button("Selected prim's assets")) {
        state.findSelectedRequested = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Selects the assets the prim selected in the view uses (and everything drawn under it): meshes, the USD files defining them, materials and their textures");
    }

    std::vector<const AssetLogEntry*> rows;
    rows.reserve(log.size());
    for (const auto& entry : log) {
        if (state.stateFilter != 0 && (int) entry.state != state.stateFilter - 1) {
            continue;
        }
        if (state.filter[0] != '\0' && entry.id.find(state.filter) == std::string::npos) {
            continue;
        }
        if (state.showSelectedOnly && !state.selected.contains(entry.id)) {
            continue;
        }
        rows.push_back(&entry);
    }

    // The selection, in the log's order.
    auto repackSelection = [&] {
        for (const auto& entry : log) {
            if (state.selected.contains(entry.id)) {
                state.repackRequested.push_back(entry.id);
            }
        }
    };
    auto selectShown = [&] {
        for (const auto* entry : rows) {
            state.selected.insert(entry->id);
        }
    };
    ImGui::BeginDisabled(state.selected.empty());
    if (ImGui::Button(std::format("Repack selected ({})###repackselected", state.selected.size()).c_str())) {
        repackSelection();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear selection")) {
        state.selected.clear();
        state.anchor.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Select shown")) {
        selectShown();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("click, Ctrl+click, Shift+click, Ctrl+A; double-click goes to the asset");
    if (!state.goToStatus.empty()) {
        ImGui::TextUnformatted(state.goToStatus.c_str());
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        selectShown();
    }

    auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##assets", 10, flags)) {
        ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 0.0f, ColumnAsset);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f, ColumnState);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 60.0f, ColumnSource);
        ImGui::TableSetupColumn("Draws", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 50.0f, ColumnDraws);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f, ColumnSize);
        ImGui::TableSetupColumn("Total ms", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 70.0f, ColumnTotal);
        ImGui::TableSetupColumn("Server ms", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 70.0f, ColumnServer);
        ImGui::TableSetupColumn("Rest ms", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 70.0f, ColumnRest);
        ImGui::TableSetupColumn("At s", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, 60.0f, ColumnRequested);
        ImGui::TableSetupColumn("Requests", ImGuiTableColumnFlags_WidthFixed, 60.0f, ColumnRequests);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (auto* sort = ImGui::TableGetSortSpecs(); sort != nullptr && sort->SpecsCount > 0) {
            auto column = (int) sort->Specs[0].ColumnUserID;
            bool descending = sort->Specs[0].SortDirection == ImGuiSortDirection_Descending;
            std::ranges::stable_sort(rows, [&](const AssetLogEntry* a, const AssetLogEntry* b) {
                auto order = compare(*a, *b, column, state.drawCount);
                return descending ? order > 0 : order < 0;
            });
        }

        ImGuiListClipper clipper;
        clipper.Begin((int) rows.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const auto& entry = *rows[(size_t) i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(ColumnAsset);
                ImGui::PushID(i);
                bool isSelected = state.selected.contains(entry.id);
                if (ImGui::Selectable(entry.id.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    const auto& io = ImGui::GetIO();
                    auto anchor = std::ranges::find_if(rows, [&](const AssetLogEntry* row) { return row->id == state.anchor; });
                    if (io.KeyShift && anchor != rows.end()) {
                        // The rows from the anchor to this one, as shown; Ctrl+Shift adds them to the selection.
                        if (!io.KeyCtrl) {
                            state.selected.clear();
                        }
                        auto from = std::min((int) (anchor - rows.begin()), i);
                        auto to = std::max((int) (anchor - rows.begin()), i);
                        for (int r = from; r <= to; r++) {
                            state.selected.insert(rows[(size_t) r]->id);
                        }
                    } else if (io.KeyCtrl) {
                        if (isSelected) {
                            state.selected.erase(entry.id);
                        } else {
                            state.selected.insert(entry.id);
                        }
                        state.anchor = entry.id;
                    } else {
                        state.selected = {entry.id};
                        state.anchor = entry.id;
                    }
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        state.goToRequested = AssetsWindowState::GoTo{.id = entry.id, .all = false};
                    }
                }
                if (ImGui::BeginPopupContextItem("##row")) {
                    // A right-click outside the selection selects that row alone, as file managers do.
                    if (!state.selected.contains(entry.id)) {
                        state.selected = {entry.id};
                        state.anchor = entry.id;
                    }
                    auto draws = state.drawCount ? state.drawCount(entry.id) : 0;
                    if (ImGui::MenuItem("Go to", nullptr, false, draws > 0)) {
                        state.goToRequested = AssetsWindowState::GoTo{.id = entry.id, .all = false};
                    }
                    if (ImGui::MenuItem(std::format("Frame all ({} draws)", draws).c_str(), nullptr, false, draws > 0)) {
                        state.goToRequested = AssetsWindowState::GoTo{.id = entry.id, .all = true};
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem(std::format("Repack {} selected", state.selected.size()).c_str())) {
                        repackSelection();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\nversion %s", entry.id.c_str(), entry.version.empty() ? "-" : entry.version.c_str());
                }
                ImGui::TableSetColumnIndex(ColumnState);
                if (entry.state == AssetLogState::Failed) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", stateName(entry.state));
                    if (ImGui::IsItemHovered()) {
                        std::string errors;
                        for (const auto& line : entry.errors) {
                            errors += line + "\n";
                        }
                        ImGui::SetTooltip("%s", errors.empty() ? "no reason given" : errors.c_str());
                    }
                } else {
                    ImGui::TextUnformatted(stateName(entry.state));
                }
                ImGui::TableSetColumnIndex(ColumnSource);
                if (entry.state == AssetLogState::Received) {
                    ImGui::TextUnformatted(entry.packed ? "packed" : "cache");
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(ColumnDraws);
                if (auto draws = state.drawCount ? state.drawCount(entry.id) : 0; draws > 0) {
                    ImGui::Text("%zu", draws);
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(ColumnSize);
                if (entry.state == AssetLogState::Received) {
                    char size[32];
                    ImGui::TextUnformatted(formatByteCount(entry.bytes, size, sizeof(size)));
                } else {
                    ImGui::TextDisabled("-");
                }
                ImGui::TableSetColumnIndex(ColumnTotal);
                msCell(totalMs(entry));
                ImGui::TableSetColumnIndex(ColumnServer);
                msCell(entry.serverMs);
                ImGui::TableSetColumnIndex(ColumnRest);
                msCell(restMs(entry));
                ImGui::TableSetColumnIndex(ColumnRequested);
                ImGui::Text("%.2f", entry.requestedMs / 1000.0);
                ImGui::TableSetColumnIndex(ColumnRequests);
                ImGui::Text("%u", entry.requests);
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
