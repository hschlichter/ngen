#pragma once

#include "rendersnapshot.h"
#include "renderworld.h"

#include <condition_variable>
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
};
