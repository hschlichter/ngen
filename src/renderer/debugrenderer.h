#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <cstdint>

struct DebugDrawData;

struct DebugLinePassResources {
    RhiPipeline* pipeline;
    RhiBuffer* vertexBuffer;
    RhiDescriptorSet* descriptorSet;
    void* vertexBufferMapped;
    uint32_t maxVertices;
};

class DebugRenderer {
public:
    auto addPass(
        FrameGraph& fg, FgTextureHandle color, FgTextureHandle depth, RhiExtent2D extent, const DebugDrawData& data, const DebugLinePassResources& resources)
        -> void;
};
