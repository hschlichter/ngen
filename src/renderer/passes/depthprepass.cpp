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
        .depth = {.testEnable = true, .writeEnable = true, .compareOp = RhiCompareOp::Greater}, // reversed-Z
    };
    this->pipelines[DrawLists::bucket(false, false)] = pipelines.registerGraphics("prepass.pipeline.cullback", request, PipelineUse::AtStart);
    request.raster.cullMode = RhiCullMode::None;
    this->pipelines[DrawLists::bucket(true, false)] = pipelines.registerGraphics("prepass.pipeline.cullnone", request, PipelineUse::AtStart);

    // Cutout: the texture coordinates too, from the full vertex stream.
    request.vertex.id = "shaders/depthcutout.vert";
    request.fragment.id = "shaders/cutout.frag";
    request.vertexStride = sizeof(Vertex);
    request.vertexAttributes = {
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, position)},
        {.location = 3, .binding = 0, .format = R32G32_SFLOAT, .offset = offsetof(struct Vertex, texCoord)},
    };
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
    std::span<const GpuInstance> instances,
    FgBufferHandle instanceBuffer,
    const DrawLists& lists,
    DrawLists::Handles drawHandles,
    const GpuScene& scene,
    RhiDescriptorSet* descriptorSet) -> const DepthPrepassData& {
    std::array<RhiPipeline*, DrawLists::bucketCount> bucketPipelines = {};
    for (uint32_t bucket = 0; bucket < DrawLists::bucketCount; bucket++) {
        bucketPipelines[bucket] = registry->get(pipelines[bucket]);
    }

    return fg.addPass<DepthPrepassData>(
        "DepthPrepass",
        [&](FrameGraphBuilder& builder, DepthPrepassData& data) {
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            builder.read(drawHandles.commands, FgAccessFlags::IndirectRead);
            builder.read(drawHandles.counts, FgAccessFlags::IndirectRead);
            builder.setSideEffects(true);
        },
        [bucketPipelines, extent, instances, &lists, drawHandles, &scene, descriptorSet](FrameGraphContext& ctx, const DepthPrepassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.depth),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 0.0f, // reversed-Z: 0 is infinitely far
            };
            RhiRenderingInfo info = {
                .extent = extent,
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(info);
            cmd->setViewport(extent);
            cmd->setScissor(extent);

            // The camera regions the geometry pass draws, one indirect call per bucket; the
            // position pool shares the vertex pool's vertexOffset. Cutout buckets read the vertex pool.
            auto* commands = ctx.buffer(drawHandles.commands);
            auto* counts = ctx.buffer(drawHandles.counts);
            for (uint32_t bucket = 0; bucket < DrawLists::bucketCount; bucket++) {
                bool cutout = bucket >= 2;
                auto region = DrawLists::cameraRegion(bucket % 2 != 0, cutout);
                // The GPU-written count decides how many commands draw; an empty region costs one call.
                if (scene.indexBuffer() == nullptr) {
                    continue;
                }
                auto* pip = bucketPipelines[bucket];
                cmd->bindPipeline(pip);
                cmd->bindDescriptorSet(pip, 0, descriptorSet);
                cmd->bindVertexBuffer(cutout ? scene.vertexBuffer() : scene.positionBuffer());
                cmd->bindIndexBuffer(scene.indexBuffer(), RhiIndexType::Uint32);
                PROFILE_GPU_ZONE(cmd, DrawLists::regionName(region));
                cmd->drawIndexedIndirectCount(commands, lists.commandOffset(region), counts, DrawLists::countOffset(region), lists.regionCapacity());
                lists.report(ctx, region, instances);
            }

            cmd->endRendering();
        });
}

auto DepthPrepass::ready() -> bool {
    const auto& handles = pipelines;
    return registry->ready(handles);
}
