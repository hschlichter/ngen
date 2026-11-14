#include "profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string_view>
#include <thread>
#include <unordered_map>

#ifdef TRACY_ENABLE
#include "common/TracyQueue.hpp"
#include "tracy/TracyC.h"
#endif

namespace profile {

namespace {

// A lane's zones live in blocks. The owning thread appends to the newest block without a lock and publishes each entry by
// advancing `count`; readers copy entries below it. A block leaves the lane once its newest zone is older than the history
// window, checked when the writer starts the next block.
constexpr uint32_t blockZones = 4096;

struct ZoneBlock {
    std::array<Zone, blockZones> zones;
    std::atomic<uint32_t> count = 0;
    std::atomic<uint64_t> newestEndNs = 0;
};

struct Lane {
    std::atomic<bool> used = false;
    // A thread writes the lane. When it exits the lane is free, and a thread registering the same name takes it over: threads started
    // again and again (a pool per call) keep one lane per name instead of using up maxLanes.
    std::atomic<bool> owned = false;
    // Taken by the writer when it starts a block, by readers to copy the block list, and around the name.
    std::mutex mutex;
    std::string name;
    std::vector<std::shared_ptr<ZoneBlock>> blocks; // oldest first; the last is the one being written
    ZoneBlock* current = nullptr;                   // the owning thread only
    // The owning thread's open zones, outermost first, published as they begin and end so another thread can see where this one is
    // (openZones, the stall watch). A reader may catch an entry while it is rewritten; a thread that stays in one zone reads exactly.
    std::array<std::atomic<uint32_t>, maxDepth> openSites;
    std::array<std::atomic<uint64_t>, maxDepth> openStarts;
    std::atomic<uint32_t> openDepth = 0;
};

struct ThreadState {
    Lane* lane = nullptr;
    const char* pendingName = nullptr; // from registerThread, applied when the lane is taken
    uint32_t depth = 0;
    std::array<uint32_t, maxDepth> siteIds;
    std::array<uint64_t, maxDepth> starts;
    std::array<uint64_t, maxDepth> values;
    std::array<bool, maxDepth> hasValues;
#ifdef TRACY_ENABLE
    std::array<TracyCZoneCtx, maxDepth> tracyZones;
#endif

    ThreadState() = default;
    ThreadState(const ThreadState&) = delete;
    auto operator=(const ThreadState&) -> ThreadState& = delete;
    ~ThreadState() {
        if (lane != nullptr) {
            lane->openDepth.store(0, std::memory_order_release);
            lane->owned.store(false, std::memory_order_release);
        }
    }
};

std::array<Lane, maxLanes> g_lanes;
std::atomic<uint32_t> g_laneCount = 0;
std::atomic<bool> g_paused = false;
std::atomic<uint64_t> g_historyNs = 10'000'000'000ull;
std::atomic<bool> g_gpuDetail = false;
std::atomic<uint64_t> g_stallNs = 5'000'000'000ull;
thread_local ThreadState t_state;

// Sites: one per (file, line, name). The name is copied (a deque keeps every string where it is); the location's strings are static.
// A site's info is written before the count that publishes its id, so readers and beginZone read it without the lock. Id 0 is no site.
std::mutex g_siteMutex;
std::deque<std::string> g_siteNames;
std::unordered_map<std::string, uint32_t> g_siteIds; // "file:line:name"
std::array<SiteInfo, maxNames + 1> g_sites;
std::atomic<uint32_t> g_siteCount = 1;

std::mutex g_frameMutex;
std::deque<FrameInterval> g_intervals;
std::deque<FrameStats> g_frames;
uint64_t g_frameCount = 0;
uint64_t g_frameStartNs = 0;
// Tracy's frame count and ours move together under this lock, so a frame image's offset, taken on another thread, is exact: Tracy fails
// a capture that gets two images for one frame.
std::mutex g_frameMarkMutex;
uint64_t g_frameMarks = 0;

struct GpuFrame {
    uint64_t frameIndex = 0;
    uint64_t submitNs = 0;
    uint64_t newestEndNs = 0;
    std::vector<Zone> zones;
};

std::mutex g_gpuMutex;
std::deque<GpuFrame> g_gpuFrames;
double g_gpuFrameMs = 0.0;
// The portable GPU busy figure: the last GPU frame's first zone start, and the share of the time since the frame before's that it was busy.
uint64_t g_lastGpuFrameStartNs = 0;
std::atomic<double> g_gpuBusyPercent = 0.0;

// Plots, in a fixed table so a sample finds its plot without the table's lock. A plot's samples are kept for the history window.
constexpr uint32_t maxPlots = 512;
struct Plot {
    std::string name;
    Group group = Group::Render;
    PlotFormat format = PlotFormat::Number;
    std::mutex mutex;
    std::deque<PlotSample> samples;
    std::atomic<bool> tracyConfigured = false;
};
std::mutex g_plotMutex;
std::array<std::unique_ptr<Plot>, maxPlots> g_plots;
std::unordered_map<std::string, uint32_t> g_plotIds;
std::atomic<uint32_t> g_plotCount = 0;

constexpr std::array<uint32_t, groupCount> groupColors = {
    0x3F7FD8, // Frame: blue
    0x26A69A, // Scene: teal
    0xF0A030, // Assets: orange
    0x8E5BD9, // Jobs: purple
    0xD05FA0, // Ui: magenta
    0x8DB040, // Tools: olive
    0x4DB6E8, // Render: light blue
    0xE0D040, // Culling: yellow
    0x50B050, // Geometry: green
    0x6A6AD0, // Shadow: indigo
    0xF07850, // Lighting: coral
    0xC09060, // Post: tan
    0xD04848, // Upload: red
    0x40A0A0, // Io: dark cyan
    0x808080, // Wait: grey
};

constexpr std::array<const char*, groupCount> groupNames = {
    "Frame",
    "Scene",
    "Assets",
    "Jobs",
    "Ui",
    "Tools",
    "Render",
    "Culling",
    "Geometry",
    "Shadow",
    "Lighting",
    "Post",
    "Upload",
    "Io",
    "Wait",
};

auto shade(uint32_t color, int step) -> uint32_t {
    uint32_t out = 0;
    for (uint32_t shift = 0; shift <= 16; shift += 8) {
        int channel = (int) ((color >> shift) & 0xFF) + step * 0x1C;
        channel = std::clamp(channel, 0, 255);
        out |= (uint32_t) channel << shift;
    }
    return out;
}

auto windowStart(uint64_t nowNs) -> uint64_t {
    auto window = g_historyNs.load(std::memory_order_relaxed);
    if (nowNs < window) {
        return 0;
    }
    return nowNs - window;
}

#ifdef TRACY_ENABLE
// Tracy's client runs from startTracy or startTracyIfAsked until exit. Every call into it checks this first.
std::atomic<bool> g_tracyRunning = false;
std::mutex g_tracyLifetimeMutex;
bool g_tracyRequested = false;
std::thread g_tracyStarter;

auto tracyRunning() -> bool {
    return g_tracyRunning.load(std::memory_order_acquire);
}

auto startTracyHere() -> void {
    ___tracy_startup_profiler();
    g_tracyRunning.store(true, std::memory_order_release);
}

// At exit, after main has returned and its threads are joined: the client flushes to a connected server and stops.
auto stopTracyAtExit() -> void {
    std::lock_guard lock(g_tracyLifetimeMutex);
    if (g_tracyStarter.joinable()) {
        g_tracyStarter.join();
    }
    if (g_tracyRunning.exchange(false)) {
        ___tracy_shutdown_profiler();
    }
}

// Tracy, the second sink (src/profile/README.md). One source location per interned name, written before the id is published,
// so beginZone reads it without a lock; its strings are the interned copies, which never move.
std::array<___tracy_source_location_data, maxNames + 1> g_tracySourceLocations;

// The GPU lane's Tracy context. Touched only by the thread that submits GPU zones (the render thread).
struct TracyGpu {
    bool created = false;
    uint16_t nextQueryId = 0;
};
TracyGpu g_tracyGpu;
constexpr uint8_t tracyGpuContext = 0;

// The GPU zones of one frame, already on the profile clock, as Tracy GPU zones on a context whose clock is the profile clock.
// Begins and ends are sent in nesting order, each followed by its time. Tracy records the CPU side of each zone now, after
// the frame's fence, not when the commands were recorded.
auto sendGpuZonesToTracy(std::span<const Zone> zones) -> void {
    if (!g_tracyGpu.created) {
        ___tracy_emit_gpu_new_context_serial({
            .gpuTime = (int64_t) now(),
            .period = 1.0f,
            .context = tracyGpuContext,
            .flags = 0,
            .type = (uint8_t) tracy::GpuContextType::Custom,
        });
        constexpr const char* gpuName = "GPU";
        ___tracy_emit_gpu_context_name_serial({.context = tracyGpuContext, .name = gpuName, .len = (uint16_t) std::strlen(gpuName)});
        g_tracyGpu.created = true;
    }
    if (___tracy_connected() == 0) {
        return;
    }
    // Re-anchor the context on the profile clock (CLOCK_MONOTONIC) against Tracy's own every frame, so the two never drift apart.
    ___tracy_emit_gpu_time_sync_serial({.gpuTime = (int64_t) now(), .context = tracyGpuContext});

    struct Open {
        uint16_t endQueryId = 0;
        uint64_t endNs = 0;
    };
    std::array<Open, maxDepth> open;
    uint32_t openCount = 0;
    auto closeTop = [&]() {
        openCount--;
        ___tracy_emit_gpu_zone_end_serial({.queryId = open[openCount].endQueryId, .context = tracyGpuContext});
        ___tracy_emit_gpu_time_serial({.gpuTime = (int64_t) open[openCount].endNs, .queryId = open[openCount].endQueryId, .context = tracyGpuContext});
    };
    for (const auto& zone : zones) {
        if (zone.depth >= maxDepth) {
            continue;
        }
        while (openCount > zone.depth) {
            closeTop();
        }
        auto beginQueryId = g_tracyGpu.nextQueryId;
        g_tracyGpu.nextQueryId += 2;
        auto site = siteOf(zone.siteId);
        auto color = groupColor(site.group, zone.siteId);
        auto sourceLocation = ___tracy_alloc_srcloc_name(site.line,
                                                         site.file,
                                                         std::strlen(site.file),
                                                         site.function,
                                                         std::strlen(site.function),
                                                         site.name,
                                                         std::strlen(site.name),
                                                         color);
        ___tracy_emit_gpu_zone_begin_alloc_serial({.srcloc = sourceLocation, .queryId = beginQueryId, .context = tracyGpuContext});
        ___tracy_emit_gpu_time_serial({.gpuTime = (int64_t) zone.startNs, .queryId = beginQueryId, .context = tracyGpuContext});
        open[openCount] = {.endQueryId = (uint16_t) (beginQueryId + 1), .endNs = zone.endNs};
        openCount++;
    }
    while (openCount > 0) {
        closeTop();
    }
}
#endif

// The zones the thread opened before it had a lane, published when it takes one.
auto publishOpenZones(Lane& lane) -> void {
    auto depth = std::min(t_state.depth, maxDepth);
    for (uint32_t i = 0; i < depth; i++) {
        lane.openSites[i].store(t_state.siteIds[i], std::memory_order_relaxed);
        lane.openStarts[i].store(t_state.starts[i], std::memory_order_relaxed);
    }
    lane.openDepth.store(depth, std::memory_order_release);
}

auto laneForThread() -> Lane* {
    if (t_state.lane != nullptr) {
        return t_state.lane;
    }
    if (t_state.pendingName != nullptr) {
        auto count = std::min(g_laneCount.load(), maxLanes);
        for (uint32_t i = 0; i < count; i++) {
            auto& lane = g_lanes[i];
            if (!lane.used.load(std::memory_order_acquire) || lane.owned.load(std::memory_order_acquire)) {
                continue;
            }
            {
                std::lock_guard lock(lane.mutex);
                if (lane.name != t_state.pendingName) {
                    continue;
                }
            }
            bool expected = false;
            if (lane.owned.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                t_state.lane = &lane;
                publishOpenZones(lane);
                return &lane;
            }
        }
    }
    auto index = g_laneCount.fetch_add(1);
    if (index >= maxLanes) {
        g_laneCount.store(maxLanes);
        return nullptr;
    }
    auto& lane = g_lanes[index];
    {
        std::lock_guard lock(lane.mutex);
        if (t_state.pendingName != nullptr) {
            lane.name = t_state.pendingName;
        } else {
            lane.name = "thread " + std::to_string(index);
        }
    }
    lane.owned.store(true, std::memory_order_relaxed);
    publishOpenZones(lane);
    lane.used.store(true, std::memory_order_release);
    t_state.lane = &lane;
    return &lane;
}

// The writer's block is full (or the lane has none yet): blocks older than the window leave, one no reader holds is reused.
auto startBlock(Lane& lane, uint64_t nowNs) -> ZoneBlock* {
    std::lock_guard lock(lane.mutex);
    auto oldest = windowStart(nowNs);
    std::shared_ptr<ZoneBlock> reuse;
    while (!lane.blocks.empty() && lane.blocks.front()->newestEndNs.load(std::memory_order_relaxed) < oldest) {
        auto old = std::move(lane.blocks.front());
        lane.blocks.erase(lane.blocks.begin());
        if (reuse == nullptr && old.use_count() == 1) {
            reuse = std::move(old);
        }
    }
    if (reuse != nullptr) {
        reuse->count.store(0, std::memory_order_relaxed);
        reuse->newestEndNs.store(0, std::memory_order_relaxed);
    } else {
        reuse = std::make_shared<ZoneBlock>();
    }
    lane.current = reuse.get();
    lane.blocks.push_back(std::move(reuse));
    return lane.current;
}

auto blocksOf(uint32_t laneIndex) -> std::vector<std::shared_ptr<ZoneBlock>> {
    if (laneIndex >= maxLanes) {
        return {};
    }
    auto& lane = g_lanes[laneIndex];
    if (!lane.used.load(std::memory_order_acquire)) {
        return {};
    }
    std::lock_guard lock(lane.mutex);
    return lane.blocks;
}

} // namespace

auto now() -> uint64_t {
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

auto startTracy() -> void {
#ifdef TRACY_ENABLE
    std::lock_guard lock(g_tracyLifetimeMutex);
    if (g_tracyRequested) {
        return;
    }
    g_tracyRequested = true;
    std::atexit(stopTracyAtExit);
    g_tracyStarter = std::thread(startTracyHere);
#endif
}

auto startTracyIfAsked() -> void {
#ifdef TRACY_ENABLE
    const char* asked = std::getenv("NGEN_TRACY");
    if (asked == nullptr || std::string_view(asked) != "1") {
        return;
    }
    std::lock_guard lock(g_tracyLifetimeMutex);
    if (g_tracyRequested) {
        return;
    }
    g_tracyRequested = true;
    std::atexit(stopTracyAtExit);
    startTracyHere();
#endif
}

namespace {

auto addSite(const std::string& key, const char* name, Group group, Level level, const char* file, const char* function, uint32_t line) -> uint32_t {
    std::lock_guard lock(g_siteMutex);
    if (auto found = g_siteIds.find(key); found != g_siteIds.end()) {
        return found->second;
    }
    auto id = g_siteCount.load(std::memory_order_relaxed);
    assert(id <= maxNames);
    if (id > maxNames) {
        return 0;
    }
    const auto& stored = g_siteNames.emplace_back(name);
    g_siteIds.emplace(key, id);
    g_sites[id] = {.name = stored.c_str(), .file = file, .function = function, .line = line, .group = group, .level = level};
#ifdef TRACY_ENABLE
    g_tracySourceLocations[id] = {.name = stored.c_str(), .function = function, .file = file, .line = line, .color = groupColor(group, id)};
#endif
    g_siteCount.store(id + 1, std::memory_order_release);
    return id;
}

} // namespace

auto registerSite(const char* name, Group group, Level level, std::source_location location) -> uint32_t {
    // Built in a reused buffer: GPU zones register every frame.
    thread_local std::string key;
    key.assign(location.file_name());
    key += ':';
    key += std::to_string(location.line());
    key += ':';
    key += name;
    return addSite(key, name, group, level, location.file_name(), location.function_name(), location.line());
}

auto siteIdOf(const char* name) -> uint32_t {
    thread_local std::string key;
    key.assign("::");
    key += name;
    return addSite(key, name, Group::Render, Level::Guide, "", "", 0);
}

auto siteOf(uint32_t siteId) -> SiteInfo {
    if (siteId != 0 && siteId < g_siteCount.load(std::memory_order_acquire)) {
        return g_sites[siteId];
    }
    return {};
}

auto nameOf(uint32_t siteId) -> const char* {
    if (siteId != 0 && siteId < g_siteCount.load(std::memory_order_acquire)) {
        return g_sites[siteId].name;
    }
    return "?";
}

auto groupOf(uint32_t siteId) -> Group {
    if (siteId != 0 && siteId < g_siteCount.load(std::memory_order_acquire)) {
        return g_sites[siteId].group;
    }
    return Group::Render;
}

auto levelOf(uint32_t siteId) -> Level {
    if (siteId != 0 && siteId < g_siteCount.load(std::memory_order_acquire)) {
        return g_sites[siteId].level;
    }
    return Level::Guide;
}

auto groupName(Group group) -> const char* {
    auto index = (uint32_t) group;
    if (index < groupCount) {
        return groupNames[index];
    }
    return "?";
}

auto groupColor(Group group, uint32_t siteId) -> uint32_t {
    auto index = (uint32_t) group;
    if (index >= groupCount) {
        return 0x808080;
    }
    auto color = groupColors[index];
    if (group == Group::Jobs) {
        // Five shades, darker to lighter, picked by the site.
        auto step = (int) ((siteId * 2654435761u) >> 16) % 5 - 2;
        color = shade(color, step);
    }
    return color;
}

auto registerThread(const char* name) -> void {
#ifdef TRACY_ENABLE
    if (tracyRunning()) {
        ___tracy_set_thread_name(name);
    }
#endif
#ifdef __linux__
    // The thread's name in the system (top, gdb), at most 15 characters. Tracy reads it for a thread named before its client
    // started, which keeps names only from then on.
    std::array<char, 16> systemName = {};
    std::strncpy(systemName.data(), name, systemName.size() - 1);
    pthread_setname_np(pthread_self(), systemName.data());
#endif
    t_state.pendingName = name;
    if (t_state.lane != nullptr) {
        std::lock_guard lock(t_state.lane->mutex);
        t_state.lane->name = name;
    } else {
        // Take the lane now so a named thread shows up in the window and in dumps even before its first zone (workers idle until
        // the first job).
        laneForThread();
    }
    static const uint32_t startedId = registerSite("ThreadStarted", Group::Wait, Level::Detail);
    beginZone(startedId);
    endZone();
}

auto beginZone(uint32_t siteId) -> void {
    if (t_state.depth >= maxDepth) {
        t_state.depth++;
        return;
    }
    auto startNs = now();
    t_state.siteIds[t_state.depth] = siteId;
    t_state.starts[t_state.depth] = startNs;
    t_state.hasValues[t_state.depth] = false;
    if (t_state.lane != nullptr) {
        t_state.lane->openSites[t_state.depth].store(siteId, std::memory_order_relaxed);
        t_state.lane->openStarts[t_state.depth].store(startNs, std::memory_order_relaxed);
        t_state.lane->openDepth.store(t_state.depth + 1, std::memory_order_release);
    }
#ifdef TRACY_ENABLE
    if (siteId != 0 && siteId <= maxNames && tracyRunning()) {
        t_state.tracyZones[t_state.depth] = ___tracy_emit_zone_begin(&g_tracySourceLocations[siteId], 1);
    } else {
        t_state.tracyZones[t_state.depth] = {};
    }
#endif
    t_state.depth++;
}

auto zoneValue(uint64_t value) -> void {
    if (t_state.depth == 0 || t_state.depth > maxDepth) {
        return;
    }
    t_state.values[t_state.depth - 1] = value;
    t_state.hasValues[t_state.depth - 1] = true;
#ifdef TRACY_ENABLE
    if (tracyRunning()) {
        ___tracy_emit_zone_value(t_state.tracyZones[t_state.depth - 1], value);
    }
#endif
}

auto endZone() -> void {
    if (t_state.depth == 0) {
        return;
    }
    t_state.depth--;
    if (t_state.lane != nullptr && t_state.depth < maxDepth) {
        t_state.lane->openDepth.store(t_state.depth, std::memory_order_release);
    }
#ifdef TRACY_ENABLE
    // Before the pause check: pausing the Performance window does not pause Tracy. A zone begun before Tracy started is inactive.
    if (t_state.depth < maxDepth && tracyRunning()) {
        ___tracy_emit_zone_end(t_state.tracyZones[t_state.depth]);
    }
#endif
    if (t_state.depth >= maxDepth || g_paused.load(std::memory_order_relaxed)) {
        return;
    }
    auto* lane = laneForThread();
    if (lane == nullptr) {
        return;
    }
    auto endNs = now();
    auto* block = lane->current;
    if (block == nullptr || block->count.load(std::memory_order_relaxed) == blockZones) {
        block = startBlock(*lane, endNs);
    }
    auto index = block->count.load(std::memory_order_relaxed);
    auto& entry = block->zones[index];
    entry.siteId = t_state.siteIds[t_state.depth];
    entry.depth = (uint16_t) t_state.depth;
    entry.hasValue = t_state.hasValues[t_state.depth];
    entry.value = t_state.values[t_state.depth];
    entry.startNs = t_state.starts[t_state.depth];
    entry.endNs = endNs;
    block->newestEndNs.store(endNs, std::memory_order_relaxed);
    block->count.store(index + 1, std::memory_order_release);
}

auto frameMark() -> void {
    {
        std::lock_guard markLock(g_frameMarkMutex);
#ifdef TRACY_ENABLE
        if (tracyRunning()) {
            ___tracy_emit_frame_mark(nullptr);
        }
#endif
        g_frameMarks++;
    }
    if (g_paused.load(std::memory_order_relaxed)) {
        return;
    }
    auto t = now();
    double gpuMs = 0.0;
    {
        std::lock_guard gpuLock(g_gpuMutex);
        gpuMs = g_gpuFrameMs;
    }
    std::lock_guard lock(g_frameMutex);
    if (g_frameStartNs != 0) {
        g_intervals.push_back({.frameIndex = g_frameCount, .startNs = g_frameStartNs, .endNs = t});
        g_frames.push_back({.frameIndex = g_frameCount, .cpuMs = (double) (t - g_frameStartNs) * 1e-6, .gpuMs = gpuMs});
        g_frameCount++;
        auto oldest = windowStart(t);
        while (!g_intervals.empty() && g_intervals.front().endNs < oldest) {
            g_intervals.pop_front();
            g_frames.pop_front();
        }
    }
    g_frameStartNs = t;
}

auto setHistorySeconds(double seconds) -> void {
    g_historyNs.store((uint64_t) (std::max(seconds, 0.0) * 1e9), std::memory_order_relaxed);
}

auto historySeconds() -> double {
    return (double) g_historyNs.load(std::memory_order_relaxed) * 1e-9;
}

auto setGpuDetail(bool enabled) -> void {
    g_gpuDetail.store(enabled, std::memory_order_relaxed);
}

auto gpuDetail() -> bool {
    return g_gpuDetail.load(std::memory_order_relaxed);
}

auto setStallSeconds(double seconds) -> void {
    g_stallNs.store((uint64_t) (std::max(seconds, 0.0) * 1e9), std::memory_order_relaxed);
}

auto stallSeconds() -> double {
    return (double) g_stallNs.load(std::memory_order_relaxed) * 1e-9;
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
#ifdef TRACY_ENABLE
    // Before the pause check.
    if (tracyRunning()) {
        sendGpuZonesToTracy(zones);
    }
#endif
    if (g_paused.load(std::memory_order_relaxed)) {
        return;
    }
    uint64_t minStart = UINT64_MAX;
    uint64_t maxEnd = 0;
    for (const auto& z : zones) {
        minStart = std::min(minStart, z.startNs);
        maxEnd = std::max(maxEnd, z.endNs);
    }
    // Busy: the union of the frame's top-level zones, over the time since the frame before started.
    uint64_t busyNs = 0;
    uint64_t coveredUntil = 0;
    for (const auto& z : zones) {
        if (z.depth != 0) {
            continue;
        }
        auto from = std::max(z.startNs, coveredUntil);
        if (z.endNs > from) {
            busyNs += z.endNs - from;
        }
        coveredUntil = std::max(coveredUntil, z.endNs);
    }
    if (!zones.empty() && g_lastGpuFrameStartNs != 0 && minStart > g_lastGpuFrameStartNs) {
        auto busy = std::min(100.0, (double) busyNs * 100.0 / (double) (minStart - g_lastGpuFrameStartNs));
        g_gpuBusyPercent.store(busy, std::memory_order_relaxed);
        static const uint32_t busyPlot = registerPlot("GPU busy", Group::Frame, PlotFormat::Percent);
        plot(busyPlot, busy);
    }
    if (!zones.empty()) {
        g_lastGpuFrameStartNs = minStart;
    }
    std::lock_guard lock(g_gpuMutex);
    g_gpuFrames.push_back({.frameIndex = frameIndex, .submitNs = submitNs, .newestEndNs = maxEnd, .zones = {zones.begin(), zones.end()}});
    auto oldest = windowStart(now());
    while (!g_gpuFrames.empty() && g_gpuFrames.front().newestEndNs < oldest) {
        g_gpuFrames.pop_front();
    }
    g_gpuFrameMs = zones.empty() ? 0.0 : (double) (maxEnd - minStart) * 1e-6;
}

auto tracyConnected() -> bool {
#ifdef TRACY_ENABLE
    return tracyRunning() && ___tracy_connected() != 0;
#else
    return false;
#endif
}

auto frameMarks() -> uint64_t {
    std::lock_guard lock(g_frameMarkMutex);
    return g_frameMarks;
}

auto frameImage(const uint8_t* rgba, uint16_t width, uint16_t height, uint64_t frame) -> void {
#ifdef TRACY_ENABLE
    if (!tracyConnected()) {
        return;
    }
    // Tracy places the image `offset` marks before its own count, which moves with ours.
    std::lock_guard lock(g_frameMarkMutex);
    if (frame > g_frameMarks || g_frameMarks - frame > 255) {
        return;
    }
    ___tracy_emit_frame_image(rgba, width, height, (uint8_t) (g_frameMarks - frame), 0);
#else
    (void) rgba;
    (void) width;
    (void) height;
    (void) frame;
#endif
}

auto registerPlot(const char* name, Group group, PlotFormat format) -> uint32_t {
    std::lock_guard lock(g_plotMutex);
    if (auto found = g_plotIds.find(name); found != g_plotIds.end()) {
        return found->second;
    }
    auto id = g_plotCount.load(std::memory_order_relaxed);
    assert(id < maxPlots);
    if (id >= maxPlots) {
        return maxPlots;
    }
    auto entry = std::make_unique<Plot>();
    entry->name = name;
    entry->group = group;
    entry->format = format;
    g_plots[id] = std::move(entry);
    g_plotIds.emplace(name, id);
    g_plotCount.store(id + 1, std::memory_order_release);
    return id;
}

auto plot(uint32_t plotId, double value) -> void {
    if (plotId >= g_plotCount.load(std::memory_order_acquire)) {
        return;
    }
    auto& entry = *g_plots[plotId];
#ifdef TRACY_ENABLE
    // Configured once Tracy runs; Tracy keeps the configuration for every server that connects later.
    if (tracyRunning()) {
        if (!entry.tracyConfigured.exchange(true)) {
            constexpr std::array<int32_t, 3> tracyFormats = {0, 1, 2}; // Number, Memory, Percentage
            ___tracy_emit_plot_config(entry.name.c_str(), tracyFormats[(uint32_t) entry.format], 0, 1, groupColor(entry.group, 0));
        }
        ___tracy_emit_plot(entry.name.c_str(), value);
    }
#endif
    if (g_paused.load(std::memory_order_relaxed)) {
        return;
    }
    auto t = now();
    std::lock_guard lock(entry.mutex);
    entry.samples.push_back({.timeNs = t, .value = value});
    auto oldest = windowStart(t);
    while (!entry.samples.empty() && entry.samples.front().timeNs < oldest) {
        entry.samples.pop_front();
    }
}

auto plotIdOf(const std::string& name) -> std::optional<uint32_t> {
    std::lock_guard lock(g_plotMutex);
    if (auto found = g_plotIds.find(name); found != g_plotIds.end()) {
        return found->second;
    }
    return std::nullopt;
}

auto plots(std::vector<PlotInfo>& out) -> void {
    out.clear();
    auto count = g_plotCount.load(std::memory_order_acquire);
    for (uint32_t id = 0; id < count; id++) {
        auto& entry = *g_plots[id];
        PlotInfo info = {.id = id, .name = entry.name, .group = entry.group, .format = entry.format};
        std::lock_guard lock(entry.mutex);
        info.samples = entry.samples.size();
        if (!entry.samples.empty()) {
            info.latest = entry.samples.back().value;
            info.min = entry.samples.front().value;
            info.max = entry.samples.front().value;
            double sum = 0.0;
            for (const auto& sample : entry.samples) {
                info.min = std::min(info.min, sample.value);
                info.max = std::max(info.max, sample.value);
                sum += sample.value;
            }
            info.average = sum / (double) entry.samples.size();
        }
        out.push_back(std::move(info));
    }
}

auto plotSamplesIn(uint32_t plotId, uint64_t startNs, uint64_t endNs, std::vector<PlotSample>& out) -> void {
    out.clear();
    if (plotId >= g_plotCount.load(std::memory_order_acquire)) {
        return;
    }
    auto& entry = *g_plots[plotId];
    std::lock_guard lock(entry.mutex);
    for (const auto& sample : entry.samples) {
        if (sample.timeNs >= startNs && sample.timeNs <= endNs) {
            out.push_back(sample);
        }
    }
}

auto gpuBusyPercent() -> double {
    return g_gpuBusyPercent.load(std::memory_order_relaxed);
}

auto memoryAlloc(const char* pool, uint64_t id, uint64_t bytes) -> void {
#ifdef TRACY_ENABLE
    if (tracyRunning()) {
        // The id stands for the address Tracy keys an allocation by.
        ___tracy_emit_memory_alloc_named(reinterpret_cast<const void*>(id), bytes, pool);
    }
#else
    (void) pool;
    (void) id;
    (void) bytes;
#endif
}

auto memoryFree(const char* pool, uint64_t id) -> void {
#ifdef TRACY_ENABLE
    if (tracyRunning()) {
        ___tracy_emit_memory_free_named(reinterpret_cast<const void*>(id), pool);
    }
#else
    (void) pool;
    (void) id;
#endif
}

auto lanes(std::vector<LaneInfo>& out) -> void {
    out.clear();
    auto count = std::min(g_laneCount.load(), maxLanes);
    for (uint32_t i = 0; i < count; i++) {
        auto& lane = g_lanes[i];
        if (!lane.used.load(std::memory_order_acquire)) {
            continue;
        }
        std::lock_guard lock(lane.mutex);
        out.push_back({.index = i, .name = lane.name});
    }
}

auto openZones(std::vector<OpenZones>& out) -> void {
    out.clear();
    auto count = std::min(g_laneCount.load(), maxLanes);
    for (uint32_t i = 0; i < count; i++) {
        auto& lane = g_lanes[i];
        if (!lane.used.load(std::memory_order_acquire) || !lane.owned.load(std::memory_order_acquire)) {
            continue;
        }
        OpenZones entry;
        entry.laneIndex = i;
        {
            std::lock_guard lock(lane.mutex);
            entry.lane = lane.name;
        }
        auto depth = std::min(lane.openDepth.load(std::memory_order_acquire), maxDepth);
        entry.zones.reserve(depth);
        for (uint32_t d = 0; d < depth; d++) {
            entry.zones.push_back({
                .siteId = lane.openSites[d].load(std::memory_order_relaxed),
                .startNs = lane.openStarts[d].load(std::memory_order_relaxed),
            });
        }
        out.push_back(std::move(entry));
    }
}

auto historyUsage(std::vector<LaneUsage>& out) -> void {
    out.clear();
    std::vector<LaneInfo> laneList;
    lanes(laneList);
    for (const auto& lane : laneList) {
        auto blocks = blocksOf(lane.index);
        LaneUsage usage = {.index = lane.index, .name = lane.name};
        for (const auto& block : blocks) {
            usage.zones += block->count.load(std::memory_order_acquire);
            usage.bytes += sizeof(ZoneBlock);
        }
        out.push_back(std::move(usage));
    }
    LaneUsage gpu = {.index = UINT32_MAX, .name = "GPU"};
    {
        std::lock_guard lock(g_gpuMutex);
        for (const auto& frame : g_gpuFrames) {
            gpu.zones += frame.zones.size();
            gpu.bytes += sizeof(GpuFrame) + frame.zones.capacity() * sizeof(Zone);
        }
    }
    out.push_back(std::move(gpu));
}

auto lastFrame() -> std::optional<FrameInterval> {
    return frameAt(0);
}

auto frameAt(uint64_t offset) -> std::optional<FrameInterval> {
    std::lock_guard lock(g_frameMutex);
    if (offset >= g_intervals.size()) {
        return std::nullopt;
    }
    return g_intervals[g_intervals.size() - 1 - offset];
}

auto frameByIndex(uint64_t frameIndex) -> std::optional<FrameInterval> {
    std::lock_guard lock(g_frameMutex);
    if (g_intervals.empty() || frameIndex < g_intervals.front().frameIndex) {
        return std::nullopt;
    }
    auto position = frameIndex - g_intervals.front().frameIndex;
    if (position >= g_intervals.size()) {
        return std::nullopt;
    }
    const auto& interval = g_intervals[position];
    if (interval.frameIndex != frameIndex) {
        return std::nullopt;
    }
    return interval;
}

auto zonesIn(uint32_t laneIndex, uint64_t startNs, uint64_t endNs, std::vector<Zone>& out) -> void {
    out.clear();
    auto blocks = blocksOf(laneIndex);
    // Newest first; stop once entries end before the window (zones are appended in end order).
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        auto count = (*block)->count.load(std::memory_order_acquire);
        if (count == 0) {
            continue;
        }
        if ((*block)->newestEndNs.load(std::memory_order_relaxed) < startNs) {
            break;
        }
        for (uint32_t i = count; i > 0; i--) {
            const auto& z = (*block)->zones[i - 1];
            if (z.endNs < startNs) {
                return;
            }
            if (z.startNs <= endNs) {
                out.push_back(z);
            }
        }
    }
}

auto gpuZonesIn(uint64_t startNs, uint64_t endNs, std::vector<Zone>& out) -> void {
    out.clear();
    std::lock_guard lock(g_gpuMutex);
    for (auto frame = g_gpuFrames.rbegin(); frame != g_gpuFrames.rend(); ++frame) {
        for (const auto& zone : frame->zones) {
            if (zone.endNs >= startNs && zone.startNs <= endNs) {
                out.push_back(zone);
            }
        }
    }
}

auto framesIn(uint64_t startNs, uint64_t endNs, std::vector<FrameInterval>& out) -> void {
    out.clear();
    std::lock_guard lock(g_frameMutex);
    for (const auto& interval : g_intervals) {
        if (interval.endNs >= startNs && interval.startNs <= endNs) {
            out.push_back(interval);
        }
    }
}

auto frameHistory(std::vector<FrameStats>& out) -> void {
    std::lock_guard lock(g_frameMutex);
    out.assign(g_frames.begin(), g_frames.end());
}

auto gpuPassTimes(uint32_t frames, GpuPassTimes& out) -> void {
    out.frames = 0;
    out.frameMs = 0.0;
    out.gpuFrameMs = 0.0;
    out.passes.clear();
    uint64_t startNs = 0;
    uint64_t endNs = 0;
    {
        std::lock_guard lock(g_frameMutex);
        auto count = std::min<size_t>(frames, g_intervals.size());
        if (count == 0) {
            return;
        }
        startNs = g_intervals[g_intervals.size() - count].startNs;
        endNs = g_intervals.back().endNs;
        out.frames = (uint32_t) count;
        out.frameMs = (double) (endNs - startNs) * 1e-6 / (double) count;
        // A frame's GPU time is 0 until a GPU frame is known.
        double gpuSum = 0.0;
        uint32_t gpuCount = 0;
        for (size_t i = g_frames.size() - std::min(count, g_frames.size()); i < g_frames.size(); i++) {
            if (g_frames[i].gpuMs > 0.0) {
                gpuSum += g_frames[i].gpuMs;
                gpuCount++;
            }
        }
        out.gpuFrameMs = gpuCount > 0 ? gpuSum / gpuCount : 0.0;
    }
    // The order is the frames' recording order, the frame graph's, not their timestamps: zero-length passes tie, and timing moves. The newest
    // frame gives the order; a pass that ran only in an older frame goes right after the pass recorded before it there.
    thread_local std::unordered_map<uint32_t, uint64_t> totalNs;
    thread_local std::vector<uint32_t> order;
    totalNs.clear();
    order.clear();
    {
        // Newest GPU frame first, stopping at the window's start: the history holds seconds of frames, the window a few dozen.
        std::lock_guard lock(g_gpuMutex);
        for (auto frame = g_gpuFrames.rbegin(); frame != g_gpuFrames.rend(); ++frame) {
            if (frame->newestEndNs < startNs) {
                break;
            }
            auto previous = order.end();
            bool hasPrevious = false;
            for (const auto& zone : frame->zones) {
                if (zone.depth != 0 || zone.endNs < startNs || zone.startNs > endNs) {
                    continue;
                }
                totalNs[zone.siteId] += zone.endNs - zone.startNs;
                auto found = std::ranges::find(order, zone.siteId);
                if (found == order.end()) {
                    auto at = hasPrevious ? previous + 1 : order.begin();
                    found = order.insert(at, zone.siteId);
                }
                previous = found;
                hasPrevious = true;
            }
        }
    }
    for (auto siteId : order) {
        out.passes.push_back({.siteId = siteId, .msPerFrame = (double) totalNs[siteId] * 1e-6 / (double) out.frames});
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

auto zoneStats(uint32_t laneIndex, uint32_t siteId) -> ZoneStats {
    std::vector<double> samples;
    if (laneIndex == UINT32_MAX) {
        std::lock_guard lock(g_gpuMutex);
        for (const auto& frame : g_gpuFrames) {
            for (const auto& zone : frame.zones) {
                if (zone.siteId == siteId) {
                    samples.push_back((double) (zone.endNs - zone.startNs) * 1e-6);
                }
            }
        }
        return statsFrom(samples);
    }
    auto blocks = blocksOf(laneIndex);
    for (const auto& block : blocks) {
        auto count = block->count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < count; i++) {
            const auto& z = block->zones[i];
            if (z.siteId == siteId) {
                samples.push_back((double) (z.endNs - z.startNs) * 1e-6);
            }
        }
    }
    return statsFrom(samples);
}

auto exportChromeTrace(const char* path, const ExportOptions& options) -> bool {
    auto* f = std::fopen(path, "w");
    if (f == nullptr) {
        return false;
    }
    uint64_t startNs = 0;
    if (options.seconds > 0.0) {
        auto t = now();
        auto span = (uint64_t) (options.seconds * 1e9);
        startNs = t > span ? t - span : 0;
    }
    std::fprintf(f, "{\"displayTimeUnit\": \"ms\", \"traceEvents\": [\n");
    bool first = true;
    auto emit = [&](const Zone& z, uint32_t tid) {
        auto site = siteOf(z.siteId);
        if (site.level == Level::Detail && !options.detail) {
            return;
        }
        std::fprintf(f,
                     "%s{\"name\": \"%s\", \"cat\": \"%s\", \"ph\": \"X\", \"pid\": 1, \"tid\": %u, \"ts\": %.3f, \"dur\": %.3f, \"args\": {\"depth\": %u, \"value\": %llu, "
                     "\"group\": \"%s\", \"level\": \"%s\", \"file\": \"%s\", \"line\": %u}}",
                     first ? "" : ",\n",
                     site.name,
                     groupName(site.group),
                     tid,
                     (double) z.startNs * 1e-3,
                     (double) (z.endNs - z.startNs) * 1e-3,
                     (uint32_t) z.depth,
                     (unsigned long long) (z.hasValue ? z.value : 0),
                     groupName(site.group),
                     site.level == Level::Detail ? "detail" : "guide",
                     site.file,
                     site.line);
        first = false;
    };
    // Thread ids start at 100: viewers treat tid 0 and tid == pid as special (idle
    // thread, process main thread) and fold lanes together. Viewers sort rows by tid,
    // so Main and Render come first and the workers follow in numeric order; lane
    // indices depend on which thread registered first.
    constexpr uint32_t firstCpuTid = 100;
    std::vector<LaneInfo> laneList;
    lanes(laneList);
    auto laneRank = [](const LaneInfo& lane) -> std::pair<int, int> {
        if (lane.name == "Main") {
            return {0, 0};
        }
        if (lane.name == "Render") {
            return {1, 0};
        }
        auto digits = lane.name.find_last_not_of("0123456789");
        int number = 0;
        if (digits != std::string::npos && digits + 1 < lane.name.size()) {
            number = std::stoi(lane.name.substr(digits + 1));
        }
        return {2, number};
    };
    std::stable_sort(laneList.begin(), laneList.end(), [&](const LaneInfo& a, const LaneInfo& b) { return laneRank(a) < laneRank(b); });
    std::vector<Zone> zones;
    for (size_t rank = 0; rank < laneList.size(); rank++) {
        const auto& lane = laneList[rank];
        auto tid = firstCpuTid + (uint32_t) rank;
        std::fprintf(f, "%s{\"name\": \"thread_name\", \"ph\": \"M\", \"pid\": 1, \"tid\": %u, \"args\": {\"name\": \"%s\"}}", first ? "" : ",\n", tid, lane.name.c_str());
        first = false;
        zonesIn(lane.index, startNs, UINT64_MAX, zones);
        for (const auto& z : zones) {
            emit(z, tid);
        }
    }
    constexpr uint32_t gpuTid = 1000;
    std::fprintf(f, "%s{\"name\": \"thread_name\", \"ph\": \"M\", \"pid\": 1, \"tid\": %u, \"args\": {\"name\": \"GPU\"}}", first ? "" : ",\n", gpuTid);
    first = false;
    gpuZonesIn(startNs, UINT64_MAX, zones);
    for (const auto& z : zones) {
        emit(z, gpuTid);
    }
    // Plots as counter events, one series per plot.
    std::vector<PlotInfo> plotList;
    plots(plotList);
    std::vector<PlotSample> samples;
    for (const auto& info : plotList) {
        plotSamplesIn(info.id, startNs, UINT64_MAX, samples);
        for (const auto& sample : samples) {
            std::fprintf(f,
                         ",\n{\"name\": \"%s\", \"cat\": \"%s\", \"ph\": \"C\", \"pid\": 1, \"ts\": %.3f, \"args\": {\"value\": %.6g}}",
                         info.name.c_str(),
                         groupName(info.group),
                         (double) sample.timeNs * 1e-3,
                         sample.value);
        }
    }
    std::fprintf(f, "\n]}\n");
    std::fclose(f);
    return true;
}

} // namespace profile
