#pragma once

#include "framegraphresource.h"

class RhiCommandBuffer;
class FrameGraph;

class FrameGraphContext {
public:
    FrameGraphContext(FrameGraph* graph, RhiCommandBuffer* cmd) : graph(graph), commandBuffer(cmd) {}

    auto texture(FgTextureHandle handle) -> RhiTexture*;
    auto cmd() -> RhiCommandBuffer* { return commandBuffer; }

private:
    FrameGraph* graph;
    RhiCommandBuffer* commandBuffer;
};
