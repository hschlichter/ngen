#include "renderthread.h"
#include "framegraph.h"
#include "profile.h"
#include "renderer.h"
#include "trace.h"

#include <utility>

auto RenderThread::start(Renderer* r) -> void {
    renderer = r;
    thread = std::jthread([this] { threadLoop(); });
    TRACE_EVENT("Render", "RenderThreadStart", "RenderThread").text("render thread started");
}

auto RenderThread::stop() -> void {
    {
        std::lock_guard lock(snapshotMutex);
        shutdownRequested = true;
    }
    snapshotReady.notify_one();
    if (thread.joinable()) {
        thread.join();
    }
    TRACE_EVENT("Render", "RenderThreadStop", "RenderThread").text("render thread stopped");
}

auto RenderThread::submitSnapshot(RenderSnapshot snapshot) -> void {
    std::unique_lock lock(snapshotMutex);
    slotAvailable.wait(lock, [this] { return !hasSnapshot || shutdownRequested; });
    if (shutdownRequested) {
        return;
    }
    pendingSnapshot = std::move(snapshot);
    hasSnapshot = true;
    lock.unlock();
    snapshotReady.notify_one();
}

auto RenderThread::submitSceneChanges(SceneChanges changes) -> void {
    std::lock_guard lock(uploadMutex);
    submitted.fetch_add(1, std::memory_order_acq_rel);
    if (pendingChanges) {
        pendingChanges->append(std::move(changes));
    } else {
        pendingChanges = std::move(changes);
    }
}

auto RenderThread::latestRenderDebug() -> std::optional<RenderDebugSnapshot> {
    std::lock_guard lock(renderDebugMutex);
    return std::exchange(renderDebugSlot, std::nullopt);
}

auto RenderThread::latestCullResult() -> std::optional<CullResult> {
    std::lock_guard lock(cullResultMutex);
    return std::exchange(cullResultSlot, std::nullopt);
}

auto RenderThread::latestFrameGraphDebug() -> std::optional<FrameGraphDebugSnapshot> {
    std::lock_guard lock(fgDebugMutex);
    return std::exchange(fgDebugSlot, std::nullopt);
}

auto RenderThread::threadLoop() -> void {
    profile::registerThread("Render");
    while (true) {
        RenderSnapshot snapshot;
        {
            PROFILE_ZONE("WaitSnapshot");
            std::unique_lock lock(snapshotMutex);
            snapshotReady.wait(lock, [this] { return hasSnapshot || shutdownRequested; });
            if (shutdownRequested && !hasSnapshot) {
                return;
            }
            snapshot = std::move(pendingSnapshot);
            hasSnapshot = false;
        }
        slotAvailable.notify_one();

        // Check for pending scene upload
        uint64_t appliedUpTo = 0;
        {
            std::lock_guard lock(uploadMutex);
            appliedUpTo = submitted.load(std::memory_order_acquire);
            if (pendingChanges) {
                auto changes = std::move(*pendingChanges);
                pendingChanges.reset();
                PROFILE_ZONE("SceneChanges");
                PROFILE_ZONE_VALUE(changes.instances.size());
                renderer->applySceneChanges(changes);
            }
        }

        bool wantDebug = fgDebugWanted.load(std::memory_order_relaxed);
        renderer->setFrameGraphDebugEnabled(wantDebug);
        bool wantRenderDebug = renderDebugWanted.load(std::memory_order_relaxed);
        renderer->setRenderDebugEnabled(wantRenderDebug);
        renderer->setCountersEnabled(countersWanted.load(std::memory_order_relaxed));
        if (auto budget = uploadBudgetWanted.exchange(0, std::memory_order_relaxed); budget > 0) {
            renderer->setUploadBudget(budget);
        }
        if (auto limit = geometryLimitWanted.exchange(UINT64_MAX, std::memory_order_relaxed); limit != UINT64_MAX) {
            renderer->setGeometryLimit(limit);
        }
        {
            std::lock_guard lock(textureInspectMutex);
            renderer->setTextureInspect(textureInspectRequest);
        }
        {
            std::lock_guard lock(reloadMutex);
            if (!reloadIds.empty()) {
                renderer->reloadShaders(std::exchange(reloadIds, {}));
            }
        }

        {
            std::lock_guard lock(captureMutex);
            if (captureWatchesChanged) {
                captureWatchesChanged = false;
                renderer->setCaptureWatches(captureWatches);
            }
            if (frameDebugWanted) {
                frameDebugWanted = false;
                renderer->requestFrameDebugCapture();
            }
        }

        renderer->render(snapshot);
        // What this frame drew: published after it, so the main thread sees residency only once it is on screen.
        gpuIdle.store(renderer->sceneGpuIdle(), std::memory_order_release);
        drawnUpTo.store(appliedUpTo, std::memory_order_release);
        {
            auto stats = renderer->sceneGpuStats();
            stats.changeListsSubmitted = submitted.load(std::memory_order_acquire);
            stats.changeListsDrawn = appliedUpTo;
            stats.gpuIdle = renderer->sceneGpuIdle();
            std::lock_guard lock(statsMutex);
            sceneStats = stats;
        }

        {
            auto results = renderer->takeCaptureResults();
            auto frameDebug = renderer->takeFrameDebugCapture();
            auto screenshots = renderer->takeScreenshotResults();
            if (!results.empty() || frameDebug.has_value() || !screenshots.empty()) {
                std::lock_guard lock(captureMutex);
                for (auto& shot : screenshots) {
                    screenshotResults.push_back(std::move(shot));
                }
                for (auto& result : results) {
                    captureResults.push_back(std::move(result));
                }
                if (frameDebug.has_value()) {
                    frameDebugSlot = std::move(frameDebug);
                }
            }
        }

        if (auto counters = renderer->takeCounters(); !counters.empty()) {
            std::lock_guard lock(countersMutex);
            for (auto& c : counters) {
                countersResults.push_back(std::move(c));
            }
            if (countersResults.size() > 300) {
                countersResults.erase(countersResults.begin(), countersResults.end() - 300);
            }
        }

        {
            auto cull = renderer->cullResult();
            if (cull.frame != 0 && cull.frame != lastCullResultFrame) {
                lastCullResultFrame = cull.frame;
                std::lock_guard lock(cullResultMutex);
                cullResultSlot = std::move(cull);
            }
        }

        if (wantDebug) {
            auto fgSnap = renderer->buildFrameGraphDebugSnapshot();
            fgSnap.frameIndex = ++fgDebugFrameCounter;
            std::lock_guard lock(fgDebugMutex);
            fgDebugSlot = std::move(fgSnap);
        }
        if (wantRenderDebug) {
            auto snap = renderer->buildRenderDebugSnapshot();
            std::lock_guard lock(renderDebugMutex);
            renderDebugSlot = std::move(snap);
        }
    }
}
