#include "depthprepass.h"

#include "mesh.h"
#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>

auto DepthPrepass::init(PipelineRegistry& pipelines, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool {
    using enum RhiFormat;
    registry = &pipelines;

    GraphicsPipelineRequest request = {
        .vertex = {.id = "shaders/depthonly.vert", .stage = RhiShaderStage::Vertex},
        .fragment = {.id = "shaders/shadow.frag", .stage = RhiShaderStage::Fragment},
        .descriptorSetLayouts = {geometrySetLayout},
        .pushConstant = {}, // model matrix comes from the instance buffer at gl_InstanceIndex
        .colorFormats = {},
        .depthFormat = depthFormat,
        .vertexStride = sizeof(std::array<float, 3>),
        .vertexAttributes = {{.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = 0}}, // position-only stream
        .raster = {.cullMode = RhiCullMode::Back},
    };
    pipelineCullBack = pipelines.registerGraphics("prepass.pipeline.cullback", request, PipelineUse::Startup);
    request.raster.cullMode = RhiCullMode::None;
    pipelineCullNone = pipelines.registerGraphics("prepass.pipeline.cullnone", request, PipelineUse::Startup);
    return true;
}

auto DepthPrepass::addPass(
    FrameGraph& fg,
    FgTextureHandle depthHandle,
    RhiExtent2D extent,
    std::span<const GpuInstance> instances,
    FgBufferHandle instanceBuffer,
    const DrawLists& lists,
    DrawLists::Handles drawHandles,
    const GpuScene& scene,
    RhiDescriptorSet* descriptorSet) -> const DepthPrepassData& {
    auto* cullBack = registry->get(pipelineCullBack);
    auto* cullNone = registry->get(pipelineCullNone);

    return fg.addPass<DepthPrepassData>(
        "DepthPrepass",
        [&](FrameGraphBuilder& builder, DepthPrepassData& data) {
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            builder.read(drawHandles.commands, FgAccessFlags::IndirectRead);
            builder.read(drawHandles.counts, FgAccessFlags::IndirectRead);
            builder.setSideEffects(true);
        },
        [cullBack, cullNone, extent, instances, &lists, drawHandles, &scene, descriptorSet](FrameGraphContext& ctx, const DepthPrepassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.depth),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo info = {
                .extent = extent,
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(info);
            cmd->setViewport(extent);
            cmd->setScissor(extent);

            // The camera regions the geometry pass draws, one indirect call per bucket; the
            // position pool shares the vertex pool's vertexOffset.
            auto* commands = ctx.buffer(drawHandles.commands);
            auto* counts = ctx.buffer(drawHandles.counts);
            for (bool doubleSided : {false, true}) {
                auto region = DrawLists::cameraRegion(doubleSided);
                // The GPU-written count decides how many commands draw; an empty region costs one call.
                if (scene.indexBuffer() == nullptr) {
                    continue;
                }
                auto* pip = doubleSided ? cullNone : cullBack;
                cmd->bindPipeline(pip);
                cmd->bindDescriptorSet(pip, 0, descriptorSet);
                cmd->bindVertexBuffer(scene.positionBuffer());
                cmd->bindIndexBuffer(scene.indexBuffer(), RhiIndexType::Uint32);
                PROFILE_GPU_ZONE(cmd, DrawLists::regionName(region));
                cmd->drawIndexedIndirectCount(commands, lists.commandOffset(region), counts, DrawLists::countOffset(region), lists.regionCapacity());
                lists.report(ctx, region, instances);
            }

            cmd->endRendering();
        });
}
