#pragma once

#include "trace.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

// The asset server's log: one trace event per line (category Asset, type Message, the line as its text), which the
// trace prints on the console with the local time and streams like any process's trace, so ngen-introspect shows it
// next to the view's events. traceWarning marks a request that failed: the asset could not be given, the server
// itself goes on.

template <typename... Args>
auto traceLine(std::format_string<Args...> format, Args&&... args) -> void {
    TRACE_EVENT("Asset", "Message", "asset-server").text(std::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
auto traceWarning(std::format_string<Args...> format, Args&&... args) -> void {
    TRACE_WARNING("Asset", "Message", "asset-server").text(std::format(format, std::forward<Args>(args)...));
}

// A byte count for people: "812 B", "10.2 KiB", "3.4 MiB".
inline auto bytesText(uint64_t bytes) -> std::string {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    if (bytes < 1024 * 1024) {
        return std::format("{:.1f} KiB", (double) bytes / 1024.0);
    }
    return std::format("{:.1f} MiB", (double) bytes / (1024.0 * 1024.0));
}

// Milliseconds since `start`.
inline auto millisSince(std::chrono::steady_clock::time_point start) -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}
