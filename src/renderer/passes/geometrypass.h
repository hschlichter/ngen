#pragma once

#include "framegraph.h"
#include "gpuscene.h"
#include "rhitypes.h"
#include "scenehandles.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>

class RhiDevice;

struct GeometryPassData {
    FgTextureHandle albedo;
    FgTextureHandle normal;
    FgTextureHandle depth;
};

class GeometryPass {
public:
    auto init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(
        FrameGraph& fg,
        FgTextureHandle depthHandle,
        RhiExtent2D extent,
        uint32_t imageIndex,
        uint32_t instanceCount,
        std::span<const GpuInstance> instances,
        FgBufferHandle instanceBuffer,
        std::span<const uint8_t> visible,
        const GpuScene& scene,
        std::span<RhiDescriptorSet*> descriptorSets,
        bool depthPrepassed) -> const GeometryPassData&;

    auto normalFormat() const -> RhiFormat { return normalTargetFormat; }

    auto descriptorSetLayout() const -> RhiDescriptorSetLayout* { return descSetLayout; }

private:
    // [doubleSided][depthPrepassed]: cull back or none, Less-with-write or Equal-without.
    RhiPipeline* pipelines[2][2] = {};
    RhiFormat normalTargetFormat = RhiFormat::R16G16B16A16_SFLOAT;
    RhiDescriptorSetLayout* descSetLayout = nullptr;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
};
