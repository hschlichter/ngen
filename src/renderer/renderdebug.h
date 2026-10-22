#pragma once

#include "framegraphdebug.h"
#include "framegraphdraw.h"
#include "pipelineregistry.h"
#include "rhitypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

// The geometry pool's buffers against their limit, and the meshes that didn't fit (GpuScene::geometryPoolStats).
struct RenderDebugGeometryPool {
    uint64_t limitBytes = 0;  // per pool buffer
    uint64_t vertexLimit = 0; // the vertices and indices the limit holds
    uint64_t indexLimit = 0;
    uint64_t vertexCapacity = 0;
    uint64_t vertexUsed = 0;
    uint64_t indexCapacity = 0;
    uint64_t indexUsed = 0;
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
};

// Everything the Render Debug window shows, copied on the render thread once per frame
// while the window is open. Same delivery as FrameGraphDebugSnapshot.
struct RenderDebugMesh {
    uint32_t meshIndex = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;
    uint32_t instances = 0; // GPU instances referencing this mesh (submeshes count separately)
};

struct RenderDebugTexture {
    uint32_t textureIndex = 0;       // TextureHandle::index
    uint32_t slot = 0;               // in the texture array; 0 while it uploads
    std::vector<uint32_t> materials; // the materials naming it, by MaterialHandle::index
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    RhiFormat format = RhiFormat::Undefined;
    uint64_t bytes = 0;
};

// Texture inspector: which texture (TextureHandle::index) and level the Render Debug window shows.
struct TextureInspectRequest {
    bool enabled = false;
    uint32_t texture = 0;
    uint32_t level = 0;

    auto operator==(const TextureInspectRequest&) const -> bool = default;
};

// What the renderer captured for the inspector this frame.
struct RenderDebugTextureInspect {
    bool valid = false;
    uint32_t texture = 0;
    uint32_t level = 0;
    uint32_t mipLevels = 1;
    uint32_t levelWidth = 0;
    uint32_t levelHeight = 0;
    uint64_t levelBytes = 0;
    uint64_t previewTextureId = 0; // ImGui texture id
    uint32_t previewWidth = 0;
    uint32_t previewHeight = 0;
};

struct RenderDebugPoolTexture {
    uint32_t width = 0;
    uint32_t height = 0;
    RhiFormat format = RhiFormat::Undefined;
    bool inUse = false;
};

struct RenderDebugSnapshot {
    uint64_t frameIndex = 0;

    // Device
    RhiDeviceLimits limits;
    bool validation = false;

    // Swapchain and pacing
    RhiExtent2D swapchainExtent = {};
    RhiFormat swapchainFormat = RhiFormat::Undefined;
    uint32_t swapchainImages = 0;
    uint32_t currentSlot = 0;

    // Scene
    uint32_t instanceCount = 0;
    uint32_t primFirstInstances = 0; // what the shadow pass draws
    uint32_t culledInstances = 0;    // skipped by the geometry pass this frame
    uint32_t materialCount = 0;
    uint32_t materialsWithTexture = 0;
    uint32_t cutoutMaterials = 0; // drawn only where their opacity reaches the threshold
    uint32_t cutoutInstances = 0; // in the cutout buckets
    std::vector<RenderDebugMesh> meshes;
    std::vector<RenderDebugTexture> textures;
    RenderDebugTextureInspect inspect;
    uint32_t lightCount = 0;
    bool hasSun = false;
    glm::vec3 sunDirection = glm::vec3(0.0f);
    glm::vec3 sunRadiance = glm::vec3(0.0f);
    glm::vec3 sunShadowColor = glm::vec3(0.0f);
    RhiExtent2D shadowMapExtent = {};

    // Pipelines: every one the passes registered, with its shaders and state
    std::vector<PipelineInfo> pipelines;

    // Frame
    std::vector<FgPassDebug> passes; // execution order, with stats and GPU time
    RhiCommandStats frameTotals;
    std::vector<RenderDebugPoolTexture> poolTextures;
    uint32_t poolAllocationsTotal = 0;

    // GPU memory: every live buffer and texture, and the device heaps (RhiDevice).
    std::vector<RhiAllocationInfo> allocations;
    std::vector<RhiMemoryHeapInfo> heaps;
    RenderDebugGeometryPool geometryPool;

    // Draw log of the most recent frame whose readback has arrived: the indirect commands the GPU
    // culling wrote, one entry per draw.
    std::vector<FgDrawRecord> draws;
};
