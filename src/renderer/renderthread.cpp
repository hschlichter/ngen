#include "renderthread.h"
#include "renderer.h"

auto RenderThread::start(Renderer* r) -> void {
    renderer = r;
    thread = std::jthread([this] { threadLoop(); });
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
                renderer->uploadRenderWorld(upload.world, *upload.meshLib, *upload.matLib);
            }
        }

        renderer->render(snapshot);
    }
}
