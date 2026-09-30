#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <format>
#include <mutex>
#include <print>
#include <string>

// The asset server's trace: one line per event on stdout, prefixed with the local time, flushed at once so a
// redirected log is current. Thread-safe.

inline std::mutex traceMutex;

template <typename... Args>
auto trace(std::format_string<Args...> format, Args&&... args) -> void {
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local = {};
    localtime_r(&seconds, &local);
    auto message = std::format(format, std::forward<Args>(args)...);
    std::lock_guard lock(traceMutex);
    std::println("{:02}:{:02}:{:02}.{:03}  {}", local.tm_hour, local.tm_min, local.tm_sec, millis, message);
    std::fflush(stdout);
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
