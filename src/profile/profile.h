#pragma once

// Profiling markers and the built-in collector behind the Performance window, the profile records and the dump.
//
// Zones are cheap enough to leave in shipping code: a thread-local stack and per-thread blocks of finished zones,
// no locks on the hot path. Every marker is a site: its name, file, line and function, a group (its colour) and
// a level (guide or detail). The collector keeps the last profile.history.seconds of everything. In
// builds with TRACY_ENABLE the collector's functions also feed Tracy; nothing that uses the macros knows about it
// (src/profile/README.md).
//
//   PROFILE_ZONE("SceneUpdate", Frame);          // scoped CPU zone on the calling thread, guide level
//   PROFILE_ZONE_DETAIL("ComposeLayer", Scene);  // the same at detail level
//   PROFILE_FRAME_MARK();                        // once per frame on the main thread
//   profile::registerThread("Main");             // lane name, else "thread N"
//
// GPU zones go through the RHI (profilegpu.h) and arrive via submitGpuZones.

#include <chrono>
#include <cstdint>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <vector>

namespace profile {

constexpr uint32_t maxLanes = 64; // main, render, scene, job workers, I/O threads, pack workers
constexpr uint32_t maxDepth = 32;
constexpr uint32_t maxNames = 4096;
// A thread that idles waits in slices this long, each an Idle zone, so an idle thread has zones (and a row in Tracy) within a slice
// of anyone looking, not only after its next piece of work.
constexpr std::chrono::milliseconds idleSlice{100};

// What a zone belongs to; the group is its colour in Tracy and the Performance window.
enum class Group : uint8_t {
    Frame,    // the main loop's steps
    Scene,    // the scene thread: composing, extracting, placing
    Assets,   // asset requests and arrivals, packing
    Jobs,     // job system work; each job name a shade
    Ui,       // windows and overlays
    Tools,    // LOD and Streaming tools, the debug camera, gizmos
    Render,   // the render thread's CPU work
    Culling,  // GPU culling, LOD selection, the depth pyramid
    Geometry, // depth prepass, geometry, debug views, draw regions
    Shadow,
    Lighting, // lighting, sky
    Post,     // anti-aliasing, overlays, present passes
    Upload,   // uploads and copies, CPU and GPU
    Io,       // RPC, trace stream, asset client I/O
    Wait,     // blocking: fences, snapshots, condition variables, idle
};
constexpr uint32_t groupCount = 15;

// Guide zones are the overview the Performance window shows by default; detail zones are everything finer.
enum class Level : uint8_t {
    Guide,
    Detail,
};

// A marker site: where a zone kind is opened, with its name, group and level. Zones and the GPU lane carry the site's id; ids start at 1,
// 0 is no site.
struct SiteInfo {
    const char* name = "?";
    const char* file = "";     // as the compiler saw it: the path relative to the project root
    const char* function = ""; // the enclosing function's signature
    uint32_t line = 0;
    Group group = Group::Render;
    Level level = Level::Guide;
};

struct Zone {
    uint32_t siteId = 0;
    uint16_t depth = 0;
    bool hasValue = false;
    uint64_t startNs = 0;
    uint64_t endNs = 0;
    uint64_t value = 0; // optional payload set with PROFILE_ZONE_VALUE: a count, a byte size
};

// Duration statistics of one zone site over the history window.
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

// What the history holds for one lane (laneIndex UINT32_MAX: the GPU).
struct LaneUsage {
    uint32_t index = 0;
    std::string name;
    uint64_t zones = 0;
    uint64_t bytes = 0;
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

// What the Chrome trace export writes: the last `seconds` of history (0: all of it), and detail zones or guide zones only.
struct ExportOptions {
    double seconds = 0.0;
    bool detail = true;
};

// CLOCK_MONOTONIC nanoseconds, the same clock as trace events (shared by every process on the machine).
auto now() -> uint64_t;

// A site per (file, line, name): the same arguments return the same id. The name is copied; `location` is the caller's line unless given
// (a function that registers for its caller, such as FrameGraph::addPass, passes its own caller's).
auto registerSite(const char* name, Group group, Level level = Level::Guide, std::source_location location = std::source_location::current()) -> uint32_t;
// A GPU zone recorded without a profile marker (RhiCommandBuffer::beginGpuZone called directly): a site by name alone.
auto siteIdOf(const char* name) -> uint32_t;
auto siteOf(uint32_t siteId) -> SiteInfo;
auto nameOf(uint32_t siteId) -> const char*;
auto groupOf(uint32_t siteId) -> Group;
auto levelOf(uint32_t siteId) -> Level;
auto groupName(Group group) -> const char*;
// 0xRRGGBB: the group's colour; Jobs zones a shade of it by site.
auto groupColor(Group group, uint32_t siteId) -> uint32_t;

// Tracy's client, in builds with TRACY_ENABLE, starts only when the program asks: its start-up sleeps 200 ms to calibrate its timer,
// and its shutdown takes as long again, which a short-lived program must not pay. Until it has started, nothing goes to Tracy; the
// built-in collector always records. Once started it stops at exit, after waiting for a connected server to take everything when
// TRACY_NO_EXIT=1 is set.
// startTracy: long-running programs, at launch; it starts on a background thread and returns at once.
auto startTracy() -> void;
// startTracyIfAsked: short-lived programs; starts on this thread when NGEN_TRACY=1 is set, so their whole run is captured.
auto startTracyIfAsked() -> void;

// Names the calling thread's lane and takes it, with a zero-length ThreadStarted zone so the thread shows from its start.
auto registerThread(const char* name) -> void;

auto beginZone(uint32_t siteId) -> void;
auto endZone() -> void;
// Attach a number to the innermost open zone on this thread (draw count, bytes, ...).
auto zoneValue(uint64_t value) -> void;
auto frameMark() -> void;

// How long the history keeps zones, frames and GPU frames (profile.history.seconds, handed in by the main loop). Any thread.
auto setHistorySeconds(double seconds) -> void;
auto historySeconds() -> double;
// Whether detail GPU zones record (profile.gpu.detail). Any thread.
auto setGpuDetail(bool enabled) -> void;
auto gpuDetail() -> bool;

// Paused: zones, frame marks and GPU sets are dropped, so history stays exactly as it
// was for as long as the pause lasts. Zone stacks keep balancing; Tracy keeps recording.
auto setPaused(bool paused) -> void;
auto isPaused() -> bool;

// GPU lane: one set per completed GPU frame. Zone times must already be on the CPU clock
// (calibrated, or anchored at the submit time); submitNs is the CPU time of the submit.
auto submitGpuZones(uint64_t frameIndex, uint64_t submitNs, std::span<const Zone> zones) -> void;

// Readers, any thread. Copies out; never blocks writers for longer than a block switch.
auto lanes(std::vector<LaneInfo>& out) -> void;
auto historyUsage(std::vector<LaneUsage>& out) -> void;
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
// Statistics for every recorded zone of this site on a CPU lane, or on the GPU lane when laneIndex is UINT32_MAX.
auto zoneStats(uint32_t laneIndex, uint32_t siteId) -> ZoneStats;
// History as Chrome trace event JSON (Perfetto, chrome://tracing, any JSON parser).
auto exportChromeTrace(const char* path, const ExportOptions& options = {}) -> bool;

// A zone for the scope; `active` false records nothing (a recursion zoned only near its top).
struct ScopedZone {
    explicit ScopedZone(uint32_t siteId, bool active = true) : active(active) {
        if (active) {
            beginZone(siteId);
        }
    }
    ~ScopedZone() {
        if (active) {
            endZone();
        }
    }
    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
    bool active;
};

} // namespace profile

#define PROFILE_CONCAT_(a, b) a##b
#define PROFILE_CONCAT(a, b) PROFILE_CONCAT_(a, b)

#define PROFILE_ZONE_LEVEL_(name, group, level)                                                                                                       \
    static const uint32_t PROFILE_CONCAT(profileZoneId_, __LINE__) = ::profile::registerSite(name, ::profile::Group::group, ::profile::Level::level); \
    ::profile::ScopedZone PROFILE_CONCAT(profileZone_, __LINE__)(PROFILE_CONCAT(profileZoneId_, __LINE__))

// `group` is a profile::Group enumerator without its scope: PROFILE_ZONE("Upload", Upload).
#define PROFILE_ZONE(name, group) PROFILE_ZONE_LEVEL_(name, group, Guide)
#define PROFILE_ZONE_DETAIL(name, group) PROFILE_ZONE_LEVEL_(name, group, Detail)

#define PROFILE_ZONE_VALUE(value) ::profile::zoneValue((uint64_t) (value))
#define PROFILE_FRAME_MARK() ::profile::frameMark()
