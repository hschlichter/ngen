#include "profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

namespace profile {

namespace {

// One writer (the owning thread), any number of readers. The writer fills an
// entry then publishes by advancing `head`; readers copy entries below `head`.
// Entries older than ringSize are overwritten; the window only looks at the last
// frame, so a torn entry can only appear far in the past.
struct Lane {
    std::atomic<bool> used = false;
    std::string name;
    std::unique_ptr<std::array<Zone, ringSize>> ring; // allocated on first use; idle lanes cost nothing
    std::atomic<uint64_t> head = 0;
};

struct ThreadState {
    Lane* lane = nullptr;
    const char* pendingName = nullptr; // from registerThread, applied when the lane is taken
    uint32_t depth = 0;
    std::array<uint32_t, maxDepth> nameIds;
    std::array<uint64_t, maxDepth> starts;
    std::array<uint64_t, maxDepth> values;
    std::array<bool, maxDepth> hasValues;
};

std::array<Lane, maxLanes> g_lanes;
std::atomic<uint32_t> g_laneCount = 0;
std::atomic<bool> g_paused = false;
thread_local ThreadState t_state;

std::mutex g_nameMutex;
std::vector<std::string> g_names;

std::mutex g_frameMutex;
std::array<FrameStats, frameHistorySize> g_frames;
std::array<FrameInterval, frameHistorySize> g_intervals;
uint64_t g_frameCount = 0;
uint64_t g_frameStartNs = 0;

struct GpuFrame {
    uint64_t frameIndex = 0;
    uint64_t submitNs = 0;
    uint32_t count = 0;
    std::array<Zone, maxGpuZonesPerFrame> zones;
};

std::mutex g_gpuMutex;
std::array<GpuFrame, frameHistorySize> g_gpuFrames;
uint64_t g_gpuFrameCount = 0;
double g_gpuFrameMs = 0.0;

auto laneForThread() -> Lane* {
    if (t_state.lane != nullptr) {
        return t_state.lane;
    }
    auto index = g_laneCount.fetch_add(1);
    if (index >= maxLanes) {
        g_laneCount.store(maxLanes);
        return nullptr;
    }
    auto& lane = g_lanes[index];
    if (t_state.pendingName != nullptr) {
        lane.name = t_state.pendingName;
    } else {
        lane.name = "thread " + std::to_string(index);
    }
    lane.ring = std::make_unique<std::array<Zone, ringSize>>();
    lane.used.store(true, std::memory_order_release);
    t_state.lane = &lane;
    return &lane;
}

} // namespace

auto now() -> uint64_t {
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

auto registerName(const char* name) -> uint32_t {
    std::lock_guard lock(g_nameMutex);
    for (uint32_t i = 0; i < (uint32_t) g_names.size(); i++) {
        if (g_names[i] == name) {
            return i;
        }
    }
    g_names.emplace_back(name);
    return (uint32_t) g_names.size() - 1;
}

auto nameOf(uint32_t id) -> const char* {
    std::lock_guard lock(g_nameMutex);
    if (id < g_names.size()) {
        return g_names[id].c_str();
    }
    return "?";
}

auto registerThread(const char* name) -> void {
    t_state.pendingName = name;
    if (t_state.lane != nullptr) {
        t_state.lane->name = name;
    }
}

auto beginZone(uint32_t nameId) -> void {
    if (t_state.depth >= maxDepth) {
        t_state.depth++;
        return;
    }
    t_state.nameIds[t_state.depth] = nameId;
    t_state.starts[t_state.depth] = now();
    t_state.hasValues[t_state.depth] = false;
    t_state.depth++;
}

auto zoneValue(uint64_t value) -> void {
    if (t_state.depth == 0 || t_state.depth > maxDepth) {
        return;
    }
    t_state.values[t_state.depth - 1] = value;
    t_state.hasValues[t_state.depth - 1] = true;
}

auto endZone() -> void {
    if (t_state.depth == 0) {
        return;
    }
    t_state.depth--;
    if (t_state.depth >= maxDepth || g_paused.load(std::memory_order_relaxed)) {
        return;
    }
    auto* lane = laneForThread();
    if (lane == nullptr) {
        return;
    }
    auto head = lane->head.load(std::memory_order_relaxed);
    auto& entry = (*lane->ring)[head % ringSize];
    entry.nameId = t_state.nameIds[t_state.depth];
    entry.depth = (uint16_t) t_state.depth;
    entry.hasValue = t_state.hasValues[t_state.depth];
    entry.value = t_state.values[t_state.depth];
    entry.startNs = t_state.starts[t_state.depth];
    entry.endNs = now();
    lane->head.store(head + 1, std::memory_order_release);
}

auto frameMark() -> void {
    if (g_paused.load(std::memory_order_relaxed)) {
        return;
    }
    auto t = now();
    std::lock_guard lock(g_frameMutex);
    if (g_frameStartNs != 0) {
        auto slot = g_frameCount % frameHistorySize;
        double gpuMs = 0.0;
        {
            std::lock_guard gpuLock(g_gpuMutex);
            gpuMs = g_gpuFrameMs;
        }
        g_intervals[slot] = {.frameIndex = g_frameCount, .startNs = g_frameStartNs, .endNs = t};
        g_frames[slot] = {.frameIndex = g_frameCount, .cpuMs = (double) (t - g_frameStartNs) * 1e-6, .gpuMs = gpuMs};
        g_frameCount++;
    }
    g_frameStartNs = t;
}

auto setPaused(bool paused) -> void {
    auto was = g_paused.exchange(paused);
    if (was && !paused) {
        // The frame in progress spanned the pause; start the next interval fresh.
        std::lock_guard lock(g_frameMutex);
        g_frameStartNs = 0;
    }
}

auto isPaused() -> bool {
    return g_paused.load(std::memory_order_relaxed);
}

auto submitGpuZones(uint64_t frameIndex, uint64_t submitNs, std::span<const Zone> zones) -> void {
    if (g_paused.load(std::memory_order_relaxed)) {
        return;
    }
    std::lock_guard lock(g_gpuMutex);
    auto& frame = g_gpuFrames[g_gpuFrameCount % frameHistorySize];
    frame.frameIndex = frameIndex;
    frame.submitNs = submitNs;
    frame.count = (uint32_t) std::min<size_t>(zones.size(), maxGpuZonesPerFrame);
    std::copy_n(zones.begin(), frame.count, frame.zones.begin());
    g_gpuFrameCount++;

    uint64_t minStart = UINT64_MAX;
    uint64_t maxEnd = 0;
    for (const auto& z : zones) {
        minStart = std::min(minStart, z.startNs);
        maxEnd = std::max(maxEnd, z.endNs);
    }
    g_gpuFrameMs = zones.empty() ? 0.0 : (double) (maxEnd - minStart) * 1e-6;
}

auto lanes(std::vector<LaneInfo>& out) -> void {
    out.clear();
    auto count = std::min(g_laneCount.load(), maxLanes);
    for (uint32_t i = 0; i < count; i++) {
        if (g_lanes[i].used.load(std::memory_order_acquire)) {
            out.push_back({.index = i, .name = g_lanes[i].name});
        }
    }
}

auto lastFrame() -> std::optional<FrameInterval> {
    return frameAt(0);
}

auto frameAt(uint64_t offset) -> std::optional<FrameInterval> {
    std::lock_guard lock(g_frameMutex);
    if (g_frameCount == 0 || offset >= g_frameCount || offset >= frameHistorySize) {
        return std::nullopt;
    }
    return g_intervals[(g_frameCount - 1 - offset) % frameHistorySize];
}

auto frameByIndex(uint64_t frameIndex) -> std::optional<FrameInterval> {
    std::lock_guard lock(g_frameMutex);
    if (frameIndex >= g_frameCount || g_frameCount - frameIndex > frameHistorySize) {
        return std::nullopt;
    }
    const auto& interval = g_intervals[frameIndex % frameHistorySize];
    if (interval.frameIndex != frameIndex) {
        return std::nullopt;
    }
    return interval;
}

auto zonesIn(uint32_t laneIndex, uint64_t startNs, uint64_t endNs, std::vector<Zone>& out) -> void {
    out.clear();
    if (laneIndex >= maxLanes) {
        return;
    }
    auto& lane = g_lanes[laneIndex];
    if (!lane.used.load(std::memory_order_acquire)) {
        return;
    }
    auto head = lane.head.load(std::memory_order_acquire);
    auto count = std::min<uint64_t>(head, ringSize);
    // Newest first; stop once entries end before the window (zones are appended in end order).
    for (uint64_t i = 0; i < count; i++) {
        const auto& z = (*lane.ring)[(head - 1 - i) % ringSize];
        if (z.endNs < startNs) {
            break;
        }
        if (z.startNs <= endNs) {
            out.push_back(z);
        }
    }
}

auto gpuZonesIn(uint64_t startNs, uint64_t endNs, std::vector<Zone>& out) -> void {
    out.clear();
    std::lock_guard lock(g_gpuMutex);
    auto count = std::min<uint64_t>(g_gpuFrameCount, frameHistorySize);
    for (uint64_t i = 0; i < count; i++) {
        const auto& frame = g_gpuFrames[(g_gpuFrameCount - 1 - i) % frameHistorySize];
        for (uint32_t z = 0; z < frame.count; z++) {
            const auto& zone = frame.zones[z];
            if (zone.endNs >= startNs && zone.startNs <= endNs) {
                out.push_back(zone);
            }
        }
    }
}

auto framesIn(uint64_t startNs, uint64_t endNs, std::vector<FrameInterval>& out) -> void {
    out.clear();
    std::lock_guard lock(g_frameMutex);
    auto count = std::min<uint64_t>(g_frameCount, frameHistorySize);
    for (uint64_t i = 0; i < count; i++) {
        const auto& interval = g_intervals[(g_frameCount - 1 - i) % frameHistorySize];
        if (interval.endNs < startNs) {
            break;
        }
        if (interval.startNs <= endNs) {
            out.push_back(interval);
        }
    }
    std::reverse(out.begin(), out.end());
}

auto frameHistory(std::vector<FrameStats>& out) -> void {
    out.clear();
    std::lock_guard lock(g_frameMutex);
    auto count = std::min<uint64_t>(g_frameCount, frameHistorySize);
    out.reserve(count);
    for (uint64_t i = 0; i < count; i++) {
        auto idx = g_frameCount - count + i;
        out.push_back(g_frames[idx % frameHistorySize]);
    }
}

} // namespace profile

namespace profile {

namespace {

auto statsFrom(std::vector<double>& samples) -> ZoneStats {
    ZoneStats stats;
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (auto v : samples) {
        sum += v;
    }
    stats.count = (uint32_t) samples.size();
    stats.minMs = samples.front();
    stats.maxMs = samples.back();
    stats.avgMs = sum / (double) samples.size();
    stats.p99Ms = samples[std::min(samples.size() - 1, (size_t) ((double) samples.size() * 0.99))];
    return stats;
}

} // namespace

auto zoneStats(uint32_t laneIndex, uint32_t nameId) -> ZoneStats {
    std::vector<double> samples;
    if (laneIndex == UINT32_MAX) {
        std::lock_guard lock(g_gpuMutex);
        auto count = std::min<uint64_t>(g_gpuFrameCount, frameHistorySize);
        for (uint64_t i = 0; i < count; i++) {
            const auto& frame = g_gpuFrames[(g_gpuFrameCount - 1 - i) % frameHistorySize];
            for (uint32_t z = 0; z < frame.count; z++) {
                if (frame.zones[z].nameId == nameId) {
                    samples.push_back((double) (frame.zones[z].endNs - frame.zones[z].startNs) * 1e-6);
                }
            }
        }
        return statsFrom(samples);
    }
    if (laneIndex >= maxLanes) {
        return {};
    }
    auto& lane = g_lanes[laneIndex];
    if (!lane.used.load(std::memory_order_acquire)) {
        return {};
    }
    auto head = lane.head.load(std::memory_order_acquire);
    auto count = std::min<uint64_t>(head, ringSize);
    for (uint64_t i = 0; i < count; i++) {
        const auto& z = (*lane.ring)[(head - 1 - i) % ringSize];
        if (z.nameId == nameId) {
            samples.push_back((double) (z.endNs - z.startNs) * 1e-6);
        }
    }
    return statsFrom(samples);
}

} // namespace profile
