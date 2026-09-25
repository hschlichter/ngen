#pragma once

#include "debugview.h"
#include "drawlists.h"
#include "framegraph.h"
#include "gpuscene.h"
#include "rhitypes.h"

#include <cstdint>
#include <vector>

class RhiDevice;

struct DebugViewPassData {
    FgTextureHandle value; // RGBA32F raw values (debugview.frag), read back by the cursor readout
    FgTextureHandle color; // RGBA16F colours (debugview.comp), blitted in place of the lit image
};

// Viewport debug views: draws the camera's indirect commands again through a geometry
// shader, writing one raw value per pixel, then colours the values in a compute pass.
// Only added to the graph while a view is on.
class DebugViewPass {
public:
    // Uses the geometry pass's set 0 layout (UBO, textures, instances, materials).
    auto init(RhiDevice* device, uint32_t frameCount, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool;
    auto destroy(RhiDevice* device) -> void;
    // False when the device lacks geometry shaders or the formats; the views are then off.
    auto available() const -> bool { return supported; }

    auto addPass(
        FrameGraph& fg,
        DebugView view,
        FgTextureHandle depthHandle,
        RhiExtent2D extent,
        uint32_t frameSlot,
        FgBufferHandle instanceBuffer,
        const DrawLists& lists,
        DrawLists::Handles drawHandles,
        const GpuScene& scene,
        RhiDescriptorSet* geometrySet) -> const DebugViewPassData&;

private:
    RhiDevice* device = nullptr;
    bool supported = false;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* geomShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
    RhiShaderModule* resolveShader = nullptr;
    // [doubleSided]: depth-tested views, and the overdraw view (no depth, additive).
    RhiPipeline* pipelines[2] = {};
    RhiPipeline* overdrawPipelines[2] = {};
    RhiDescriptorSetLayout* resolveLayout = nullptr;
    RhiPipeline* resolvePipeline = nullptr;
    RhiDescriptorPool* resolvePool = nullptr;
    std::vector<RhiDescriptorSet*> resolveSets; // one per frame slot; rewritten at execute
    RhiSampler* pointSampler = nullptr;
};
