#pragma once

#include "framegraph.h"
#include "gizmo.h"
#include "pipelineregistry.h"
#include "rhitypes.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

class RhiDevice;

class GizmoPass {
public:
    auto init(RhiDevice* device, PipelineRegistry& pipelines, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg, FgTextureHandle color, RhiExtent2D fullExtent, std::span<GizmoDrawRequest> requests, uint32_t imageIndex) -> void;

    // Whether its pipelines have been created; until then the frame leaves the pass out.
    auto ready() -> bool;

private:
    PipelineRegistry* registry = nullptr;
    PipelineHandle pipelineHandle;
    RhiDescriptorSetLayout* descriptorSetLayout = nullptr;
    std::vector<RhiBuffer*> vertexBuffers;
    std::vector<void*> vertexBuffersMapped;
    static constexpr uint32_t maxVertices = 4096;
};
