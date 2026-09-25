#pragma once

#include <cstdint>
#include <cstdio>

// Human-readable byte counts for the introspection windows.
inline auto formatByteCount(uint64_t bytes, char* out, size_t size) -> const char* {
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        std::snprintf(out, size, "%.2f GiB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024ull) {
        std::snprintf(out, size, "%.2f MiB", (double) bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        std::snprintf(out, size, "%.1f KiB", (double) bytes / 1024.0);
    } else {
        std::snprintf(out, size, "%llu B", (unsigned long long) bytes);
    }
    return out;
}
