#include "geometrypass.h"
#include "mesh.h"
#include "renderertypes.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "shaderloader.h"

#include <array>
#include <print>

auto GeometryPass::init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool {
    using enum RhiDescriptorType;
    using enum RhiFormat;

    vertShader = loadShaderModule(device, RhiShaderStage::Vertex, "shaders/gbuffer.vert.spv");
    fragShader = loadShaderModule(device, RhiShaderStage::Fragment, "shaders/gbuffer.frag.spv");

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

    // 16-bit float normals: a quarter of the R32 target's write bandwidth, more than
    // enough precision for a unit vector. Keep R32 where the device cannot render to it.
    auto neededUsage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled;
    if (!device->supportsTextureFormat(R16G16B16A16_SFLOAT, neededUsage)) {
        std::println(stderr, "GeometryPass: R16G16B16A16_SFLOAT not renderable, normal target stays R32G32B32A32_SFLOAT");
        normalTargetFormat = R32G32B32A32_SFLOAT;
    }
    std::array<RhiFormat, 2> colorFormats = {R8G8B8A8_UNORM, normalTargetFormat};

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayouts = {&descSetLayout, 1},
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(glm::mat4)},
        .colorFormats = colorFormats,
        .depthFormat = depthFormat,
        .vertexStride = sizeof(Vertex),
        .vertexAttributes = vertexAttrs,
    };
    // Four variants: back-face culling or none (USD doubleSided), and Less-with-write
    // when the pass owns the depth buffer or Equal-without after the depth prepass.
    for (int doubleSided = 0; doubleSided < 2; doubleSided++) {
        for (int prepassed = 0; prepassed < 2; prepassed++) {
            pipelineDesc.raster.cullMode = doubleSided != 0 ? RhiCullMode::None : RhiCullMode::Back;
            if (prepassed != 0) {
                pipelineDesc.depth = {.testEnable = true, .writeEnable = false, .compareOp = RhiCompareOp::Equal};
            } else {
                pipelineDesc.depth = {.testEnable = true, .writeEnable = true, .compareOp = RhiCompareOp::Less};
            }
            pipelines[doubleSided][prepassed] = device->createGraphicsPipeline(pipelineDesc);
            if (pipelines[doubleSided][prepassed] == nullptr) {
                return false;
            }
        }
    }
    return true;
}

auto GeometryPass::destroy(RhiDevice* device) -> void {
    device->destroyDescriptorSetLayout(descSetLayout);
    for (auto& row : pipelines) {
        for (auto* pip : row) {
            device->destroyPipeline(pip);
        }
    }
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
}

auto GeometryPass::addPass(
    FrameGraph& fg,
    FgTextureHandle depthHandle,
    RhiExtent2D extent,
    uint32_t imageIndex,
    uint32_t instanceCount,
    std::span<const GpuInstance> instances,
    std::span<const uint8_t> visible,
    const std::unordered_map<uint32_t, CachedMesh>& meshCache,
    std::span<RhiDescriptorSet*> descriptorSets,
    bool depthPrepassed) -> const GeometryPassData& {
    FgTextureDesc albedoDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = RhiFormat::R8G8B8A8_UNORM,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled,
    };
    FgTextureDesc normalDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = normalTargetFormat,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled,
    };

    auto* cullBack = pipelines[0][depthPrepassed ? 1 : 0];
    auto* cullNone = pipelines[1][depthPrepassed ? 1 : 0];

    return fg.addPass<GeometryPassData>(
        "GeometryPass",
        [&](FrameGraphBuilder& builder, GeometryPassData& data) {
            data.albedo = builder.write(builder.createTexture("gbuffer.albedo", albedoDesc), FgAccessFlags::ColorAttachment);
            data.normal = builder.write(builder.createTexture("gbuffer.normal", normalDesc), FgAccessFlags::ColorAttachment);
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.setSideEffects(true);
        },
        [cullBack, cullNone, depthPrepassed, imageIndex, instanceCount, extent, instances, visible, &meshCache, descriptorSets](FrameGraphContext& ctx, const GeometryPassData& data) {
            auto* cmd = ctx.cmd();

            std::array<RhiRenderingAttachmentInfo, 2> colorAtts = {{
                {
                    .texture = ctx.texture(data.albedo),
                    .state = RhiTextureState::ColorAttachment,
                    .clear = true,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                },
                {
                    .texture = ctx.texture(data.normal),
                    .state = RhiTextureState::ColorAttachment,
                    .clear = true,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
                },
            }};
            // After the prepass the depth buffer already holds the frame; keep it.
            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.depth),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = !depthPrepassed,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo renderInfo = {
                .extent = extent,
                .colorAttachments = {colorAtts.data(), colorAtts.size()},
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(renderInfo);
            cmd->setViewport(extent);
            cmd->setScissor(extent);

            // The visibility list is aligned with the instances; a size mismatch means the
            // snapshot predates the current world, so draw everything rather than misindex.
            bool useVisible = visible.size() == instanceCount;
            // Single-sided instances under back-face culling first, then double-sided ones
            // without it: one pipeline bind per group.
            for (bool doubleSided : {false, true}) {
                auto* pip = doubleSided ? cullNone : cullBack;
                bool bound = false;
                for (uint32_t m = 0; m < instanceCount; m++) {
                    auto& inst = instances[m];
                    if (inst.doubleSided != doubleSided) {
                        continue;
                    }
                    if (useVisible && visible[m] == 0) {
                        continue;
                    }
                    auto meshIt = meshCache.find(inst.mesh.index);
                    if (meshIt == meshCache.end()) {
                        continue;
                    }
                    auto& cached = meshIt->second;
                    if (!bound) {
                        cmd->bindPipeline(pip);
                        bound = true;
                    }

                    auto model = inst.transform;
                    cmd->pushConstants(pip, RhiShaderStage::Vertex, 0, sizeof(glm::mat4), &model);
                    cmd->bindVertexBuffer(cached.vertexBuffer);
                    cmd->bindIndexBuffer(cached.indexBuffer, RhiIndexType::Uint32);
                    cmd->bindDescriptorSet(pip, 0, descriptorSets[(imageIndex * instanceCount) + m]);
                    // Heavy draws get their own GPU zone so the pass time can be attributed.
                    bool heavy = inst.indexCount >= largeDrawIndexCount;
                    if (heavy) {
                        cmd->beginGpuZone("LargeDraw");
                    }
                    ctx.beginDraw({.instance = m, .mesh = inst.mesh.index, .material = inst.material.index, .prim = inst.prim, .indexOffset = inst.indexOffset, .indexCount = inst.indexCount});
                    cmd->drawIndexed(inst.indexCount, 1, inst.indexOffset, 0, 0);
                    ctx.endDraw();
                    if (heavy) {
                        cmd->endGpuZone();
                    }
                }
            }

            cmd->endRendering();
        });
}
