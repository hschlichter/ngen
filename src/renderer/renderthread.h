#pragma once

#include "capture.h"
#include "drawlists.h"
#include "framedebug.h"
#include "framegraphdebug.h"
#include "gpucounters.h"
#include "renderdebug.h"
#include "rendersnapshot.h"
#include "renderworld.h"
#include "screenshot.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

class Renderer;

class RenderThread {
public:
    auto start(Renderer* renderer) -> void;
    auto stop() -> void;

    auto submitSnapshot(RenderSnapshot snapshot) -> void;
    // Scene changes are applied in the order submitted; several submitted between frames are applied together.
    auto submitSceneChanges(SceneChanges changes) -> void;
    // Every change list submitted has been applied and drawn, with nothing left to upload and no pipeline pending, as of the last frame
    // the render thread finished.
    auto sceneDrawn() const -> bool {
        auto submittedSoFar = submitted.load(std::memory_order_acquire);
        auto drawnSoFar = drawnUpTo.load(std::memory_order_acquire);
        return drawnSoFar == submittedSoFar && gpuIdle.load(std::memory_order_acquire);
    }

    // The GPU scene's flow after the last finished frame: change lists, upload queue, geometry pool. Always published.
    auto latestSceneGpuStats() -> SceneGpuStats {
        std::lock_guard lock(statsMutex);
        return sceneStats;
    }
    // Runtime settings, applied before the next frame: bytes per SceneUpload pass, and the most each geometry pool buffer may take (0 for
    // the device's limit; lowering it stops growth, it doesn't shrink the pool).
    auto setUploadBudget(uint64_t bytes) -> void { uploadBudgetWanted.store(bytes, std::memory_order_relaxed); }
    auto setGeometryLimit(uint64_t bytes) -> void { geometryLimitWanted.store(bytes, std::memory_order_relaxed); }

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
    // Screenshots written since the last call.
    auto takeScreenshotResults() -> std::vector<ScreenshotResult> {
        std::lock_guard lock(captureMutex);
        return std::exchange(screenshotResults, {});
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
    // Asset ids whose sources changed; the renderer reloads those that are its shaders before its next frame.
    auto reloadShaders(std::span<const std::string> ids) -> void {
        std::lock_guard lock(reloadMutex);
        reloadIds.insert(reloadIds.end(), ids.begin(), ids.end());
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
    std::optional<SceneChanges> pendingChanges;
    std::atomic<uint64_t> submitted{0}; // change lists submitted
    std::atomic<uint64_t> drawnUpTo{0}; // change lists applied by a frame the render thread finished
    std::atomic<bool> gpuIdle{true};    // that frame left nothing to upload
    std::mutex statsMutex;
    SceneGpuStats sceneStats;
    std::atomic<uint64_t> uploadBudgetWanted{0};           // 0: unchanged
    std::atomic<uint64_t> geometryLimitWanted{UINT64_MAX}; // UINT64_MAX: unchanged

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
    std::vector<ScreenshotResult> screenshotResults;
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
    std::mutex reloadMutex;
    std::vector<std::string> reloadIds;
    uint64_t fgDebugFrameCounter = 0;
};
