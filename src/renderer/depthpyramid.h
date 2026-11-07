#pragma once

#include "framegraphresource.h"
#include "pipelineregistry.h"
#include "rhitypes.h"

#include <cstdint>
#include <vector>

class FrameGraph;
class RhiDevice;

// The depth pyramid for occlusion culling (src/renderer/README.md, "Occlusion culling"): R32_SFLOAT, mip 0 the depth buffer's size rounded
// down to powers of two, every level to 1x1, each texel the farthest depth (the minimum, reversed-Z) of what it covers. Built by
// shaders/hzbbuild.comp, a dispatch per level: HzbBuild from occlusion's first phase, for the second phase's tests. By default that phase is
// the geometry pass's and the pyramid is built once a frame; with a depth prepass it is the prepass's, and HzbBuildFinal rebuilds the
// pyramid from the prepass's whole depth for the geometry pass's meshlets. It reads one of two depth buffers of the same size: the
// rendered camera's, or the culling camera's own while that isn't the rendered one (a debug camera, a frozen frustum). Kept across frames
// and remade with the depth buffers; imported into the graph each frame, so its contents start undefined.
class DepthPyramid {
public:
    auto init(RhiDevice* device, PipelineRegistry& pipelines) -> bool;
    auto destroy() -> void;

    // The depth buffer a build reads: the rendered camera's, or the culling camera's own.
    enum class Source : uint32_t {
        View = 0,
        Culling = 1,
    };
    // Remakes the texture and its per-level descriptor sets for depth buffers of this size, reading `viewDepth` or `cullDepth`. The caller
    // has waited for the device (the swapchain and depth buffers are remade with it).
    auto resize(RhiExtent2D depthExtent, RhiTexture* viewDepth, RhiTexture* cullDepth) -> void;

    // Imports the pyramid into the frame's graph; every pass whose shaders bind it reads it through this handle.
    auto import(FrameGraph& fg) -> FgTextureHandle;
    // Adds a build pass named `name`: reads `depth`, which is the `source` buffer, writes every level of the pyramid.
    auto addBuildPass(FrameGraph& fg, const char* name, Source source, FgTextureHandle depth, FgTextureHandle pyramid) -> void;

    auto texture() const -> RhiTexture* { return pyramid; }
    auto sampler() const -> RhiSampler* { return pointSampler; }
    auto width() const -> uint32_t { return size.width; }
    auto height() const -> uint32_t { return size.height; }
    auto mipCount() const -> uint32_t { return mips; }

    // Whether its pipeline has been created; until then occlusion is off.
    auto ready() -> bool;

private:
    auto releaseTexture() -> void;

    RhiDevice* device = nullptr;
    PipelineRegistry* registry = nullptr;
    PipelineHandle pipelineHandle;
    RhiDescriptorSetLayout* setLayout = nullptr;
    RhiSampler* pointSampler = nullptr;
    RhiTexture* pyramid = nullptr;
    RhiExtent2D size = {0, 0};
    RhiExtent2D depthSize = {0, 0};
    uint32_t mips = 0;
    RhiDescriptorPool* pool = nullptr;
    // Per level, for each source: every set names its source's depth (a bound image must be in the layout its descriptor names, read or not).
    std::vector<RhiDescriptorSet*> sets;
    std::vector<RhiDescriptorSet*> cullSets;
};
