#include "lightingpass.h"
#include "geometrypass.h"
#include "renderworld.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "shaderloader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

auto LightingPass::init(RhiDevice* dev, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool {
    using enum RhiDescriptorType;

    device = dev;
    sceneColorFormat = colorFormat;

    vertShader = loadShaderModule(device, RhiShaderStage::Vertex, "shaders/lighting.vert.spv");
    fragShader = loadShaderModule(device, RhiShaderStage::Fragment, "shaders/lighting.frag.spv");

    std::array<RhiDescriptorBinding, 6> bindings = {{
        {.binding = 0, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        {.binding = 2, .type = UniformBuffer, .stage = RhiShaderStage::Fragment},
        {.binding = 3, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        {.binding = 4, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment}, // shadow atlas, plain sampler (views, manual compare)
        {.binding = 5, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment}, // shadow atlas, compare sampler (PCF)
    }};
    descriptorSetLayout = device->createDescriptorSetLayout(bindings);
    device->setDebugName(descriptorSetLayout, "lighting.setlayout");

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayouts = {&descriptorSetLayout, 1},
        .pushConstant = {.stage = RhiShaderStage::Fragment, .offset = 0, .size = 3 * sizeof(int32_t)},
        .colorFormats = {&colorFormat, 1},
        .vertexStride = 0,
        .raster = {.cullMode = RhiCullMode::None},
        .depth = {.testEnable = false, .writeEnable = false},
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);
    device->setDebugName(pipeline, "lighting.pipeline");
    if (pipeline == nullptr) {
        return false;
    }

    descriptorPool = device->createDescriptorPool(imageCount, bindings);
    device->setDebugName(descriptorPool, "lighting.sets.pool");
    descriptorSets.assign(imageCount, nullptr);
    device->allocateDescriptorSets(descriptorPool, descriptorSetLayout, descriptorSets);
    for (size_t i = 0; i < descriptorSets.size(); i++) {
        device->setDebugName(descriptorSets[i], std::format("lighting.set.slot{}", i).c_str());
    }

    uniformBuffers.resize(imageCount);
    uniformBuffersMapped.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        RhiBufferDesc uboDesc = {
            .size = sizeof(LightingUBO),
            .usage = RhiBufferUsage::Uniform,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        uniformBuffers[i] = device->createBuffer(uboDesc);
        device->setDebugName(uniformBuffers[i], "lighting.ubo");
        uniformBuffersMapped[i] = device->mapBuffer(uniformBuffers[i]);
    }

    return true;
}

auto LightingPass::destroy(RhiDevice* dev) -> void {
    dev->freeDescriptorSets(descriptorPool, descriptorSets);
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

auto LightingPass::addPass(
    FrameGraph& fg,
    const GeometryPassData& geomData,
    FgTextureHandle depthHandle,
    FgTextureHandle shadowHandle,
    RhiExtent2D extent,
    uint32_t imageIndex,
    RhiSampler* sampler,
    const LightingInputs& lightInputs,
    GBufferView viewMode,
    bool showOverlay,
    bool showShadowOverlay,
    const glm::mat4& invViewProj,
    RhiSampler* shadowSampler,
    std::span<const ShadowCascade> cascades,
    bool pcf,
    RhiExtent2D atlasExtent) -> const LightingPassData& {
    // Copied for the execute lambda; the snapshot the span points into may be gone by then.
    std::array<ShadowCascade, maxShadowCascades> cascadeCopy;
    uint32_t cascadeCount = (uint32_t) std::min(cascades.size(), (size_t) maxShadowCascades);
    for (uint32_t c = 0; c < cascadeCount; c++) {
        cascadeCopy[c] = cascades[c];
    }
    FgTextureDesc sceneColorDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = sceneColorFormat,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled,
    };

    return fg.addPass<LightingPassData>(
        "LightingPass",
        [&](FrameGraphBuilder& builder, LightingPassData& data) {
            data.albedo = builder.read(geomData.albedo, FgAccessFlags::ShaderRead);
            data.normal = builder.read(geomData.normal, FgAccessFlags::ShaderRead);
            data.depth = builder.read(depthHandle, FgAccessFlags::ShaderRead);
            data.shadowMap = builder.read(shadowHandle, FgAccessFlags::ShaderRead);
            data.sceneColor = builder.write(builder.createTexture("sceneColor", sceneColorDesc), FgAccessFlags::ColorAttachment);
            builder.setSideEffects(true);
        },
        [this, imageIndex, extent, sampler, lightInputs, viewMode, showOverlay, showShadowOverlay, invViewProj, shadowSampler, cascadeCopy, cascadeCount, pcf, atlasExtent](
            FrameGraphContext& ctx, const LightingPassData& data) {
            auto* cmd = ctx.cmd();

            LightingUBO lightUbo = {
                .lightDirection = glm::vec4(lightInputs.direction, 1.0f),
                .lightColor = glm::vec4(lightInputs.radiance, 0.15f),
                .depthParams = glm::vec4(0.1f, 3000.0f, 0.0f, 0.0f),
                .shadowTint = glm::vec4(lightInputs.shadowColor, 0.0f),
                .invViewProj = invViewProj,
                .cascadeParams = glm::vec4((float) cascadeCount, pcf ? 1.0f : 0.0f, (float) atlasExtent.width, 0.0f),
            };
            for (uint32_t c = 0; c < maxShadowCascades; c++) {
                const auto& cascade = cascadeCopy[c < cascadeCount ? c : 0];
                lightUbo.cascadeViewProj[c] = cascade.viewProj;
                lightUbo.cascadeRects[c] = cascade.atlasRect;
                lightUbo.cascadeSplits[c] = c < cascadeCount ? cascade.splitFar : 1e30f;
                lightUbo.cascadeTexelDepth[c] = cascade.texelDepthNdc;
                lightUbo.cascadeTexelWorld[c] = cascade.texelWorldSize;
            }
            memcpy(uniformBuffersMapped[imageIndex], &lightUbo, sizeof(lightUbo));

            std::array<RhiDescriptorWrite, 6> writes = {{
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
                {
                    .binding = 4,
                    .type = RhiDescriptorType::CombinedImageSampler,
                    .texture = ctx.texture(data.shadowMap),
                    .sampler = sampler,
                },
                {
                    .binding = 5,
                    .type = RhiDescriptorType::CombinedImageSampler,
                    .texture = ctx.texture(data.shadowMap),
                    .sampler = shadowSampler,
                },
            }};
            device->updateDescriptorSet(descriptorSets[imageIndex], writes);

            RhiRenderingAttachmentInfo colorAtt = {
                .texture = ctx.texture(data.sceneColor),
                .state = RhiTextureState::ColorAttachment,
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
            cmd->bindDescriptorSet(pipeline, 0, descriptorSets[imageIndex]);

            struct LightingPush {
                int32_t viewMode;
                int32_t showOverlay;
                int32_t showShadowOverlay;
            };
            LightingPush push = {
                .viewMode = static_cast<int32_t>(viewMode),
                .showOverlay = showOverlay ? 1 : 0,
                .showShadowOverlay = showShadowOverlay ? 1 : 0,
            };
            cmd->pushConstants(pipeline, RhiShaderStage::Fragment, 0, sizeof(push), &push);

            cmd->draw(3, 1, 0, 0);
            cmd->endRendering();
        });
}
