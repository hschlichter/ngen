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

    auto bindings = descriptorBindings();
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
        .pushConstant = {}, // model matrix comes from the instance buffer at gl_InstanceIndex
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

auto GeometryPass::descriptorBindings() -> std::array<RhiDescriptorBinding, 4> {
    using enum RhiDescriptorType;
    return {{
        {.binding = 0, .type = UniformBuffer, .stage = RhiShaderStage::Vertex},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment, .count = GpuScene::maxTextures},
        {.binding = 2, .type = StorageBuffer, .stage = RhiShaderStage::Vertex},
        {.binding = 3, .type = StorageBuffer, .stage = RhiShaderStage::Vertex},
    }};
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
    std::span<const GpuInstance> instances,
    FgBufferHandle instanceBuffer,
    const DrawLists& lists,
    DrawLists::Handles drawHandles,
    const GpuScene& scene,
    RhiDescriptorSet* descriptorSet,
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
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            builder.read(drawHandles.commands, FgAccessFlags::IndirectRead);
            builder.read(drawHandles.counts, FgAccessFlags::IndirectRead);
            builder.setSideEffects(true);
        },
        [cullBack, cullNone, depthPrepassed, extent, instances, &lists, drawHandles, &scene, descriptorSet](FrameGraphContext& ctx, const GeometryPassData& data) {
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

            // One indirect call per pipeline bucket: single-sided instances under back-face
            // culling first, then double-sided ones without it.
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
                // One set for every draw: materials are indexed through the instance record.
                cmd->bindDescriptorSet(pip, 0, descriptorSet);
                // One pool for every mesh: commands address it by firstIndex and vertexOffset.
                cmd->bindVertexBuffer(scene.vertexBuffer());
                cmd->bindIndexBuffer(scene.indexBuffer(), RhiIndexType::Uint32);
                // Each command's firstInstance is its instance index: instances[gl_InstanceIndex].
                cmd->drawIndexedIndirectCount(commands, lists.commandOffset(region), counts, DrawLists::countOffset(region), lists.regionCapacity());
                lists.report(ctx, region, instances);
            }

            cmd->endRendering();
        });
}
