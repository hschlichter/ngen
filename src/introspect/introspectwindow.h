#pragma once

#include "introspectsession.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// ngen-introspect's window, drawn with Dear ImGui, in two tabs. Records: the processes with their records on the left, the
// selected record on the right. Trace: every process's events merged by time, the full width. Any record is shown the
// same way: objects as a tree, arrays of objects as sortable tables.
class IntrospectWindow {
public:
    explicit IntrospectWindow(IntrospectSession& session);

    // Draws the whole window into the main viewport. Call between ImGui::NewFrame and ImGui::Render.
    auto draw() -> void;
    // Selects a record as soon as a process matching `target` (a kind, or "<kind>:<pid>") is there.
    auto selectWhenAvailable(std::string target, std::string record) -> void;
    // Opens the Trace tab instead of the Records tab.
    auto showTrace() -> void;
    // Events kept in the Trace tab; the oldest go first.
    static constexpr size_t maxTraceRows = 200'000;

private:
    struct TraceRow {
        uint64_t tsNs = 0;
        std::string process;
        std::shared_ptr<const rpc::Json> event;
        int level = 0;          // 0 info, 1 warning, 2 error
        std::string fieldsText; // the fields as compact JSON, for the table and the text filter
    };

    auto drawProcesses(const std::vector<IntrospectSession::Process>& processes) -> void;
    auto drawRecord() -> void;
    auto takeTrace() -> void;
    auto traceRowVisible(const TraceRow& row) const -> bool;
    auto drawTrace(const std::vector<IntrospectSession::Process>& processes) -> void;

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

    bool openTraceTab = false;
    std::vector<IntrospectSession::TraceEvent> incoming;
    std::vector<TraceRow> traceRows; // sorted by time
    std::vector<int> traceVisible;   // indices into traceRows that pass the filters
    bool traceVisibleStale = true;
    uint64_t traceOriginNs = 0; // the first event's time; the table's times count from it
    bool tracePaused = false;
    bool traceFollow = true;
    float traceFollowScrollY = -1.0f; // where following last put the scroll; -1 after the rows changed under it
    // Filters: an event shows when each non-empty filter is a substring of its field.
    std::array<char, 64> traceFilterProcess = {};
    std::array<char, 64> traceFilterCategory = {};
    std::array<char, 64> traceFilterType = {};
    std::array<char, 128> traceFilterText = {}; // name, text or fields
    int traceMinLevel = 0;                      // 0 everything, 1 warnings and errors, 2 errors
    std::shared_ptr<const rpc::Json> selectedEvent;
    std::string selectedEventProcess;
};
