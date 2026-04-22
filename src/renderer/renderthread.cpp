#include "renderthread.h"
#include "framegraph.h"
#include "observationmacros.h"
#include "renderer.h"

#include <utility>

auto RenderThread::start(Renderer* r) -> void {
    renderer = r;
    thread = std::jthread([this] { threadLoop(); });
    OBS_EVENT("Render", "RenderThreadStart", "RenderThread");
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
    OBS_EVENT("Render", "RenderThreadStop", "RenderThread");
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

auto RenderThread::submitRenderUpload(RenderUpload upload) -> void {
    std::lock_guard lock(uploadMutex);
    pendingUpload = std::move(upload);
}

auto RenderThread::latestFrameGraphDebug() -> std::optional<FrameGraphDebugSnapshot> {
    std::lock_guard lock(fgDebugMutex);
    return std::exchange(fgDebugSlot, std::nullopt);
}

auto RenderThread::threadLoop() -> void {
    while (true) {
        RenderSnapshot snapshot;
        {
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
        {
            std::lock_guard lock(uploadMutex);
            if (pendingUpload) {
                auto upload = std::move(*pendingUpload);
                pendingUpload.reset();
                OBS_EVENT("Render", "SceneUploadReceived", "RenderWorld").field("mesh_count", (int64_t) upload.world.meshInstances.size());
                renderer->uploadRenderWorld(upload.world, *upload.meshLib, *upload.matLib);
            }
        }

        bool wantDebug = fgDebugWanted.load(std::memory_order_relaxed);
        renderer->setFrameGraphDebugEnabled(wantDebug);

        renderer->render(snapshot);

        if (wantDebug) {
            auto fgSnap = renderer->buildFrameGraphDebugSnapshot();
            fgSnap.frameIndex = ++fgDebugFrameCounter;
            std::lock_guard lock(fgDebugMutex);
            fgDebugSlot = std::move(fgSnap);
        }
    }
}
