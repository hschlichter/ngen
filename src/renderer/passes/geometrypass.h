#pragma once

#include "framegraph.h"
#include "rhitypes.h"
#include "scenehandles.h"

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>

class RhiDevice;

struct CachedMesh {
    RhiBuffer* vertexBuffer = nullptr;
    RhiBuffer* indexBuffer = nullptr;
    uint32_t indexCount = 0;
};

struct GpuInstance {
    MeshHandle mesh;
    MaterialHandle material;
    glm::mat4 transform;
};

struct GeometryPassData {
    FgTextureHandle albedo;
    FgTextureHandle normal;
    FgTextureHandle depth;
};

class GeometryPass {
public:
    auto init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(FrameGraph& fg,
                 FgTextureHandle depthHandle,
                 RhiExtent2D extent,
                 uint32_t imageIndex,
                 uint32_t instanceCount,
                 std::span<const GpuInstance> instances,
                 const std::unordered_map<uint32_t, CachedMesh>& meshCache,
                 std::span<RhiDescriptorSet*> descriptorSets) -> const GeometryPassData&;

    auto descriptorSetLayout() const -> RhiDescriptorSetLayout* { return descSetLayout; }

private:
    RhiPipeline* pipeline = nullptr;
    RhiDescriptorSetLayout* descSetLayout = nullptr;
    RhiShaderModule* vertShader = nullptr;
    RhiShaderModule* fragShader = nullptr;
};
