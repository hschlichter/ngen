#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <cstdint>
#include <vector>

class RhiDevice;

struct AAPassData {
    FgTextureHandle sceneColor;
    FgTextureHandle sceneColorAA;
};

// FXAA as a compute pass: samples sceneColor, writes sceneColorAA through a
// storage image. First compute node in the frame graph. Output is a linear
// float target (storage images cannot be sRGB); the blit to the sRGB backbuffer
// converts. Overlays draw on the backbuffer after that blit, never here.
//
// If the device cannot use the float format as a storage image the pass falls
// back to the old blit and outputFormat() reports the input format instead.
class AAPass {
public:
    auto init(RhiDevice* device, uint32_t frameCount, RhiFormat inputFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg, FgTextureHandle input, RhiExtent2D extent, uint32_t frameSlot, RhiSampler* sampler, bool enabled) -> const AAPassData&;

    // Format of sceneColorAA.
    auto outputFormat() const -> RhiFormat { return format; }
    auto isCompute() const -> bool { return computeSupported; }

private:
    RhiDevice* device = nullptr;
    RhiFormat format = RhiFormat::Undefined;
    bool computeSupported = false;
    RhiShaderModule* shader = nullptr;
    RhiDescriptorSetLayout* setLayout = nullptr;
    RhiPipeline* pipeline = nullptr;
    RhiDescriptorPool* pool = nullptr;
    std::vector<RhiDescriptorSet*> sets; // one per frame slot; rewritten at execute
};
