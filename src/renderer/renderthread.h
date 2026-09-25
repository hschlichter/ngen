#pragma once

#include "capture.h"
#include "drawlists.h"
#include "framedebug.h"
#include "framegraphdebug.h"
#include "gpucounters.h"
#include "renderdebug.h"
#include "rendersnapshot.h"
#include "renderworld.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class Renderer;
class MeshLibrary;
class MaterialLibrary;

struct RenderUpload {
    RenderWorld world;
    std::shared_ptr<const MeshLibrary> meshLib;
    std::shared_ptr<const MaterialLibrary> matLib;
};

class RenderThread {
public:
    auto start(Renderer* renderer) -> void;
    auto stop() -> void;

    auto submitSnapshot(RenderSnapshot snapshot) -> void;
    auto submitRenderUpload(RenderUpload upload) -> void;

    auto setFrameGraphDebugEnabled(bool enabled) -> void { fgDebugWanted.store(enabled, std::memory_order_relaxed); }
    auto latestFrameGraphDebug() -> std::optional<FrameGraphDebugSnapshot>;
    auto setRenderDebugEnabled(bool enabled) -> void { renderDebugWanted.store(enabled, std::memory_order_relaxed); }
    auto latestRenderDebug() -> std::optional<RenderDebugSnapshot>;
    // Latest GPU culling readback, when one arrived since the last call.
    auto latestCullResult() -> std::optional<CullResult>;
    // Capture watches (latest-only) and every capture result since the last call.
    // requestFrameDebug records the frame that applies these watches as a FrameDebugCapture,
    // so the captures and the frame debug data describe the same frame.
    auto setCaptureWatches(std::vector<CaptureWatch> watches, bool requestFrameDebug = false) -> void {
        std::lock_guard lock(captureMutex);
        captureWatches = std::move(watches);
        captureWatchesChanged = true;
        frameDebugWanted = frameDebugWanted || requestFrameDebug;
    }
    auto latestFrameDebug() -> std::optional<FrameDebugCapture> {
        std::lock_guard lock(captureMutex);
        return std::exchange(frameDebugSlot, std::nullopt);
    }
    auto takeCaptureResults() -> std::vector<CaptureResult> {
        std::lock_guard lock(captureMutex);
        return std::exchange(captureResults, {});
    }
    // GPU counters, one per completed frame while enabled (see Renderer::setCountersEnabled).
    auto setCountersEnabled(bool enabled) -> void { countersWanted.store(enabled, std::memory_order_relaxed); }
    auto takeCounters() -> std::vector<GpuCounters> {
        std::lock_guard lock(countersMutex);
        return std::exchange(countersResults, {});
    }
    auto setTextureInspect(TextureInspectRequest request) -> void {
        std::lock_guard lock(textureInspectMutex);
        textureInspectRequest = request;
    }

private:
    auto threadLoop() -> void;

    Renderer* renderer = nullptr;
    std::jthread thread;

    // Snapshot slot (single-buffered with back-pressure)
    std::mutex snapshotMutex;
    std::condition_variable snapshotReady;
    std::condition_variable slotAvailable;
    RenderSnapshot pendingSnapshot;
    bool hasSnapshot = false;
    bool shutdownRequested = false;

    // Scene upload slot (separate channel)
    std::mutex uploadMutex;
    std::optional<RenderUpload> pendingUpload;

    // Frame graph debug slot (separate channel, latest-only, non-blocking)
    std::atomic<bool> fgDebugWanted{false};
    std::mutex fgDebugMutex;
    std::optional<FrameGraphDebugSnapshot> fgDebugSlot;
    std::atomic<bool> renderDebugWanted{false};
    std::mutex renderDebugMutex;
    std::optional<RenderDebugSnapshot> renderDebugSlot;
    std::mutex captureMutex;
    std::vector<CaptureWatch> captureWatches;
    bool captureWatchesChanged = false;
    std::vector<CaptureResult> captureResults;
    bool frameDebugWanted = false;
    std::optional<FrameDebugCapture> frameDebugSlot;
    std::atomic<bool> countersWanted{false};
    std::mutex countersMutex;
    std::vector<GpuCounters> countersResults;
    std::mutex cullResultMutex;
    std::optional<CullResult> cullResultSlot;
    uint64_t lastCullResultFrame = 0;
    std::mutex textureInspectMutex;
    TextureInspectRequest textureInspectRequest;
    uint64_t fgDebugFrameCounter = 0;
};
