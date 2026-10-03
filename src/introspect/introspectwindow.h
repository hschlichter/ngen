#pragma once

#include "introspectsession.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ngen-introspect's window, drawn with Dear ImGui: the processes on the left with their records, the selected
// record on the right. Any record is shown the same way: objects as a tree, arrays of objects as sortable tables.
class IntrospectWindow {
public:
    explicit IntrospectWindow(IntrospectSession& session);

    // Draws the whole window into the main viewport. Call between ImGui::NewFrame and ImGui::Render.
    auto draw() -> void;
    // Selects a record as soon as a process matching `target` (a kind, or "<kind>:<pid>") is there.
    auto selectWhenAvailable(std::string target, std::string record) -> void;

private:
    auto drawProcesses(const std::vector<IntrospectSession::Process>& processes) -> void;
    auto drawRecord() -> void;

    IntrospectSession& session;
    std::string selectedProcess;
    std::string selectedRecord;
    std::string pendingTarget;
    std::string pendingRecord;
    bool autoRefresh = false;
    int refreshIntervalMs = 500;
    std::chrono::steady_clock::time_point lastFetch;
    // Sorted row order per table, by ImGui id; rebuilt when the sort or the row count changes.
    std::unordered_map<uint32_t, std::vector<int>> tableOrders;
};
