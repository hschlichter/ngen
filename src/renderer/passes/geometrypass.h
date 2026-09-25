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
    RhiBuffer* vertexBuffer = nullptr;   // full Vertex stream, geometry pass
    RhiBuffer* positionBuffer = nullptr; // positions only, shadow and depth prepass (docs/plan_position_stream.md)
    RhiBuffer* indexBuffer = nullptr;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;
};

struct GpuInstance {
    MeshHandle mesh;
    MaterialHandle material;
    uint32_t prim = 0; // PrimHandle::index of the source prim, for the render debugger
    glm::mat4 transform;
    // Submesh index range within the mesh; primFirst marks the first submesh
    // instance of a prim (the shadow pass draws the whole mesh once, there).
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    bool primFirst = true;
    bool doubleSided = false; // drawn with the cull-none pipeline
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

    auto addPass(
        FrameGraph& fg,
        FgTextureHandle depthHandle,
        RhiExtent2D extent,
        uint32_t imageIndex,
        uint32_t instanceCount,
        std::span<const GpuInstance> instances,
        FgBufferHandle instanceBuffer,
        std::span<const uint8_t> visible,
        const std::unordered_map<uint32_t, CachedMesh>& meshCache,
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
