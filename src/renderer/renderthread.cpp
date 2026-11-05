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
    {
        // The main thread waiting for the render thread to take the previous snapshot: a frame bound by the render thread or the GPU.
        PROFILE_ZONE("WaitRenderThread");
        slotAvailable.wait(lock, [this] { return !hasSnapshot || shutdownRequested; });
    }
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

auto RenderThread::latestCullResult() -> std::optional<CullResult> {
    std::lock_guard lock(cullResultMutex);
    return std::exchange(cullResultSlot, std::nullopt);
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

        if (auto budget = uploadBudgetWanted.exchange(0, std::memory_order_relaxed); budget > 0) {
            renderer->setUploadBudget(budget);
        }
        if (auto limit = geometryLimitWanted.exchange(UINT64_MAX, std::memory_order_relaxed); limit != UINT64_MAX) {
            renderer->setGeometryLimit(limit);
        }
        {
            std::lock_guard lock(reloadMutex);
            if (!reloadIds.empty()) {
                renderer->reloadShaders(std::exchange(reloadIds, {}));
            }
        }

        {
            PROFILE_ZONE("RenderFrame");
            renderer->render(snapshot);
        }
        // What this frame drew: published after it, so the main thread sees residency only once it is on screen.
        gpuIdle.store(renderer->sceneGpuIdle(), std::memory_order_release);
        drawnUpTo.store(appliedUpTo, std::memory_order_release);
        {
            PROFILE_ZONE("SceneGpuStats");
            auto stats = renderer->sceneGpuStats();
            stats.changeListsSubmitted = submitted.load(std::memory_order_acquire);
            stats.changeListsDrawn = appliedUpTo;
            stats.gpuIdle = renderer->sceneGpuIdle();
            std::lock_guard lock(statsMutex);
            sceneStats = stats;
        }

        {
            PROFILE_ZONE("TakeResults");
            if (auto results = renderer->takeCaptureResults(); !results.empty()) {
                std::lock_guard lock(captureMutex);
                for (auto& result : results) {
                    captureResults.push_back(std::move(result));
                }
            }
        }

        {
            PROFILE_ZONE("CullResult");
            auto cull = renderer->cullResult();
            if (cull.frame != 0 && cull.frame != lastCullResultFrame) {
                lastCullResultFrame = cull.frame;
                std::lock_guard lock(cullResultMutex);
                cullResultSlot = std::move(cull);
            }
        }
    }
}
