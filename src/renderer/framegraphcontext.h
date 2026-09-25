#pragma once

#include "framegraphdraw.h"
#include "framegraphresource.h"

class RhiCommandBuffer;
class FrameGraph;

class FrameGraphContext {
public:
    FrameGraphContext(FrameGraph* graph, RhiCommandBuffer* cmd) : graph(graph), commandBuffer(cmd) {}

    auto texture(FgTextureHandle handle) -> RhiTexture*;
    auto buffer(FgBufferHandle handle) -> RhiBuffer*;
    auto cmd() -> RhiCommandBuffer* { return commandBuffer; }

    // Bracket one draw for the render debugger. No-ops unless the draw log is enabled;
    // draws inside the timing window also get a GPU zone. Pass and draw index are
    // filled in by the context.
    auto beginDraw(const FgDrawRecord& record) -> void;
    auto endDraw() -> void;

private:
    FrameGraph* graph;
    RhiCommandBuffer* commandBuffer;
    bool drawZoneOpen = false;
};
