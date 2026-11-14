#include "stallwatch.h"

#include "profile.h"
#include "trace.h"

#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#endif

namespace profile {

namespace {

constexpr std::chrono::milliseconds checkInterval{250};

// A lane's stall: it begins when a zone has been open longer than the threshold, and ends when that zone, the outermost of those over it,
// ends. Going deeper inside it is the same stall.
struct Reported {
    uint32_t siteId = 0;
    uint64_t startNs = 0;
    StallInfo info;
};

std::mutex g_mutex;
std::map<uint32_t, Reported> g_reported; // by lane index
uint64_t g_reportedCount = 0;
std::jthread g_thread;

auto seconds(uint64_t fromNs, uint64_t toNs) -> double {
    if (toNs <= fromNs) {
        return 0.0;
    }
    return (double) (toNs - fromNs) * 1e-9;
}

auto stackText(const OpenZones& stack, uint64_t nowNs) -> std::string {
    std::string text;
    for (const auto& zone : stack.zones) {
        if (!text.empty()) {
            text += " > ";
        }
        text += std::format("{} {:.1f} s", nameOf(zone.siteId), seconds(zone.startNs, nowNs));
    }
    return text;
}

auto check() -> void {
    auto thresholdNs = (uint64_t) (stallSeconds() * 1e9);
    std::vector<OpenZones> stacks;
    openZones(stacks);
    auto nowNs = now();
    std::lock_guard lock(g_mutex);
    std::map<uint32_t, Reported> still;
    for (const auto& stack : stacks) {
        // Zones over the threshold: the outermost is where the stall began, the innermost is where the thread is stuck, with whatever it
        // does inside.
        const OpenZone* outermost = nullptr;
        const OpenZone* stuck = nullptr;
        if (thresholdNs > 0) {
            for (const auto& zone : stack.zones) {
                if (nowNs > zone.startNs && nowNs - zone.startNs >= thresholdNs) {
                    if (outermost == nullptr) {
                        outermost = &zone;
                    }
                    stuck = &zone;
                }
            }
        }
        auto previous = g_reported.find(stack.laneIndex);
        bool same = previous != g_reported.end() && outermost != nullptr && previous->second.siteId == outermost->siteId &&
                    previous->second.startNs == outermost->startNs;
        if (previous != g_reported.end() && !same) {
            const auto& info = previous->second.info;
            auto outerName = nameOf(previous->second.siteId);
            TRACE_EVENT("Profile", "StallEnded", info.lane)
                .text(std::format("{} left {} after about {:.1f} s", info.lane, outerName, seconds(previous->second.startNs, nowNs)))
                .field("lane", info.lane)
                .field("zone", outerName)
                .field("seconds", seconds(previous->second.startNs, nowNs));
        }
        if (stuck == nullptr) {
            continue;
        }
        Reported entry;
        entry.siteId = outermost->siteId;
        entry.startNs = outermost->startNs;
        entry.info.lane = stack.lane;
        entry.info.zone = nameOf(stuck->siteId);
        entry.info.stack = stackText(stack, nowNs);
        entry.info.seconds = seconds(stuck->startNs, nowNs);
        if (!same) {
            auto site = siteOf(stuck->siteId);
            TRACE_WARNING("Profile", "Stall", stack.lane)
                .text(std::format("{} has been in {} for {:.1f} s ({}:{}); open zones, outermost first: {}", stack.lane, entry.info.zone, entry.info.seconds, site.file, site.line, entry.info.stack))
                .field("lane", stack.lane)
                .field("zone", entry.info.zone)
                .field("file", site.file)
                .field("line", site.line)
                .field("seconds", entry.info.seconds)
                .field("stack", entry.info.stack);
            g_reportedCount++;
        }
        still[stack.laneIndex] = std::move(entry);
    }
    g_reported = std::move(still);
}

} // namespace

auto startStallWatch() -> void {
    if (g_thread.joinable()) {
        return;
    }
    g_thread = std::jthread([](std::stop_token stopToken) {
#ifdef __linux__
        pthread_setname_np(pthread_self(), "StallWatch");
#endif
        std::mutex sleepMutex;
        std::condition_variable_any sleep;
        while (!stopToken.stop_requested()) {
            check();
            std::unique_lock lock(sleepMutex);
            sleep.wait_for(lock, stopToken, checkInterval, [] { return false; });
        }
    });
}

auto stopStallWatch() -> void {
    if (!g_thread.joinable()) {
        return;
    }
    g_thread.request_stop();
    g_thread.join();
}

auto stallWatchState() -> StallWatchState {
    StallWatchState state;
    state.running = g_thread.joinable();
    std::lock_guard lock(g_mutex);
    state.reported = g_reportedCount;
    for (const auto& [lane, entry] : g_reported) {
        state.current.push_back(entry.info);
    }
    return state;
}

} // namespace profile
