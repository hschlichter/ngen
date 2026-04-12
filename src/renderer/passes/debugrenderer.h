#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <cstdint>
#include <span>
#include <vector>

class RhiDevice;
struct DebugDrawData;

class DebugRenderer {
public:
    auto init(RhiDevice* device, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat, RhiFormat depthFormat, std::span<RhiBuffer*> uniformBuffers)
        -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg, FgTextureHandle color, FgTextureHandle depth, RhiExtent2D extent, const DebugDrawData& data, uint32_t imageIndex) -> void;

private:
    RhiPipeline* pipeline = nullptr;
    RhiDescriptorSetLayout* descriptorSetLayout = nullptr;
    RhiDescriptorPool* descriptorPool = nullptr;
    std::vector<RhiDescriptorSet*> descriptorSets;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
    std::vector<RhiBuffer*> vertexBuffers;
    std::vector<void*> vertexBuffersMapped;
    static constexpr uint32_t maxVertices = 65536;
};
