#include "shadowpass.h"
#include "renderertypes.h"

#include "deletionqueue.h"
#include "mesh.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "shaderloader.h"

#include <algorithm>
#include <array>

namespace {
struct ShadowPush {
    glm::mat4 lightViewProj;
};

constexpr std::array<RhiDescriptorBinding, 1> instanceBindings = {{
    {.binding = 0, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Vertex},
}};
} // namespace

auto ShadowPass::init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool {
    using enum RhiFormat;

    vertShader = loadShaderModule(device, RhiShaderStage::Vertex, "shaders/shadow.vert.spv");
    fragShader = loadShaderModule(device, RhiShaderStage::Fragment, "shaders/shadow.frag.spv");

    std::array<RhiVertexAttribute, 1> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = 0}, // position-only stream
    }};

    descSetLayout = device->createDescriptorSetLayout(instanceBindings);

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayouts = {&descSetLayout, 1}, // instance buffer; the cascade matrix is a push constant
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(ShadowPush)},
        .colorFormats = {}, // depth-only
        .depthFormat = depthFormat,
        .vertexStride = sizeof(std::array<float, 3>),
        .vertexAttributes = vertexAttrs,
        .raster = {.cullMode = RhiCullMode::Back},
    };
    pipelineCullBack = device->createGraphicsPipeline(pipelineDesc);
    pipelineDesc.raster.cullMode = RhiCullMode::None;
    pipelineCullNone = device->createGraphicsPipeline(pipelineDesc);
    return pipelineCullBack != nullptr && pipelineCullNone != nullptr;
}

auto ShadowPass::bindInstanceBuffer(RhiDevice* device, RhiBuffer* instanceBuffer, DeletionQueue& deletionQueue, uint64_t frame) -> void {
    if (descPool != nullptr) {
        deletionQueue.defer(frame, [device, pool = descPool, set = descSet] {
            device->freeDescriptorSets(pool, {&set, 1});
            device->destroyDescriptorPool(pool);
        });
    }
    descPool = device->createDescriptorPool(1, instanceBindings);
    device->allocateDescriptorSets(descPool, descSetLayout, {&descSet, 1});
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
    device->destroyPipeline(pipelineCullBack);
    device->destroyPipeline(pipelineCullNone);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
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
    const GpuScene& scene) -> const ShadowPassData& {
    FgTextureDesc desc = {
        .width = atlasExtent.width,
        .height = atlasExtent.height,
        .format = depthFormat,
        .usage = RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled,
    };

    auto* cullBack = pipelineCullBack;
    auto* cullNone = pipelineCullNone;
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
        [cullBack, cullNone, instanceSet, atlasExtent, cascadeCopy, cascadeCount, instances, &lists, drawHandles, &scene](FrameGraphContext& ctx, const ShadowPassData& data) {
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
                // culling first, then double-sided ones without it. Commands draw whole meshes
                // on each prim's first submesh instance (docs/plan_indirect_draws.md).
                for (bool doubleSided : {false, true}) {
                    auto region = DrawLists::cascadeRegion(c, doubleSided);
                    // The GPU-written count decides how many commands draw; an empty region costs one call.
                    if (scene.indexBuffer() == nullptr) {
                        continue;
                    }
                    auto* pip = doubleSided ? cullNone : cullBack;
                    cmd->bindPipeline(pip);
                    cmd->bindDescriptorSet(pip, 0, instanceSet);
                    cmd->bindVertexBuffer(scene.positionBuffer());
                    cmd->bindIndexBuffer(scene.indexBuffer(), RhiIndexType::Uint32);
                    ShadowPush push{cascade.viewProj};
                    cmd->pushConstants(pip, RhiShaderStage::Vertex, 0, sizeof(push), &push);
                    cmd->drawIndexedIndirectCount(ctx.buffer(drawHandles.commands), lists.commandOffset(region), ctx.buffer(drawHandles.counts), DrawLists::countOffset(region), lists.regionCapacity());
                    lists.report(ctx, region, instances);
                }
            }

            cmd->endRendering();
        });
}
