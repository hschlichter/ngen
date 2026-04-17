#pragma once

#include "framegraphresource.h"

class FrameGraph;

class FrameGraphBuilder {
public:
    FrameGraphBuilder(FrameGraph* graph, uint32_t passIndex) : graph(graph), passIndex(passIndex) {}

    auto createTexture(const char* name, const FgTextureDesc& desc) -> FgTextureHandle;
    auto read(FgTextureHandle handle, FgAccessFlags access) -> FgTextureHandle;
    auto write(FgTextureHandle handle, FgAccessFlags access) -> FgTextureHandle;
    auto setSideEffects(bool value) -> void;

private:
    FrameGraph* graph;
    uint32_t passIndex;
};
