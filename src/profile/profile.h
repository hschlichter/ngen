#pragma once

// Profiling markers and the built-in collector behind the Performance window.
//
// Zones are cheap enough to leave in shipping code: a thread-local stack and a
// per-thread ring buffer of finished zones, no locks and no allocation on the
// hot path. Names are interned once per call site. A later Tracy integration
// becomes a second expansion of the same macros; nothing that uses them changes.
//
//   PROFILE_ZONE("SceneUpdate");        // scoped CPU zone on the calling thread
//   PROFILE_FRAME_MARK();               // once per frame on the main thread
//   profile::registerThread("Main"); // optional lane name, else "thread N"
//
// GPU zones go through the RHI (profilegpu.h) and arrive via submitGpuZones.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace profile {

constexpr uint32_t maxLanes = 32;    // main, render, job workers, whatever else registers
constexpr uint32_t ringSize = 16384; // zones per lane; covers several seconds at ~15 zones per frame
constexpr uint32_t maxDepth = 32;
constexpr uint32_t frameHistorySize = 1024; // frames of history for scrubbing; 5 s at 200 fps, 17 s at 60
constexpr uint32_t maxGpuZonesPerFrame = 64;

struct Zone {
    uint32_t nameId = 0;
    uint16_t depth = 0;
    bool hasValue = false;
    uint64_t startNs = 0;
    uint64_t endNs = 0;
    uint64_t value = 0; // optional payload set with PROFILE_ZONE_VALUE: a count, a byte size
};

// Duration statistics of one zone name over the history window.
struct ZoneStats {
    uint32_t count = 0;
    double minMs = 0.0;
    double avgMs = 0.0;
    double maxMs = 0.0;
    double p99Ms = 0.0;
};

struct LaneInfo {
    uint32_t index = 0;
    std::string name;
};

struct FrameInterval {
    uint64_t frameIndex = 0;
    uint64_t startNs = 0;
    uint64_t endNs = 0;
};

struct FrameStats {
    uint64_t frameIndex = 0;
    double cpuMs = 0.0; // main-thread frame interval
    double gpuMs = 0.0; // most recent GPU frame known when the frame closed; 0 = unknown
};

// Monotonic nanoseconds, same base as the observation bus.
auto now() -> uint64_t;

// Name interning. Static strings only; call once per call site and keep the id.
auto registerName(const char* name) -> uint32_t;
auto nameOf(uint32_t id) -> const char*;

// Names the calling thread's lane. The lane itself is taken on the thread's first zone, so
// threads that never record (idle job workers) do not consume one.
auto registerThread(const char* name) -> void;

auto beginZone(uint32_t nameId) -> void;
auto endZone() -> void;
// Attach a number to the innermost open zone on this thread (draw count, bytes, ...).
auto zoneValue(uint64_t value) -> void;
auto frameMark() -> void;

// Paused: zones, frame marks and GPU sets are dropped, so history stays exactly as it
// was for as long as the pause lasts. Zone stacks keep balancing; nothing else runs.
auto setPaused(bool paused) -> void;
auto isPaused() -> bool;

// GPU lane: one set per completed GPU frame. Zone times must already be on the CPU clock
// (calibrated, or anchored at the submit time); submitNs is the CPU time of the submit.
auto submitGpuZones(uint64_t frameIndex, uint64_t submitNs, std::span<const Zone> zones) -> void;

// Readers, any thread. Copies out; never blocks writers.
auto lanes(std::vector<LaneInfo>& out) -> void;
auto lastFrame() -> std::optional<FrameInterval>;
// Frame `offset` frames before the latest (0 = latest). Empty when out of history.
auto frameAt(uint64_t offset) -> std::optional<FrameInterval>;
// Frame by absolute index, for a pinned selection that must not drift as frames arrive. Empty when evicted.
auto frameByIndex(uint64_t frameIndex) -> std::optional<FrameInterval>;
auto zonesIn(uint32_t laneIndex, uint64_t startNs, uint64_t endNs, std::vector<Zone>& out) -> void;
// GPU zones intersecting [startNs, endNs] on the CPU clock, from every GPU frame in history.
auto gpuZonesIn(uint64_t startNs, uint64_t endNs, std::vector<Zone>& out) -> void;
// Main-thread frame intervals intersecting [startNs, endNs], oldest first.
auto framesIn(uint64_t startNs, uint64_t endNs, std::vector<FrameInterval>& out) -> void;
auto frameHistory(std::vector<FrameStats>& out) -> void;
// Statistics for every recorded zone with this name on a CPU lane, or on the GPU lane when laneIndex is UINT32_MAX.
auto zoneStats(uint32_t laneIndex, uint32_t nameId) -> ZoneStats;
// Whole history as Chrome trace event JSON (Perfetto, chrome://tracing, any JSON parser).
auto exportChromeTrace(const char* path) -> bool;

struct ScopedZone {
    explicit ScopedZone(uint32_t nameId) { beginZone(nameId); }
    ~ScopedZone() { endZone(); }
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
};

} // namespace profile

#define PROFILE_CONCAT_(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_(a, b)

#define PROFILE_ZONE(name)                                                                          \
    static const uint32_t PROFILE_CONCAT(profileZoneId_, __LINE__) = ::profile::registerName(name); \
    ::profile::ScopedZone PROFILE_CONCAT(profileZone_, __LINE__)(PROFILE_CONCAT(profileZoneId_, __LINE__))

#define PROFILE_ZONE_VALUE(value) ::profile::zoneValue((uint64_t) (value))
#define PROFILE_FRAME_MARK() ::profile::frameMark()
