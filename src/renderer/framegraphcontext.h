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

    // Logs one draw for the render debugger; no-op unless the draw log is enabled. Pass and
    // draw index are filled in by the context.
    auto logDraw(const FgDrawRecord& record) -> void;
    // Draws and primitives an indirect call issued, which the RHI cannot see; folded into
    // the executing pass's stats.
    auto addIndirectStats(uint32_t draws, uint64_t primitives) -> void;
    // A barrier inside the executing pass, on a texture it writes as a storage image: its writes so far made visible to its reads and writes
    // after (General to General, as a pass that writes one mip from the one before). Recorded with the graph's barriers for the pass, so the
    // frame debugger lists it.
    auto storageBarrier(FgTextureHandle handle) -> void;

private:
    FrameGraph* graph;
    RhiCommandBuffer* commandBuffer;
};
