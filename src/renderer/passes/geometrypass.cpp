#include "geometrypass.h"
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
    // Task and mesh shaders pull the vertices (meshlet.task, gbuffer.mesh): no vertex input.
    GraphicsPipelineRequest request = {
        .fragment = {.id = "shaders/gbuffer.frag", .stage = RhiShaderStage::Fragment},
        .task = ShaderRef{.id = "shaders/meshlet.task", .stage = RhiShaderStage::Task},
        .mesh = ShaderRef{.id = "shaders/gbuffer.mesh", .stage = RhiShaderStage::Mesh},
        .descriptorSetLayouts = {descSetLayout},
        .pushConstant = {.stage = RhiShaderStage::Task | RhiShaderStage::Mesh, .offset = 0, .size = sizeof(MeshletPush)},
        .colorFormats = {R8G8B8A8_UNORM, normalTargetFormat, R32_UINT},
        .depthFormat = depthFormat,
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

auto GeometryPass::descriptorBindings() -> std::array<RhiDescriptorBinding, 16> {
    using enum RhiDescriptorType;
    auto shaders = RhiShaderStage::Task | RhiShaderStage::Mesh | RhiShaderStage::Fragment;
    return {{
        {.binding = 0, .type = UniformBuffer, .stage = shaders},
        {.binding = 1, .type = CombinedImageSampler, .stage = RhiShaderStage::Fragment, .count = GpuScene::maxTextures},
        // The cutout fragment shaders read the instance's material and its opacity.
        {.binding = 2, .type = StorageBuffer, .stage = shaders},
        {.binding = 3, .type = StorageBuffer, .stage = shaders},
        // The geometry pool (meshlet.glsl): vertices, positions, meshlets, their vertex indices and triangles, the mesh table.
        {.binding = 4, .type = StorageBuffer, .stage = shaders},
        {.binding = 5, .type = StorageBuffer, .stage = shaders},
        {.binding = 6, .type = StorageBuffer, .stage = shaders},
        {.binding = 7, .type = StorageBuffer, .stage = shaders},
        {.binding = 8, .type = StorageBuffer, .stage = shaders},
        {.binding = 9, .type = StorageBuffer, .stage = shaders},
        // The frame slot's culling buffers: the meshlet work items, the culling parameters, the meshlet counts.
        {.binding = 10, .type = StorageBuffer, .stage = shaders},
        {.binding = 11, .type = StorageBuffer, .stage = shaders},
        {.binding = 12, .type = StorageBuffer, .stage = shaders},
        // The depth pyramid, for the late phase's meshlets.
        {.binding = 13, .type = CombinedImageSampler, .stage = shaders},
        // The regions' draws and work item ranges.
        {.binding = 14, .type = StorageBuffer, .stage = shaders},
        // The render-LOD state (lodselect.comp's): the cutout buckets' mesh shaders read a fading instance's share of the pattern, the geometry
        // pass's its LOD tag (lodfade.glsl).
        {.binding = 15, .type = StorageBuffer, .stage = shaders},
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
    bool depthPrepassed,
    FgTextureHandle hzb,
    bool occlusion,
    GeometryPhase phase,
    const GeometryPassData* first) -> const GeometryPassData& {
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

    // The second phase draws onto the first's targets.
    bool second = phase == GeometryPhase::Second && first != nullptr;
    return fg.addPass<GeometryPassData>(
        second ? "GeometryPassLate" : "GeometryPass",
        [&](FrameGraphBuilder& builder, GeometryPassData& data) {
            if (second) {
                data.albedo = builder.write(first->albedo, FgAccessFlags::ColorAttachment);
                data.normal = builder.write(first->normal, FgAccessFlags::ColorAttachment);
                data.instance = builder.write(first->instance, FgAccessFlags::ColorAttachment);
                data.depth = builder.write(first->depth, FgAccessFlags::DepthAttachment);
            } else {
                data.albedo = builder.write(builder.createTexture("gbuffer.albedo", albedoDesc), FgAccessFlags::ColorAttachment);
                data.normal = builder.write(builder.createTexture("gbuffer.normal", normalDesc), FgAccessFlags::ColorAttachment);
                data.instance = builder.write(builder.createTexture("gbuffer.instance", instanceDesc), FgAccessFlags::ColorAttachment);
                data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            }
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            // The task shaders read their draw's instance from the commands, and add to the meshlet counts.
            // One draw per region: its RegionWork is the draw's command and the task shaders' range of work items.
            builder.read(drawHandles.regionWork, FgAccessFlags::IndirectRead | FgAccessFlags::StorageRead);
            builder.read(drawHandles.workItems, FgAccessFlags::StorageRead);
            builder.write(drawHandles.meshletStats, FgAccessFlags::StorageWrite);
            builder.read(hzb, FgAccessFlags::ShaderRead);
            if (drawHandles.lodStates.valid()) {
                builder.read(drawHandles.lodStates, FgAccessFlags::StorageRead);
            }
            builder.setSideEffects(true);
        },
        [bucketPipelines, depthPrepassed, extent, instances, &lists, drawHandles, &scene, descriptorSet, occlusion, phase, second](FrameGraphContext& ctx, const GeometryPassData& data) {
            auto* cmd = ctx.cmd();

            std::array<RhiRenderingAttachmentInfo, 3> colorAtts = {{
                {
                    .texture = ctx.texture(data.albedo),
                    .state = RhiTextureState::ColorAttachment,
                    .clear = !second,
                    .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                },
                {
                    .texture = ctx.texture(data.normal),
                    .state = RhiTextureState::ColorAttachment,
                    .clear = !second,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
                },
                {
                    // Cleared to 0 (all bits zero, as an integer too): nothing drawn.
                    .texture = ctx.texture(data.instance),
                    .state = RhiTextureState::ColorAttachment,
                    .clear = !second,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
                },
            }};
            // After the prepass the depth buffer already holds the frame; keep it.
            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.depth),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = !depthPrepassed && !second,
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
            // culling first, then double-sided ones without it, then the same two cutout; with occlusion, then the late phase's. With
            // occlusion every meshlet of both phases is tested against the pyramid of the prepass's whole depth (HzbBuildFinal): one it
            // hides lies behind the depth at every pixel it covers, so under the Equal test it would shade nothing.
            auto* regionWork = ctx.buffer(drawHandles.regionWork);
            // Both: the first phase's regions, then with occlusion the second's. First and Second: one of them.
            uint32_t firstPhase = phase == GeometryPhase::Second ? 1u : 0u;
            uint32_t endPhase = phase == GeometryPhase::First ? 1u : (occlusion ? 2u : 1u);
            // The pyramid test: every meshlet after a two-phase prepass (its whole depth), the second phase's without one.
            bool testPyramid = occlusion && phase != GeometryPhase::First;
            for (uint32_t drawPhase = firstPhase; drawPhase < endPhase; drawPhase++) {
                bool late = drawPhase == 1;
                for (uint32_t bucket = 0; bucket < DrawLists::bucketCount; bucket++) {
                    auto region = late ? DrawLists::cameraLateRegion(bucket % 2 != 0, bucket >= 2) : DrawLists::cameraRegion(bucket % 2 != 0, bucket >= 2);
                    // The GPU-written count decides how many commands draw; an empty region costs one call.
                    if (!scene.poolReady() || descriptorSet == nullptr) {
                        continue;
                    }
                    auto* pip = bucketPipelines[bucket];
                    cmd->bindPipeline(pip);
                    // One set for every draw: materials are indexed through the instance record, vertices pulled from the pool.
                    cmd->bindDescriptorSet(pip, 0, descriptorSet);
                    // The camera's meshlet counts come from this pass.
                    MeshletPush push = {.region = region, .flags = meshletFlagCount | (testPyramid ? meshletFlagOcclusion : 0u)};
                    cmd->pushConstants(pip, RhiShaderStage::Task | RhiShaderStage::Mesh, 0, sizeof(push), &push);
                    PROFILE_GPU_ZONE(cmd, DrawLists::regionName(region));
                    DrawLists::drawRegion(cmd, regionWork, region);
                    lists.report(ctx, region, instances);
                }
            }

            cmd->endRendering();
        });
}

auto GeometryPass::ready() -> bool {
    std::span<const PipelineHandle> handles(&pipelines[0][0][0], 8);
    return registry->ready(handles);
}
