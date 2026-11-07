#include "shadowpass.h"
#include "renderertypes.h"

#include "deletionqueue.h"
#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <array>
#include <format>

namespace {
// MeshletPush, then the cascade's matrix (shadow.mesh's push constants).
struct ShadowPush {
    MeshletPush meshlet;
    uint32_t pad[2] = {};
    glm::mat4 lightViewProj;
};
static_assert(offsetof(ShadowPush, lightViewProj) == 16);
} // namespace

auto ShadowPass::init(PipelineRegistry& pipelines, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool {
    registry = &pipelines;

    // The geometry set for every bucket: the task and mesh shaders pull positions (and for cutout the texture coordinates) from the pool.
    GraphicsPipelineRequest request = {
        .fragment = {.id = "shaders/shadow.frag", .stage = RhiShaderStage::Fragment},
        .task = ShaderRef{.id = "shaders/meshlet.task", .stage = RhiShaderStage::Task},
        .mesh = ShaderRef{.id = "shaders/shadow.mesh", .stage = RhiShaderStage::Mesh},
        .descriptorSetLayouts = {geometrySetLayout},
        .pushConstant = {.stage = RhiShaderStage::Task | RhiShaderStage::Mesh, .offset = 0, .size = sizeof(ShadowPush)},
        .colorFormats = {}, // depth-only
        .depthFormat = depthFormat,
        .raster = {.cullMode = RhiCullMode::Back},
    };
    this->pipelines[DrawLists::bucket(false, false)] = pipelines.registerGraphics("shadow.pipeline.cullback", request, PipelineUse::AtStart);
    request.raster.cullMode = RhiCullMode::None;
    this->pipelines[DrawLists::bucket(true, false)] = pipelines.registerGraphics("shadow.pipeline.cullnone", request, PipelineUse::AtStart);

    // Cutout: the texture coordinates and material too, for the opacity.
    request.mesh = ShaderRef{.id = "shaders/shadowcutout.mesh", .stage = RhiShaderStage::Mesh};
    request.fragment.id = "shaders/cutout.frag";
    request.raster.cullMode = RhiCullMode::Back;
    this->pipelines[DrawLists::bucket(false, true)] = pipelines.registerGraphics("shadow.pipeline.cullback.cutout", request, PipelineUse::AtStart);
    request.raster.cullMode = RhiCullMode::None;
    this->pipelines[DrawLists::bucket(true, true)] = pipelines.registerGraphics("shadow.pipeline.cullnone.cutout", request, PipelineUse::AtStart);
    return true;
}

auto ShadowPass::addPass(
    FrameGraph& fg,
    RhiExtent2D atlasExtent,
    RhiFormat depthFormat,
    std::span<const ShadowCascade> cascades,
    std::span<const GpuInstance> instances,
    FgBufferHandle instanceBuffer,
    const DrawLists& lists,
    DrawLists::Handles drawHandles,
    const GpuScene& scene,
    RhiDescriptorSet* geometrySet,
    FgTextureHandle hzb) -> const ShadowPassData& {
    FgTextureDesc desc = {
        .width = atlasExtent.width,
        .height = atlasExtent.height,
        .format = depthFormat,
        .usage = RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled,
    };

    std::array<RhiPipeline*, DrawLists::bucketCount> bucketPipelines = {};
    for (uint32_t bucket = 0; bucket < DrawLists::bucketCount; bucket++) {
        bucketPipelines[bucket] = registry->get(pipelines[bucket]);
    }
    // Copies: the execute lambda runs later in the frame, after the snapshot may be gone.
    std::array<ShadowCascade, maxShadowCascades> cascadeCopy;
    uint32_t cascadeCount = (uint32_t) std::min(cascades.size(), (size_t) maxShadowCascades);
    for (uint32_t c = 0; c < cascadeCount; c++) {
        cascadeCopy[c] = cascades[c];
    }

    return fg.addPass<ShadowPassData>(
        "ShadowPass",
        [&](FrameGraphBuilder& builder, ShadowPassData& data) {
            data.shadowMap = builder.write(builder.createTexture("shadowMap", desc), FgAccessFlags::DepthAttachment);
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            // One draw per region: its RegionWork is the draw's command and the task shaders' range of work items.
            builder.read(drawHandles.regionWork, FgAccessFlags::IndirectRead | FgAccessFlags::StorageRead);
            builder.read(drawHandles.workItems, FgAccessFlags::StorageRead);
            // The cascades' meshlet counts come from this pass.
            builder.write(drawHandles.meshletStats, FgAccessFlags::StorageWrite);
            // Bound with the geometry set; the cascades don't test occlusion.
            builder.read(hzb, FgAccessFlags::ShaderRead);
            if (drawHandles.lodStates.valid()) {
                builder.read(drawHandles.lodStates, FgAccessFlags::StorageRead);
            }
        },
        [bucketPipelines, geometrySet, atlasExtent, cascadeCopy, cascadeCount, instances, &lists, drawHandles, &scene](FrameGraphContext& ctx, const ShadowPassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.shadowMap),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo info = {
                .extent = atlasExtent,
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(info);

            for (uint32_t c = 0; c < cascadeCount; c++) {
                const auto& cascade = cascadeCopy[c];
                // Tile viewport and scissor from the cascade's atlas rect.
                int32_t tileX = (int32_t) (cascade.atlasRect.x * (float) atlasExtent.width);
                int32_t tileY = (int32_t) (cascade.atlasRect.y * (float) atlasExtent.height);
                RhiExtent2D tile = {(uint32_t) (cascade.atlasRect.z * (float) atlasExtent.width), (uint32_t) (cascade.atlasRect.w * (float) atlasExtent.height)};
                cmd->setViewport(tileX, tileY, tile);
                cmd->setScissor(tileX, tileY, tile);

                // One indirect call per bucket per cascade: single-sided meshes under back-face
                // culling first, then double-sided ones without it, then the same two cutout.
                // Commands draw whole meshes on each prim's first submesh instance, except for
                // prims with a cutout submesh, which draw submesh by submesh.
                for (uint32_t bucket = 0; bucket < DrawLists::bucketCount; bucket++) {
                    bool cutout = bucket >= 2;
                    auto region = DrawLists::cascadeRegion(c, bucket % 2 != 0, cutout);
                    // The GPU-written count decides how many commands draw; an empty region costs one call.
                    if (!scene.poolReady() || geometrySet == nullptr) {
                        continue;
                    }
                    auto* pip = bucketPipelines[bucket];
                    cmd->bindPipeline(pip);
                    cmd->bindDescriptorSet(pip, 0, geometrySet);
                    ShadowPush push = {.meshlet = {.region = region, .flags = meshletFlagCount}, .lightViewProj = cascade.viewProj};
                    cmd->pushConstants(pip, RhiShaderStage::Task | RhiShaderStage::Mesh, 0, sizeof(push), &push);
                    PROFILE_GPU_ZONE(cmd, DrawLists::regionName(region));
                    DrawLists::drawRegion(cmd, ctx.buffer(drawHandles.regionWork), region);
                    lists.report(ctx, region, instances);
                }
            }

            cmd->endRendering();
        });
}

auto ShadowPass::ready() -> bool {
    const auto& handles = pipelines;
    return registry->ready(handles);
}
