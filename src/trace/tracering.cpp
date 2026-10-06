#include "tracering.h"

#include "trace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <mutex>
#include <print>
#include <sstream>
#include <thread>

namespace trace {

namespace {

// Producers wake waiting readers once per this many events; readers also wake on their own timeout.
constexpr uint64_t wakeEvery = 1024;

} // namespace

auto now() -> uint64_t {
    // steady_clock is CLOCK_MONOTONIC on Linux, counted from boot: the same in every process.
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

auto levelName(Level level) -> const char* {
    switch (level) {
        case Level::Info:
            return "info";
        case Level::Warning:
            return "warning";
        case Level::Error:
            return "error";
    }
    return "info";
}

auto printToConsole(const Event& event) -> void {
    static std::mutex consoleMutex;
    auto now = std::chrono::system_clock::now();
    auto seconds = std::chrono::system_clock::to_time_t(now);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local = {};
    localtime_r(&seconds, &local);
    auto message = event.text.empty() ? std::format("{} {}", event.type, event.name) : event.text;
    std::lock_guard lock(consoleMutex);
    switch (event.level) {
        case Level::Info:
            std::println(stdout, "{:02}:{:02}:{:02}.{:03}  {}", local.tm_hour, local.tm_min, local.tm_sec, millis, message);
            // Flushed at once, so a redirected log is current.
            std::fflush(stdout);
            break;
        case Level::Warning:
            std::println(stderr, "{:02}:{:02}:{:02}.{:03}  warning: {}", local.tm_hour, local.tm_min, local.tm_sec, millis, message);
            break;
        case Level::Error:
            std::println(stderr, "{:02}:{:02}:{:02}.{:03}  error: {}", local.tm_hour, local.tm_min, local.tm_sec, millis, message);
            break;
    }
}

Builder::~Builder() {
    printToConsole(event);
    ring().push(std::move(event));
}

auto threadLabel() -> const std::string& {
    thread_local std::string label = [] {
        std::ostringstream text;
        text << std::this_thread::get_id();
        return text.str();
    }();
    return label;
}

TraceRing::TraceRing() : slots(capacity) {
}

auto TraceRing::push(Event event) -> void {
    if (event.thread.empty()) {
        event.thread = threadLabel();
    }
    bool wake = false;
    {
        std::lock_guard lock(mutex);
        // Stamped under the lock, so times never decrease with seq.
        if (event.tsNs == 0) {
            event.tsNs = now();
        }
        event.seq = next;
        slots[next % capacity] = std::move(event);
        next++;
        wake = next % wakeEvery == 0;
    }
    if (wake) {
        grew.notify_all();
    }
}

auto TraceRing::read(uint64_t from, size_t max, std::vector<Event>& out) const -> Read {
    std::lock_guard lock(mutex);
    uint64_t first = next > capacity ? next - capacity : 1;
    Read result;
    if (from < first) {
        result.dropped = first - from;
        from = first;
    }
    uint64_t end = std::min<uint64_t>(next, from + max);
    for (uint64_t seq = from; seq < end; seq++) {
        out.push_back(slots[seq % capacity]);
    }
    result.next = end;
    return result;
}

auto TraceRing::head() const -> uint64_t {
    std::lock_guard lock(mutex);
    return next;
}

auto TraceRing::oldest() const -> uint64_t {
    std::lock_guard lock(mutex);
    return next > capacity ? next - capacity : 1;
}

auto TraceRing::firstAtOrAfter(uint64_t tsNs) const -> uint64_t {
    std::lock_guard lock(mutex);
    uint64_t first = next > capacity ? next - capacity : 1;
    // Times rise with seq, so search.
    uint64_t low = first;
    uint64_t high = next;
    while (low < high) {
        uint64_t middle = low + (high - low) / 2;
        if (slots[middle % capacity].tsNs < tsNs) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

auto TraceRing::waitFor(uint64_t seq, std::chrono::milliseconds timeout) const -> void {
    std::unique_lock lock(mutex);
    grew.wait_for(lock, timeout, [&] { return next >= seq; });
}

auto ring() -> TraceRing& {
    static TraceRing instance;
    return instance;
}

} // namespace trace
