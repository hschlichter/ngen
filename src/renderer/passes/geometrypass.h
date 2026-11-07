#pragma once

#include "drawlists.h"
#include "framegraph.h"
#include "gpuscene.h"
#include "pipelineregistry.h"
#include "rhitypes.h"
#include "scenehandles.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <unordered_map>

class RhiDevice;

struct GeometryPassData {
    FgTextureHandle albedo;
    FgTextureHandle normal;
    FgTextureHandle instance; // R32_UINT: the instance index + 1 per pixel, 0 where nothing is drawn (picking reads it)
    FgTextureHandle depth;
};

// Which of occlusion's phases a geometry pass draws. Both: every camera region, after a depth prepass that ran both phases (the meshlets
// tested against the pyramid of its whole depth), or without occlusion. First: the regions of what was visible last frame, clearing the
// targets, no pyramid test (it is built from this pass's depth). Second: what the pyramid newly shows, onto the first pass's targets.
enum class GeometryPhase {
    Both,
    First,
    Second,
};

class GeometryPass {
public:
    auto init(RhiDevice* device, PipelineRegistry& pipelines, RhiExtent2D extent, RhiFormat depthFormat) -> bool;

    // Set 0, one set per frame slot, for every mesh pass: 0 view UBO, 1 material texture array (GpuScene::maxTextures), 2 instance buffer,
    // 3 material table, 4-9 the geometry pool (vertices, positions, meshlets, meshlet vertices, meshlet triangles, mesh table), 10-12 the
    // slot's meshlet work items, culling parameters and meshlet counts, 13 the depth pyramid, 14 the slot's region draws, 15 the render-LOD
    // state (shaders/meshlet.glsl).
    static auto descriptorBindings() -> std::array<RhiDescriptorBinding, 16>;
    auto destroy(RhiDevice* device) -> void;

    auto addPass(
        FrameGraph& fg,
        FgTextureHandle depthHandle,
        RhiExtent2D extent,
        std::span<const GpuInstance> instances,
        FgBufferHandle instanceBuffer,
        const DrawLists& lists,
        DrawLists::Handles drawHandles,
        const GpuScene& scene,
        RhiDescriptorSet* descriptorSet,
        bool depthPrepassed,
        FgTextureHandle hzb,
        bool occlusion,
        GeometryPhase phase,
        const GeometryPassData* first) -> const GeometryPassData&;

    auto normalFormat() const -> RhiFormat { return normalTargetFormat; }

    auto descriptorSetLayout() const -> RhiDescriptorSetLayout* { return descSetLayout; }

    // Whether its pipelines have been created; until then the frame leaves the pass out.
    auto ready() -> bool;

private:
    PipelineRegistry* registry = nullptr;
    // [cutout][doubleSided][depthPrepassed]: gbuffer.frag or gbuffercutout.frag, cull back or none, Greater-with-write (reversed-Z) or Equal-without.
    PipelineHandle pipelines[2][2][2] = {};
    RhiFormat normalTargetFormat = RhiFormat::R16G16B16A16_SFLOAT;
    RhiDescriptorSetLayout* descSetLayout = nullptr;
};
