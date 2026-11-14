#include "depthprepass.h"

#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>

auto DepthPrepass::init(PipelineRegistry& pipelines, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool {
    using enum RhiFormat;
    registry = &pipelines;

    // Task and mesh shaders pull positions from the pool (meshlet.task, depth.mesh); cutout ones the texture coordinate too.
    GraphicsPipelineRequest request = {
        .fragment = {.id = "shaders/shadow.frag", .stage = RhiShaderStage::Fragment},
        .task = ShaderRef{.id = "shaders/meshlet.task", .stage = RhiShaderStage::Task},
        .mesh = ShaderRef{.id = "shaders/depth.mesh", .stage = RhiShaderStage::Mesh},
        .descriptorSetLayouts = {geometrySetLayout},
        .pushConstant = {.stage = RhiShaderStage::Task | RhiShaderStage::Mesh, .offset = 0, .size = sizeof(MeshletPush)},
        .colorFormats = {},
        .depthFormat = depthFormat,
        .raster = {.cullMode = RhiCullMode::Back},
        .depth = {.testEnable = true, .writeEnable = true, .compareOp = RhiCompareOp::Greater}, // reversed-Z
    };
    this->pipelines[DrawLists::bucket(false, false)] = pipelines.registerGraphics("prepass.pipeline.cullback", request, PipelineUse::AtStart);
    request.raster.cullMode = RhiCullMode::None;
    this->pipelines[DrawLists::bucket(true, false)] = pipelines.registerGraphics("prepass.pipeline.cullnone", request, PipelineUse::AtStart);

    request.mesh = ShaderRef{.id = "shaders/depthcutout.mesh", .stage = RhiShaderStage::Mesh};
    request.fragment.id = "shaders/cutout.frag";
    request.raster.cullMode = RhiCullMode::Back;
    this->pipelines[DrawLists::bucket(false, true)] = pipelines.registerGraphics("prepass.pipeline.cullback.cutout", request, PipelineUse::AtStart);
    request.raster.cullMode = RhiCullMode::None;
    this->pipelines[DrawLists::bucket(true, true)] = pipelines.registerGraphics("prepass.pipeline.cullnone.cutout", request, PipelineUse::AtStart);
    return true;
}

auto DepthPrepass::addPass(
    FrameGraph& fg,
    FgTextureHandle depthHandle,
    RhiExtent2D extent,
    const ChunkedList<GpuInstance>& instances,
    FgBufferHandle instanceBuffer,
    const DrawLists& lists,
    DrawLists::Handles drawHandles,
    const GpuScene& scene,
    RhiDescriptorSet* descriptorSet,
    FgTextureHandle hzb,
    bool late,
    bool cullCamera) -> const DepthPrepassData& {
    std::array<RhiPipeline*, DrawLists::bucketCount> bucketPipelines = {};
    for (uint32_t bucket = 0; bucket < DrawLists::bucketCount; bucket++) {
        bucketPipelines[bucket] = registry->get(pipelines[bucket]);
    }

    return fg.addPass<DepthPrepassData>(
        cullCamera ? (late ? "DepthPrepassCullLate" : "DepthPrepassCull") : (late ? "DepthPrepassLate" : "DepthPrepass"),
        profile::Group::Geometry,
        [&](FrameGraphBuilder& builder, DepthPrepassData& data) {
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.read(hzb, FgAccessFlags::ShaderRead);
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            // One draw per region: its RegionWork is the draw's command and the task shaders' range of work items.
            builder.read(drawHandles.regionWork, FgAccessFlags::IndirectRead | FgAccessFlags::StorageRead);
            builder.read(drawHandles.workItems, FgAccessFlags::StorageRead);
            if (drawHandles.lodStates.valid()) {
                builder.read(drawHandles.lodStates, FgAccessFlags::StorageRead);
            }
            builder.setSideEffects(true);
        },
        [bucketPipelines, extent, instances = &instances, &lists, drawHandles, &scene, descriptorSet, late, cullCamera](FrameGraphContext& ctx, const DepthPrepassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.depth),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = !late,     // the late phase adds to the first phase's depth
                .clearDepth = 0.0f, // reversed-Z: 0 is infinitely far
            };
            RhiRenderingInfo info = {
                .extent = extent,
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(info);
            cmd->setViewport(extent);
            cmd->setScissor(extent);

            // The camera regions the geometry pass draws, one indirect call per bucket, culling the same meshlets (it doesn't count them).
            auto* regionWork = ctx.buffer(drawHandles.regionWork);
            for (uint32_t bucket = 0; bucket < DrawLists::bucketCount; bucket++) {
                bool cutout = bucket >= 2;
                auto region = late ? DrawLists::cameraLateRegion(bucket % 2 != 0, cutout) : DrawLists::cameraRegion(bucket % 2 != 0, cutout);
                // The GPU-written count decides how many commands draw; an empty region costs one call.
                if (!scene.poolReady() || descriptorSet == nullptr) {
                    continue;
                }
                auto* pip = bucketPipelines[bucket];
                cmd->bindPipeline(pip);
                cmd->bindDescriptorSet(pip, 0, descriptorSet);
                // The late phase's meshlets are tested against the depth pyramid, as the geometry pass tests them.
                uint32_t flags = late ? meshletFlagOcclusion : 0u;
                if (cullCamera) {
                    flags |= meshletFlagCullCamera;
                }
                MeshletPush push = {.region = region, .flags = flags};
                cmd->pushConstants(pip, RhiShaderStage::Task | RhiShaderStage::Mesh, 0, sizeof(push), &push);
                PROFILE_GPU_ZONE(cmd, DrawLists::drawName(region), Geometry);
                DrawLists::drawRegion(cmd, regionWork, region);
                lists.report(ctx, region, *instances);
            }

            cmd->endRendering();
        });
}

auto DepthPrepass::ready() -> bool {
    const auto& handles = pipelines;
    return registry->ready(handles);
}
