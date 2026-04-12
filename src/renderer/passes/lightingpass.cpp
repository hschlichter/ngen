#include "lightingpass.h"
#include "geometrypass.h"
#include "renderworld.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>
#include <cstring>

auto LightingPass::init(RhiDevice* dev, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool {
    using enum RhiDescriptorType;

    device = dev;

    vertShader = device->createShaderModule("shaders/lighting.vert.spv");
    fragShader = device->createShaderModule("shaders/lighting.frag.spv");

    std::array<RhiDescriptorBinding, 4> bindings = {{
        {.binding = 0, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        {.binding = 2, .type = UniformBuffer, .stage = RhiShaderStage::Fragment},
        {.binding = 3, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
    }};
    descriptorSetLayout = device->createDescriptorSetLayout(bindings);

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayout = descriptorSetLayout,
        .colorFormats = {&colorFormat, 1},
        .vertexStride = 0,
        .pushConstant = {.stage = RhiShaderStage::Fragment, .offset = 0, .size = 2 * sizeof(int32_t)},
        .viewportExtent = extent,
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .backfaceCulling = false,
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);
    if (pipeline == nullptr) {
        return false;
    }

    descriptorPool = device->createDescriptorPool(imageCount, bindings);
    descriptorSets = device->allocateDescriptorSets(descriptorPool, descriptorSetLayout, imageCount);

    uniformBuffers.resize(imageCount);
    uniformBuffersMapped.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        RhiBufferDesc uboDesc = {
            .size = sizeof(LightingUBO),
            .usage = RhiBufferUsage::Uniform,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        uniformBuffers[i] = device->createBuffer(uboDesc);
        uniformBuffersMapped[i] = device->mapBuffer(uniformBuffers[i]);
    }

    return true;
}

auto LightingPass::destroy(RhiDevice* dev) -> void {
    for (auto* ds : descriptorSets) {
        delete ds;
    }
    dev->destroyDescriptorPool(descriptorPool);
    dev->destroyDescriptorSetLayout(descriptorSetLayout);

    for (size_t i = 0; i < uniformBuffers.size(); i++) {
        dev->unmapBuffer(uniformBuffers[i]);
        dev->destroyBuffer(uniformBuffers[i]);
    }

    dev->destroyPipeline(pipeline);
    dev->destroyShaderModule(vertShader);
    dev->destroyShaderModule(fragShader);
}

auto LightingPass::addPass(FrameGraph& fg,
                           const GeometryPassData& geomData,
                           FgTextureHandle depthHandle,
                           FgTextureHandle colorHandle,
                           RhiExtent2D extent,
                           uint32_t imageIndex,
                           RhiSampler* sampler,
                           const std::vector<RenderLight>& lights,
                           GBufferView viewMode,
                           bool showOverlay) -> void {
    struct LightingPassData {
        FgTextureHandle albedo;
        FgTextureHandle normal;
        FgTextureHandle depth;
        FgTextureHandle color;
    };

    fg.addPass<LightingPassData>(
        "LightingPass",
        [&](FrameGraphBuilder& builder, LightingPassData& data) {
            data.albedo = builder.read(geomData.albedo, FgAccessFlags::ShaderRead);
            data.normal = builder.read(geomData.normal, FgAccessFlags::ShaderRead);
            data.depth = builder.read(depthHandle, FgAccessFlags::ShaderRead);
            data.color = builder.write(colorHandle, FgAccessFlags::ColorAttachment);
            builder.setSideEffects(true);
        },
        [this, imageIndex, extent, sampler, &lights, viewMode, showOverlay](FrameGraphContext& ctx, const LightingPassData& data) {
            auto* cmd = ctx.cmd();

            LightingUBO lightUbo = {
                .lightDirection = glm::vec4(glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)), 1.0f),
                .lightColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.15f),
                .depthParams = glm::vec4(0.1f, 3000.0f, 0.0f, 0.0f),
            };
            for (const auto& light : lights) {
                if (light.type == LightType::Directional) {
                    auto dir = glm::normalize(glm::vec3(light.worldTransform[2]));
                    lightUbo.lightDirection = glm::vec4(dir, light.intensity);
                    lightUbo.lightColor = glm::vec4(light.color, 0.15f);
                    break;
                }
            }
            memcpy(uniformBuffersMapped[imageIndex], &lightUbo, sizeof(lightUbo));

            std::array<RhiDescriptorWrite, 4> writes = {{
                {
                    .binding = 0,
                    .type = RhiDescriptorType::CombinedImageSampler,
                    .texture = ctx.texture(data.albedo),
                    .sampler = sampler,
                },
                {
                    .binding = 1,
                    .type = RhiDescriptorType::CombinedImageSampler,
                    .texture = ctx.texture(data.normal),
                    .sampler = sampler,
                },
                {
                    .binding = 2,
                    .type = RhiDescriptorType::UniformBuffer,
                    .buffer = uniformBuffers[imageIndex],
                    .bufferRange = sizeof(LightingUBO),
                },
                {
                    .binding = 3,
                    .type = RhiDescriptorType::CombinedImageSampler,
                    .texture = ctx.texture(data.depth),
                    .sampler = sampler,
                },
            }};
            device->updateDescriptorSet(descriptorSets[imageIndex], writes);

            RhiRenderingAttachmentInfo colorAtt = {
                .texture = ctx.texture(data.color),
                .layout = RhiImageLayout::ColorAttachment,
                .clear = true,
                .clearColor = {0.12f, 0.12f, 0.15f, 1.0f},
            };
            RhiRenderingInfo renderInfo = {
                .extent = extent,
                .colorAttachments = {&colorAtt, 1},
            };
            cmd->beginRendering(renderInfo);
            cmd->bindPipeline(pipeline);
            cmd->setViewport(extent);
            cmd->setScissor(extent);
            cmd->bindDescriptorSet(pipeline, descriptorSets[imageIndex]);

            struct LightingPush {
                int32_t viewMode;
                int32_t showOverlay;
            };
            LightingPush push = {
                .viewMode = static_cast<int32_t>(viewMode),
                .showOverlay = showOverlay ? 1 : 0,
            };
            cmd->pushConstants(pipeline, RhiShaderStage::Fragment, 0, sizeof(push), &push);

            cmd->draw(3, 1, 0, 0);
            cmd->endRendering();
        });
}
