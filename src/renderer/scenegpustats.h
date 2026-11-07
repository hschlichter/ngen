#pragma once

#include <cstdint>

// One part of the geometry pool, in its elements: what the limit holds, the capacity, what is used.
struct RenderDebugPoolUse {
    uint64_t limit = 0;
    uint64_t capacity = 0;
    uint64_t used = 0;
};

// The geometry pool's buffers against their limit, and the meshes that didn't fit (GpuScene::geometryPoolStats).
struct RenderDebugGeometryPool {
    uint64_t limitBytes = 0; // per pool buffer
    RenderDebugPoolUse vertices;
    RenderDebugPoolUse meshlets;
    RenderDebugPoolUse meshletVertices;
    RenderDebugPoolUse meshletTriangles;
    uint32_t waitingMeshes = 0;
    uint64_t waitingBytes = 0;
};

// The GPU scene's upload queue (GpuScene::uploadStats), from the render thread every frame.
struct RenderUploadStats {
    uint32_t queued = 0;
    uint64_t queuedBytes = 0;
    uint64_t budget = 0; // bytes per SceneUpload pass
    uint32_t lastUploads = 0;
    uint64_t lastBytes = 0;
    uint64_t lastFrame = 0;
    uint64_t totalBytes = 0;
    uint32_t texturesWaitingSlot = 0;
    uint32_t instanceRecords = 0; // the last instance upload: records copied, and the copy regions (runs of consecutive slots)
    uint32_t instanceRegions = 0;
};

// The GPU scene's flow, published by the render thread after every frame, whatever window is open: the change lists it took from the main
// thread, the upload queue, and the geometry pool.
struct SceneGpuStats {
    uint64_t frame = 0;
    uint64_t changeListsSubmitted = 0;
    uint64_t changeListsDrawn = 0; // applied by a finished frame
    bool gpuIdle = true;           // that frame left nothing to upload
    uint32_t drawSlots = 0;        // live and dead: slots are kept until reused
    uint32_t meshes = 0;
    uint32_t textures = 0;
    uint32_t materials = 0;
    RenderUploadStats uploads;
    RenderDebugGeometryPool geometryPool;
    bool skyDrawn = false; // the frame drew the scene's dome light as its sky (its image on the GPU, render.sky.on)
};
