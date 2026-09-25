#pragma once

#include "drawlists.h"
#include "framegraph.h"
#include "geometrypass.h" // GpuInstance, GpuScene
#include "rhitypes.h"
#include "shadowcascades.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>

class RhiDevice;
class DeletionQueue;

struct ShadowPassData {
    FgTextureHandle shadowMap;
};

class ShadowPass {
public:
    auto init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    // Points the pass's descriptor set at the instance buffer. Called whenever the buffer is
    // (re)created; the old set may still be bound by frames in flight, so it is freed through
    // the deletion queue at `frame`.
    auto bindInstanceBuffer(RhiDevice* device, RhiBuffer* instanceBuffer, DeletionQueue& deletionQueue, uint64_t frame) -> void;

    // One atlas texture of atlasExtent; each cascade draws into its tile with its own
    // viewport, scissor and light matrix. Cascade c draws DrawLists::cascadeRegion(c, *)
    // indirectly.
    auto addPass(
        FrameGraph& fg,
        RhiExtent2D atlasExtent,
        RhiFormat depthFormat,
        std::span<const ShadowCascade> cascades,
        std::span<const GpuInstance> instances,
        FgBufferHandle instanceBuffer,
        const DrawLists& lists,
        DrawLists::Handles drawHandles,
        const GpuScene& scene) -> const ShadowPassData&;

private:
    RhiDescriptorSetLayout* descSetLayout = nullptr; // binding 0: instance buffer
    RhiDescriptorPool* descPool = nullptr;
    RhiDescriptorSet* descSet = nullptr;
    RhiPipeline* pipelineCullBack = nullptr;
    RhiPipeline* pipelineCullNone = nullptr;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
};
