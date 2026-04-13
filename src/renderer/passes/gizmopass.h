#pragma once

#include "framegraph.h"
#include "gizmo.h"
#include "rhitypes.h"

#include <cstdint>
#include <span>
#include <vector>

class RhiDevice;

class GizmoPass {
public:
    auto init(RhiDevice* device, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg, FgTextureHandle color, RhiExtent2D fullExtent, std::span<GizmoDrawRequest> requests, uint32_t imageIndex) -> void;

private:
    RhiPipeline* pipeline = nullptr;
    RhiDescriptorSetLayout* descriptorSetLayout = nullptr;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
    std::vector<RhiBuffer*> vertexBuffers;
    std::vector<void*> vertexBuffersMapped;
    static constexpr uint32_t maxVertices = 4096;
};
