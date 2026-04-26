#include "shadowpass.h"

#include "mesh.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>

namespace {
struct ShadowPush {
    glm::mat4 lightViewProj;
    glm::mat4 model;
};
} // namespace

auto ShadowPass::init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool {
    using enum RhiFormat;

    vertShader = device->createShaderModule("shaders/shadow.vert.spv");
    fragShader = device->createShaderModule("shaders/shadow.frag.spv");

    std::array<RhiVertexAttribute, 1> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, position)},
    }};

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayout = nullptr, // no descriptors; all data via push constants
        .colorFormats = {},             // depth-only
        .depthFormat = depthFormat,
        .vertexStride = sizeof(Vertex),
        .vertexAttributes = vertexAttrs,
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(ShadowPush)},
        .viewportExtent = extent,
        .backfaceCulling = false,
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);
    return pipeline != nullptr;
}

auto ShadowPass::destroy(RhiDevice* device) -> void {
    device->destroyPipeline(pipeline);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
}

auto ShadowPass::addPass(FrameGraph& fg,
                         RhiExtent2D extent,
                         RhiFormat depthFormat,
                         const glm::mat4& lightViewProj,
                         std::span<const GpuInstance> instances,
                         const std::unordered_map<uint32_t, CachedMesh>& meshCache) -> const ShadowPassData& {
    FgTextureDesc desc = {
        .width = extent.width,
        .height = extent.height,
        .format = depthFormat,
        .usage = RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled,
    };

    auto* pip = pipeline;

    return fg.addPass<ShadowPassData>(
        "ShadowPass",
        [&](FrameGraphBuilder& builder, ShadowPassData& data) {
            data.shadowMap = builder.write(builder.createTexture("shadowMap", desc), FgAccessFlags::DepthAttachment);
        },
        [pip, extent, lightViewProj, instances, &meshCache](FrameGraphContext& ctx, const ShadowPassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.shadowMap),
                .layout = RhiImageLayout::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo info = {
                .extent = extent,
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(info);
            cmd->bindPipeline(pip);
            cmd->setViewport(extent);
            cmd->setScissor(extent);

            for (const auto& inst : instances) {
                auto meshIt = meshCache.find(inst.mesh.index);
                if (meshIt == meshCache.end()) {
                    continue;
                }
                const auto& cached = meshIt->second;

                ShadowPush push{lightViewProj, inst.transform};
                cmd->pushConstants(pip, RhiShaderStage::Vertex, 0, sizeof(push), &push);
                cmd->bindVertexBuffer(cached.vertexBuffer);
                cmd->bindIndexBuffer(cached.indexBuffer);
                cmd->drawIndexed(cached.indexCount, 1, 0, 0, 0);
            }

            cmd->endRendering();
        });
}
