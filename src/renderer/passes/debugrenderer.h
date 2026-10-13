#pragma once

#include "framegraph.h"
#include "pipelineregistry.h"
#include "rhitypes.h"

#include <cstdint>
#include <span>
#include <vector>

class RhiDevice;
struct DebugDrawData;

class DebugRenderer {
public:
    auto init(RhiDevice* device, PipelineRegistry& pipelines, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat, RhiFormat depthFormat, std::span<RhiBuffer*> uniformBuffers)
        -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg, FgTextureHandle color, FgTextureHandle depth, RhiExtent2D extent, const DebugDrawData& data, uint32_t imageIndex) -> void;

private:
    PipelineRegistry* registry = nullptr;
    PipelineHandle pipelineHandle;
    RhiDescriptorSetLayout* descriptorSetLayout = nullptr;
    RhiDescriptorPool* descriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
    std::vector<RhiBuffer*> vertexBuffers;
    std::vector<void*> vertexBuffersMapped;
    static constexpr uint32_t maxVertices = 65536;
};
