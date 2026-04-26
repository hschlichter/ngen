#include "geometrypass.h"
#include "mesh.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>

auto GeometryPass::init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool {
    using enum RhiDescriptorType;
    using enum RhiFormat;

    vertShader = device->createShaderModule("shaders/gbuffer.vert.spv");
    fragShader = device->createShaderModule("shaders/gbuffer.frag.spv");

    std::array<RhiDescriptorBinding, 2> bindings = {{
        {.binding = 0, .type = UniformBuffer, .stage = RhiShaderStage::Vertex},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
    }};
    descSetLayout = device->createDescriptorSetLayout(bindings);

    std::array<RhiVertexAttribute, 4> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, position)},
        {.location = 1, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, normal)},
        {.location = 2, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, color)},
        {.location = 3, .binding = 0, .format = R32G32_SFLOAT, .offset = offsetof(struct Vertex, texCoord)},
    }};

    std::array<RhiFormat, 2> colorFormats = {R8G8B8A8_UNORM, R32G32B32A32_SFLOAT};

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayout = descSetLayout,
        .colorFormats = colorFormats,
        .depthFormat = depthFormat,
        .vertexStride = sizeof(Vertex),
        .vertexAttributes = vertexAttrs,
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(glm::mat4)},
        .viewportExtent = extent,
        .backfaceCulling = false,
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);
    return pipeline != nullptr;
}

auto GeometryPass::destroy(RhiDevice* device) -> void {
    device->destroyDescriptorSetLayout(descSetLayout);
    device->destroyPipeline(pipeline);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
}

auto GeometryPass::addPass(FrameGraph& fg,
                           FgTextureHandle depthHandle,
                           RhiExtent2D extent,
                           uint32_t imageIndex,
                           uint32_t instanceCount,
                           std::span<const GpuInstance> instances,
                           const std::unordered_map<uint32_t, CachedMesh>& meshCache,
                           std::span<RhiDescriptorSet*> descriptorSets) -> const GeometryPassData& {
    FgTextureDesc albedoDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = RhiFormat::R8G8B8A8_UNORM,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled,
    };
    FgTextureDesc normalDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = RhiFormat::R32G32B32A32_SFLOAT,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled,
    };

    auto* pip = pipeline;

    return fg.addPass<GeometryPassData>(
        "GeometryPass",
        [&](FrameGraphBuilder& builder, GeometryPassData& data) {
            data.albedo = builder.write(builder.createTexture("gbuffer.albedo", albedoDesc), FgAccessFlags::ColorAttachment);
            data.normal = builder.write(builder.createTexture("gbuffer.normal", normalDesc), FgAccessFlags::ColorAttachment);
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.setSideEffects(true);
        },
        [pip, imageIndex, instanceCount, extent, instances, &meshCache, descriptorSets](FrameGraphContext& ctx, const GeometryPassData& data) {
            auto* cmd = ctx.cmd();

            std::array<RhiRenderingAttachmentInfo, 2> colorAtts = {{
                {
                    .texture = ctx.texture(data.albedo),
                    .layout = RhiImageLayout::ColorAttachment,
                    .clear = true,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                },
                {
                    .texture = ctx.texture(data.normal),
                    .layout = RhiImageLayout::ColorAttachment,
                    .clear = true,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
                },
            }};
            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.depth),
                .layout = RhiImageLayout::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo renderInfo = {
                .extent = extent,
                .colorAttachments = {colorAtts.data(), colorAtts.size()},
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(renderInfo);
            cmd->bindPipeline(pip);
            cmd->setViewport(extent);
            cmd->setScissor(extent);

            for (uint32_t m = 0; m < instanceCount; m++) {
                auto& inst = instances[m];
                auto meshIt = meshCache.find(inst.mesh.index);
                if (meshIt == meshCache.end()) {
                    continue;
                }
                auto& cached = meshIt->second;

                auto model = inst.transform;
                cmd->pushConstants(pip, RhiShaderStage::Vertex, 0, sizeof(glm::mat4), &model);
                cmd->bindVertexBuffer(cached.vertexBuffer);
                cmd->bindIndexBuffer(cached.indexBuffer);
                cmd->bindDescriptorSet(pip, descriptorSets[(imageIndex * instanceCount) + m]);
                cmd->drawIndexed(cached.indexCount, 1, 0, 0, 0);
            }

            cmd->endRendering();
        });
}
