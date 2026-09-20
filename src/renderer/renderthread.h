#pragma once

#include "framegraphdebug.h"
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
    auto setDrawTiming(FgDrawTimingRequest request) -> void {
        std::lock_guard lock(drawTimingMutex);
        drawTimingRequest = std::move(request);
    }
    auto setTextureInspect(TextureInspectRequest request) -> void {
        std::lock_guard lock(drawTimingMutex);
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
    std::mutex drawTimingMutex;
    FgDrawTimingRequest drawTimingRequest;
    TextureInspectRequest textureInspectRequest;
    uint64_t fgDebugFrameCounter = 0;
};
