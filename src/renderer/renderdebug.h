#pragma once

#include "framegraphdebug.h"
#include "framegraphdraw.h"
#include "rhitypes.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

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
    uint32_t materialIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    RhiFormat format = RhiFormat::Undefined;
    uint64_t bytes = 0;
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
    uint32_t materialCount = 0;
    uint32_t materialsWithTexture = 0;
    std::vector<RenderDebugMesh> meshes;
    std::vector<RenderDebugTexture> textures;
    uint32_t lightCount = 0;
    bool hasSun = false;
    glm::vec3 sunDirection = glm::vec3(0.0f);
    glm::vec3 sunRadiance = glm::vec3(0.0f);
    glm::vec3 sunShadowColor = glm::vec3(0.0f);
    RhiExtent2D shadowMapExtent = {};

    // Frame
    std::vector<FgPassDebug> passes; // execution order, with stats and GPU time
    RhiCommandStats frameTotals;
    std::vector<RenderDebugPoolTexture> poolTextures;
    uint32_t poolAllocationsTotal = 0;

    // Draw log of the most recent frame whose GPU zones have been read, with per-draw
    // GPU time for the draws inside the timing window.
    std::vector<FgDrawRecord> draws;
    FgDrawTimingRequest drawTiming;
};
