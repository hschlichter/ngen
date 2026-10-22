#include "geometrypass.h"
#include "mesh.h"
#include "profilegpu.h"
#include "renderertypes.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "trace.h"

#include <array>
#include <format>

auto GeometryPass::init(RhiDevice* device, PipelineRegistry& pipelineRegistry, RhiExtent2D extent, RhiFormat depthFormat) -> bool {
    using enum RhiDescriptorType;
    using enum RhiFormat;
    registry = &pipelineRegistry;

    auto bindings = descriptorBindings();
    descSetLayout = device->createDescriptorSetLayout(bindings);
    device->setDebugName(descSetLayout, "geometry.setlayout");

    // 16-bit float normals: a quarter of the R32 target's write bandwidth, more than
    // enough precision for a unit vector. Keep R32 where the device cannot render to it.
    auto neededUsage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled;
    if (!device->supportsTextureFormat(R16G16B16A16_SFLOAT, neededUsage)) {
        TRACE_WARNING("Render", "FormatFallback", "GeometryPass").text("R16G16B16A16_SFLOAT not renderable, normal target stays R32G32B32A32_SFLOAT");
        normalTargetFormat = R32G32B32A32_SFLOAT;
    }
    GraphicsPipelineRequest request = {
        .vertex = {.id = "shaders/gbuffer.vert", .stage = RhiShaderStage::Vertex},
        .fragment = {.id = "shaders/gbuffer.frag", .stage = RhiShaderStage::Fragment},
        .descriptorSetLayouts = {descSetLayout},
        .pushConstant = {}, // model matrix comes from the instance buffer at gl_InstanceIndex
        .colorFormats = {R8G8B8A8_UNORM, normalTargetFormat, R32_UINT},
        .depthFormat = depthFormat,
        .vertexStride = sizeof(Vertex),
        .vertexAttributes = {
            {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, position)},
            {.location = 1, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, normal)},
            {.location = 2, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, color)},
            {.location = 3, .binding = 0, .format = R32G32_SFLOAT, .offset = offsetof(struct Vertex, texCoord)},
        },
    };
    // Eight variants: opaque or cutout (gbuffercutout.frag discards where the material's
    // opacity is below its threshold), back-face culling or none (USD doubleSided), and
    // Greater-with-write (reversed-Z) when the pass owns the depth buffer or Equal-without after the depth prepass.
    for (int cutout = 0; cutout < 2; cutout++) {
        request.fragment.id = cutout != 0 ? "shaders/gbuffercutout.frag" : "shaders/gbuffer.frag";
        for (int doubleSided = 0; doubleSided < 2; doubleSided++) {
            for (int prepassed = 0; prepassed < 2; prepassed++) {
                request.raster.cullMode = doubleSided != 0 ? RhiCullMode::None : RhiCullMode::Back;
                if (prepassed != 0) {
                    request.depth = {.testEnable = true, .writeEnable = false, .compareOp = RhiCompareOp::Equal};
                } else {
                    request.depth = {.testEnable = true, .writeEnable = true, .compareOp = RhiCompareOp::Greater};
                }
                auto name = std::format("geometry.pipeline.{}.{}{}", doubleSided != 0 ? "cullnone" : "cullback", prepassed != 0 ? "equal" : "less", cutout != 0 ? ".cutout" : "");
                pipelines[cutout][doubleSided][prepassed] = pipelineRegistry.registerGraphics(name, request, PipelineUse::AtStart);
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
        // The cutout fragment shaders read the instance's material and its opacity.
        {.binding = 2, .type = StorageBuffer, .stage = RhiShaderStage::Vertex | RhiShaderStage::Fragment},
        {.binding = 3, .type = StorageBuffer, .stage = RhiShaderStage::Vertex | RhiShaderStage::Fragment},
    }};
}

auto GeometryPass::destroy(RhiDevice* device) -> void {
    device->destroyDescriptorSetLayout(descSetLayout);
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
    FgTextureDesc instanceDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = RhiFormat::R32_UINT,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled,
    };

    // By bucket (DrawLists::bucket): single-sided, double-sided, then the same two cutout.
    std::array<RhiPipeline*, DrawLists::bucketCount> bucketPipelines = {};
    for (int cutout = 0; cutout < 2; cutout++) {
        for (int doubleSided = 0; doubleSided < 2; doubleSided++) {
            bucketPipelines[DrawLists::bucket(doubleSided != 0, cutout != 0)] = registry->get(pipelines[cutout][doubleSided][depthPrepassed ? 1 : 0]);
        }
    }

    return fg.addPass<GeometryPassData>(
        "GeometryPass",
        [&](FrameGraphBuilder& builder, GeometryPassData& data) {
            data.albedo = builder.write(builder.createTexture("gbuffer.albedo", albedoDesc), FgAccessFlags::ColorAttachment);
            data.normal = builder.write(builder.createTexture("gbuffer.normal", normalDesc), FgAccessFlags::ColorAttachment);
            data.instance = builder.write(builder.createTexture("gbuffer.instance", instanceDesc), FgAccessFlags::ColorAttachment);
            data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            builder.read(drawHandles.commands, FgAccessFlags::IndirectRead);
            builder.read(drawHandles.counts, FgAccessFlags::IndirectRead);
            builder.setSideEffects(true);
        },
        [bucketPipelines, depthPrepassed, extent, instances, &lists, drawHandles, &scene, descriptorSet](FrameGraphContext& ctx, const GeometryPassData& data) {
            auto* cmd = ctx.cmd();

            std::array<RhiRenderingAttachmentInfo, 3> colorAtts = {{
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
                {
                    // Cleared to 0 (all bits zero, as an integer too): nothing drawn.
                    .texture = ctx.texture(data.instance),
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
                .clearDepth = 0.0f, // reversed-Z: 0 is infinitely far
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
            // culling first, then double-sided ones without it, then the same two cutout.
            auto* commands = ctx.buffer(drawHandles.commands);
            auto* counts = ctx.buffer(drawHandles.counts);
            for (uint32_t bucket = 0; bucket < DrawLists::bucketCount; bucket++) {
                auto region = DrawLists::cameraRegion(bucket % 2 != 0, bucket >= 2);
                // The GPU-written count decides how many commands draw; an empty region costs one call.
                if (scene.indexBuffer() == nullptr) {
                    continue;
                }
                auto* pip = bucketPipelines[bucket];
                cmd->bindPipeline(pip);
                // One set for every draw: materials are indexed through the instance record.
                cmd->bindDescriptorSet(pip, 0, descriptorSet);
                // One pool for every mesh: commands address it by firstIndex and vertexOffset.
                cmd->bindVertexBuffer(scene.vertexBuffer());
                cmd->bindIndexBuffer(scene.indexBuffer(), RhiIndexType::Uint32);
                // Each command's firstInstance is its instance index: instances[gl_InstanceIndex].
                PROFILE_GPU_ZONE(cmd, DrawLists::regionName(region));
                cmd->drawIndexedIndirectCount(commands, lists.commandOffset(region), counts, DrawLists::countOffset(region), lists.regionCapacity(region));
                lists.report(ctx, region, instances);
            }

            cmd->endRendering();
        });
}

auto GeometryPass::ready() -> bool {
    std::span<const PipelineHandle> handles(&pipelines[0][0][0], 8);
    return registry->ready(handles);
}
