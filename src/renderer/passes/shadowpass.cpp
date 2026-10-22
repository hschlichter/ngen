#include "shadowpass.h"
#include "renderertypes.h"

#include "deletionqueue.h"
#include "mesh.h"
#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <array>
#include <format>

namespace {
struct ShadowPush {
    glm::mat4 lightViewProj;
};

constexpr std::array<RhiDescriptorBinding, 1> instanceBindings = {{
    {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Vertex},
}};
} // namespace

auto ShadowPass::init(RhiDevice* device, PipelineRegistry& pipelines, RhiExtent2D extent, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool {
    using enum RhiFormat;
    registry = &pipelines;

    descSetLayout = device->createDescriptorSetLayout(instanceBindings);
    device->setDebugName(descSetLayout, "shadow.setlayout");

    GraphicsPipelineRequest request = {
        .vertex = {.id = "shaders/shadow.vert", .stage = RhiShaderStage::Vertex},
        .fragment = {.id = "shaders/shadow.frag", .stage = RhiShaderStage::Fragment},
        .descriptorSetLayouts = {descSetLayout}, // instance buffer; the cascade matrix is a push constant
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(ShadowPush)},
        .colorFormats = {}, // depth-only
        .depthFormat = depthFormat,
        .vertexStride = sizeof(std::array<float, 3>),
        .vertexAttributes = {{.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = 0}}, // position-only stream
        .raster = {.cullMode = RhiCullMode::Back},
    };
    this->pipelines[DrawLists::bucket(false, false)] = pipelines.registerGraphics("shadow.pipeline.cullback", request, PipelineUse::AtStart);
    request.raster.cullMode = RhiCullMode::None;
    this->pipelines[DrawLists::bucket(true, false)] = pipelines.registerGraphics("shadow.pipeline.cullnone", request, PipelineUse::AtStart);

    // Cutout: the texture coordinates too, from the full vertex stream, and the geometry set for the material's opacity.
    request.vertex.id = "shaders/shadowcutout.vert";
    request.fragment.id = "shaders/cutout.frag";
    request.descriptorSetLayouts = {geometrySetLayout};
    request.vertexStride = sizeof(Vertex);
    request.vertexAttributes = {
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, position)},
        {.location = 3, .binding = 0, .format = R32G32_SFLOAT, .offset = offsetof(struct Vertex, texCoord)},
    };
    request.raster.cullMode = RhiCullMode::Back;
    this->pipelines[DrawLists::bucket(false, true)] = pipelines.registerGraphics("shadow.pipeline.cullback.cutout", request, PipelineUse::AtStart);
    request.raster.cullMode = RhiCullMode::None;
    this->pipelines[DrawLists::bucket(true, true)] = pipelines.registerGraphics("shadow.pipeline.cullnone.cutout", request, PipelineUse::AtStart);
    return true;
}

auto ShadowPass::bindInstanceBuffer(RhiDevice* device, RhiBuffer* instanceBuffer, DeletionQueue& deletionQueue, uint64_t frame) -> void {
    if (descPool != nullptr) {
        deletionQueue.defer(frame, [device, pool = descPool, set = descSet] {
            device->freeDescriptorSets(pool, {&set, 1});
            device->destroyDescriptorPool(pool);
        });
    }
    descPool = device->createDescriptorPool(1, instanceBindings);
    device->setDebugName(descPool, "shadow.sets.pool");
    device->allocateDescriptorSets(descPool, descSetLayout, {&descSet, 1});
    device->setDebugName(descSet, "shadow.set");
    std::array<RhiDescriptorWrite, 1> writes = {{
        {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .buffer = instanceBuffer, .bufferRange = 0},
    }};
    device->updateDescriptorSet(descSet, writes);
}

auto ShadowPass::destroy(RhiDevice* device) -> void {
    if (descPool != nullptr) {
        device->freeDescriptorSets(descPool, {&descSet, 1});
        device->destroyDescriptorPool(descPool);
    }
    device->destroyDescriptorSetLayout(descSetLayout);
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
    RhiDescriptorSet* geometrySet) -> const ShadowPassData& {
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
    auto* instanceSet = descSet;
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
            builder.read(drawHandles.commands, FgAccessFlags::IndirectRead);
            builder.read(drawHandles.counts, FgAccessFlags::IndirectRead);
        },
        [bucketPipelines, instanceSet, geometrySet, atlasExtent, cascadeCopy, cascadeCount, instances, &lists, drawHandles, &scene](FrameGraphContext& ctx, const ShadowPassData& data) {
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
                    if (scene.indexBuffer() == nullptr || (cutout && geometrySet == nullptr)) {
                        continue;
                    }
                    auto* pip = bucketPipelines[bucket];
                    cmd->bindPipeline(pip);
                    cmd->bindDescriptorSet(pip, 0, cutout ? geometrySet : instanceSet);
                    cmd->bindVertexBuffer(cutout ? scene.vertexBuffer() : scene.positionBuffer());
                    cmd->bindIndexBuffer(scene.indexBuffer(), RhiIndexType::Uint32);
                    ShadowPush push{cascade.viewProj};
                    cmd->pushConstants(pip, RhiShaderStage::Vertex, 0, sizeof(push), &push);
                    PROFILE_GPU_ZONE(cmd, DrawLists::regionName(region));
                    cmd->drawIndexedIndirectCount(ctx.buffer(drawHandles.commands), lists.commandOffset(region), ctx.buffer(drawHandles.counts), DrawLists::countOffset(region), lists.regionCapacity(region));
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
