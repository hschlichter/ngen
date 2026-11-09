#include "lightingpass.h"

#include "geometrypass.h"
#include "projection.h"
#include "renderconfig.h"
#include "renderworld.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

auto LightingPass::init(RhiDevice* dev, PipelineRegistry& pipelines, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool {
    using enum RhiDescriptorType;

    device = dev;
    registry = &pipelines;
    sceneColorFormat = colorFormat;

    std::array<RhiDescriptorBinding, 8> bindings = {{
        {.binding = 0, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        {.binding = 2, .type = UniformBuffer, .stage = RhiShaderStage::Fragment},
        {.binding = 3, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment},
        {.binding = 4, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment}, // shadow atlas, plain sampler (views, manual compare)
        {.binding = 5, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment}, // shadow atlas, compare sampler (PCF)
        {.binding = 6, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment}, // the sky's lat-long image
        {.binding = 7, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment}, // the depth pyramid (Hzb view), else the depth
    }};
    skySampler = device->createSampler({
        .mipmapMode = RhiMipmapMode::Nearest,
        .addressU = RhiAddressMode::Repeat,
        .addressV = RhiAddressMode::ClampToEdge,
        .addressW = RhiAddressMode::ClampToEdge,
        .debugName = "sampler.sky",
    });
    descriptorSetLayout = device->createDescriptorSetLayout(bindings);
    device->setDebugName(descriptorSetLayout, "lighting.setlayout");

    GraphicsPipelineRequest request = {
        .vertex = {.id = "shaders/lighting.vert", .stage = RhiShaderStage::Vertex},
        .fragment = {.id = "shaders/lighting.frag", .stage = RhiShaderStage::Fragment},
        .descriptorSetLayouts = {descriptorSetLayout},
        .pushConstant = {.stage = RhiShaderStage::Fragment, .offset = 0, .size = 6 * sizeof(int32_t)},
        .colorFormats = {colorFormat},
        .vertexStride = 0,
        .raster = {.cullMode = RhiCullMode::None},
        .depth = {.testEnable = false, .writeEnable = false},
    };
    pipelineHandle = pipelines.registerGraphics("lighting.pipeline", request, PipelineUse::AtStart);

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
    dev->destroySampler(skySampler);
    dev->freeDescriptorSets(descriptorPool, descriptorSets);
    dev->destroyDescriptorPool(descriptorPool);
    dev->destroyDescriptorSetLayout(descriptorSetLayout);

    for (size_t i = 0; i < uniformBuffers.size(); i++) {
        dev->unmapBuffer(uniformBuffers[i]);
        dev->destroyBuffer(uniformBuffers[i]);
    }
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
    FgTextureHandle hzbHandle,
    int hzbLevel,
    bool showOverlay,
    bool showShadowOverlay,
    bool lodFlash,
    bool lodColorByDepth,
    const glm::mat4& invViewProj,
    RhiSampler* shadowSampler,
    std::span<const ShadowCascade> cascades,
    bool pcf,
    RhiExtent2D atlasExtent,
    float previewFar,
    RhiTexture* noSky) -> const LightingPassData& {
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
        profile::Group::Lighting,
        [&](FrameGraphBuilder& builder, LightingPassData& data) {
            data.albedo = builder.read(geomData.albedo, FgAccessFlags::ShaderRead);
            data.normal = builder.read(geomData.normal, FgAccessFlags::ShaderRead);
            data.depth = builder.read(depthHandle, FgAccessFlags::ShaderRead);
            data.shadowMap = builder.read(shadowHandle, FgAccessFlags::ShaderRead);
            // The pyramid is read only while its view is on, so every other frame's graph is unchanged; binding 7 then holds the depth.
            if (viewMode == GBufferView::Hzb && hzbHandle.valid()) {
                data.hzb = builder.read(hzbHandle, FgAccessFlags::ShaderRead);
            }
            data.sceneColor = builder.write(builder.createTexture("sceneColor", sceneColorDesc), FgAccessFlags::ColorAttachment);
            builder.setSideEffects(true);
        },
        [this, pipeline = registry->get(pipelineHandle), imageIndex, extent, sampler, lightInputs, viewMode, hzbLevel, showOverlay, showShadowOverlay, lodFlash, lodColorByDepth, invViewProj, shadowSampler, cascadeCopy, cascadeCount, pcf, atlasExtent, previewFar, noSky](
            FrameGraphContext& ctx, const LightingPassData& data) {
            auto* cmd = ctx.cmd();

            LightingUBO lightUbo = {
                .lightDirection = glm::vec4(lightInputs.direction, 1.0f),
                .lightColor = glm::vec4(lightInputs.radiance, 0.0f),
                .depthParams = glm::vec4(projectionNear, previewFar, 0.0f, 0.0f),
                .shadowTint = glm::vec4(lightInputs.shadowColor, 0.0f),
                .invViewProj = invViewProj,
                .cascadeParams = glm::vec4((float) cascadeCount, pcf ? 1.0f : 0.0f, (float) atlasExtent.width, 0.0f),
                .ambientUp = glm::vec4(lightInputs.up, 0.0f),
                .skyAmbient = glm::vec4(lightInputs.ambient.sky, 0.0f),
                .groundAmbient = glm::vec4(lightInputs.ambient.ground, 0.0f),
            };
            if (lightInputs.cascadeCamera) {
                lightUbo.cascadeEye = glm::vec4(lightInputs.cascadeCamera->first, 1.0f);
                lightUbo.cascadeForward = glm::vec4(lightInputs.cascadeCamera->second, 0.0f);
            }
            if (lightInputs.sky) {
                lightUbo.skyRadiance = glm::vec4(lightInputs.sky->radiance, 1.0f);
                lightUbo.skyToImage = glm::mat4(lightInputs.sky->toImage);
                for (size_t i = 0; i < lightInputs.sky->irradiance.size(); i++) {
                    lightUbo.skyIrradiance[i] = glm::vec4(lightInputs.sky->irradiance[i], 0.0f);
                }
            }
            for (uint32_t c = 0; c < maxShadowCascades; c++) {
                const auto& cascade = cascadeCopy[c < cascadeCount ? c : 0];
                lightUbo.cascadeViewProj[c] = cascade.viewProj;
                lightUbo.cascadeRects[c] = cascade.atlasRect;
                lightUbo.cascadeSplits[c] = c < cascadeCount ? cascade.splitFar : 1e30f;
                lightUbo.cascadeTexelDepth[c] = cascade.texelDepthNdc;
                lightUbo.cascadeTexelWorld[c] = cascade.texelWorldSize;
            }
            memcpy(uniformBuffersMapped[imageIndex], &lightUbo, sizeof(lightUbo));

            std::array<RhiDescriptorWrite, 8> writes = {{
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
                {
                    .binding = 6,
                    .type = RhiDescriptorType::CombinedImageSampler,
                    .texture = lightInputs.sky ? lightInputs.sky->texture : noSky,
                    .sampler = skySampler,
                },
                {
                    .binding = 7,
                    .type = RhiDescriptorType::CombinedImageSampler,
                    .texture = data.hzb.valid() ? ctx.texture(data.hzb) : ctx.texture(data.depth),
                    .sampler = sampler,
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
                int32_t lodFlash;
                int32_t lodColorByDepth;
                int32_t hzbLevel; // the Hzb view's level; -1 while the pyramid wasn't built this frame
            };
            LightingPush push = {
                .viewMode = static_cast<int32_t>(viewMode),
                .showOverlay = showOverlay ? 1 : 0,
                .showShadowOverlay = showShadowOverlay ? 1 : 0,
                .lodFlash = lodFlash ? 1 : 0,
                .lodColorByDepth = lodColorByDepth ? 1 : 0,
                .hzbLevel = data.hzb.valid() ? hzbLevel : -1,
            };
            cmd->pushConstants(pipeline, RhiShaderStage::Fragment, 0, sizeof(push), &push);

            cmd->draw(3, 1, 0, 0);
            cmd->endRendering();
        });
}

auto LightingPass::ready() -> bool {
    std::array<PipelineHandle, 1> handles = {pipelineHandle};
    return registry->ready(handles);
}

namespace renderconfig {

namespace {

auto toVec3(const std::array<float, 3>& value) -> glm::vec3 {
    return {value[0], value[1], value[2]};
}

auto toArray(const glm::vec3& value) -> std::array<float, 3> {
    return {value.x, value.y, value.z};
}

} // namespace

config::Enum bufferView{"render.view.buffer",
                        std::vector<std::string>(gbufferViewNames.begin(), gbufferViewNames.end()),
                        0,
                        "What the lighting pass shows: the lit image, a G-buffer target, or a view of one of its inputs (shadows, mips, cascades, LOD levels)."};
config::Int hzbLevel{"render.view.hzblevel", 0, "The level of the depth pyramid the hzb buffer view shows: 0 the finest; past the last, the last.", {.min = 0, .max = 15}};
config::Bool bufferOverlay{"render.overlay.buffer", false, "A strip of the G-buffer targets (albedo, normals, depth, lit) along the bottom of the view."};
config::Bool shadowOverlay{"render.overlay.shadow", false, "A strip of the shadow cascades' maps along the bottom of the view."};
config::Vec3 ambientSky{"render.ambient.sky", toArray(AmbientSettings{}.sky), "The hemisphere ambient on faces turned up, as a fraction of the sun's radiance."};
config::Vec3 ambientGround{"render.ambient.ground", toArray(AmbientSettings{}.ground), "The hemisphere ambient on faces turned down, as a fraction of the sun's radiance."};
config::Bool skyOn{"render.sky.on", true, "The scene's dome light drawn as the sky and lighting the scene in place of the configured ambient; off, the clear colour and render.ambient.*."};

auto ambient() -> AmbientSettings {
    return {.sky = toVec3(ambientSky.get()), .ground = toVec3(ambientGround.get())};
}

auto setAmbient(const AmbientSettings& settings) -> void {
    ambientSky.set(toArray(settings.sky));
    ambientGround.set(toArray(settings.ground));
}

} // namespace renderconfig
