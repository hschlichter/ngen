#pragma once

#include "capture.h"
#include "drawlists.h"
#include "rendersnapshot.h"
#include "renderworld.h"
#include "scenegpustats.h"

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

    // Latest GPU culling readback, when one arrived since the last call.
    auto latestCullResult() -> std::optional<CullResult>;
    // Every capture result since the last call; the watches go with the snapshot (RenderSnapshot::captureWatches).
    auto takeCaptureResults() -> std::vector<CaptureResult> {
        std::lock_guard lock(captureMutex);
        return std::exchange(captureResults, {});
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

    std::mutex captureMutex;
    std::vector<CaptureResult> captureResults;
    std::mutex cullResultMutex;
    std::optional<CullResult> cullResultSlot;
    uint64_t lastCullResultFrame = 0;
    std::mutex reloadMutex;
    std::vector<std::string> reloadIds;
};
