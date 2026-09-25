#pragma once

#include "renderdebug.h"

#include <optional>
#include <string>

// Persistent UI state of the Memory window.
struct MemoryWindowState {
    char filter[64] = "";
    bool groupByCategory = true;
};

// GPU memory: heaps with budget, totals per memory usage and category, and every live
// allocation (RhiDevice::allocations), from the latest render debug snapshot.
auto drawMemoryWindow(bool& show, const std::optional<RenderDebugSnapshot>& snap, MemoryWindowState& state) -> void;

// Category of an allocation: its name up to the first '.' or ':'.
auto allocationCategory(const std::string& name) -> std::string;
