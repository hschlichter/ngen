#include "debugviewpass.h"

#include "renderconfig.h"
#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "trace.h"

#include <algorithm>
#include <array>
#include <format>

namespace {

constexpr auto valueFormat = RhiFormat::R32G32B32A32_SFLOAT;
constexpr auto colorFormat = RhiFormat::R16G16B16A16_SFLOAT;

// MeshletPush, then the viewport and the view (debugview.mesh and debugview.frag).
struct DrawPush {
    MeshletPush meshlet;
    float viewport[2];
    uint32_t mode;
};
static_assert(offsetof(DrawPush, viewport) == 8);

struct ResolvePush {
    uint32_t size[2];
    uint32_t mode;
    float overdrawMax;
    uint32_t streamingRoots; // the states written for the streaming view
};

} // namespace

auto DebugViewPass::init(RhiDevice* dev, PipelineRegistry& pipelineRegistry, uint32_t frameCount, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool {
    using enum RhiFormat;
    device = dev;
    registry = &pipelineRegistry;
    depthTargetFormat = depthFormat;
    if (!device->supportsTextureFormat(valueFormat, RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled | RhiTextureUsage::TransferSrc) ||
        !device->supportsTextureFormat(colorFormat, RhiTextureUsage::Storage | RhiTextureUsage::TransferSrc)) {
        TRACE_WARNING("Render", "DebugViewsOff", "DebugViewPass").text("value or colour format unsupported; debug views are off");
        return true;
    }

    // The task shader culls as the geometry pass's does and launches two mesh workgroups per meshlet (meshletFlagSplit): debugview.mesh
    // gives every triangle vertices of its own, for its size, edges and index.
    GraphicsPipelineRequest desc = {
        .fragment = {.id = "shaders/debugview.frag", .stage = RhiShaderStage::Fragment},
        .task = ShaderRef{.id = "shaders/meshlet.task", .stage = RhiShaderStage::Task},
        .mesh = ShaderRef{.id = "shaders/debugview.mesh", .stage = RhiShaderStage::Mesh},
        .descriptorSetLayouts = {geometrySetLayout},
        .pushConstant = {.stage = RhiShaderStage::Task | RhiShaderStage::Mesh | RhiShaderStage::Fragment, .offset = 0, .size = sizeof(DrawPush)},
        .colorFormats = {valueFormat},
        .depthFormat = depthFormat,
    };
    for (int doubleSided = 0; doubleSided < 2; doubleSided++) {
        const char* cull = doubleSided != 0 ? "cullnone" : "cullback";
        desc.raster.cullMode = doubleSided != 0 ? RhiCullMode::None : RhiCullMode::Back;
        // Same position expression as the geometry pass: GreaterOrEqual (reversed-Z) keeps its visible surfaces.
        desc.depthFormat = depthFormat;
        desc.depth = {.testEnable = true, .writeEnable = false, .compareOp = RhiCompareOp::GreaterOrEqual};
        desc.blend = {};
        pipelines[doubleSided] = registry->registerGraphics(std::format("debugview.pipeline.{}", cull), desc, PipelineUse::FirstUse);
        // Overdraw: every fragment counts, so no depth; blending adds 1 per fragment.
        desc.depthFormat = RhiFormat::Undefined;
        desc.depth = {.testEnable = false, .writeEnable = false};
        desc.blend = {
            .enable = true,
            .srcColor = RhiBlendFactor::One,
            .dstColor = RhiBlendFactor::One,
            .colorOp = RhiBlendOp::Add,
            .srcAlpha = RhiBlendFactor::One,
            .dstAlpha = RhiBlendFactor::One,
            .alphaOp = RhiBlendOp::Add,
        };
        overdrawPipelines[doubleSided] = registry->registerGraphics(std::format("debugview.overdraw.{}", cull), desc, PipelineUse::FirstUse);
        // The meshlet culling view: every meshlet into a depth buffer of its own (the culled ones aren't in the frame's), then the x-ray,
        // testing against it without writing and keeping what lies behind (Less, reversed-Z).
        desc.depthFormat = depthFormat;
        desc.blend = {};
        desc.depth = {.testEnable = true, .writeEnable = true, .compareOp = RhiCompareOp::Greater};
        classifyPipelines[doubleSided] = registry->registerGraphics(std::format("debugview.meshletcull.{}", cull), desc, PipelineUse::FirstUse);
        desc.depth = {.testEnable = true, .writeEnable = false, .compareOp = RhiCompareOp::Less};
        xrayPipelines[doubleSided] = registry->registerGraphics(std::format("debugview.meshletcull.xray.{}", cull), desc, PipelineUse::FirstUse);
    }

    std::array<RhiDescriptorBinding, 4> bindings = {{
        {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Compute},
        {.binding = 1, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute},
        {.binding = 2, .type = RhiDescriptorType::StorageBuffer, .stage = RhiShaderStage::Compute},
        {.binding = 3, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Compute},
    }};
    resolveLayout = device->createDescriptorSetLayout(bindings);
    device->setDebugName(resolveLayout, "debugview.resolve.setlayout");
    ComputePipelineRequest resolveDesc = {
        .shader = {.id = "shaders/debugview.comp", .stage = RhiShaderStage::Compute},
        .descriptorSetLayouts = {resolveLayout},
        .pushConstant = {.stage = RhiShaderStage::Compute, .offset = 0, .size = sizeof(ResolvePush)},
    };
    resolvePipeline = registry->registerCompute("debugview.resolve.pipeline", resolveDesc, PipelineUse::FirstUse);
    resolvePool = device->createDescriptorPool(frameCount, bindings);
    device->setDebugName(resolvePool, "debugview.resolve.pool");
    resolveSets.assign(frameCount, nullptr);
    if (!device->allocateDescriptorSets(resolvePool, resolveLayout, resolveSets)) {
        return false;
    }
    for (size_t i = 0; i < resolveSets.size(); i++) {
        device->setDebugName(resolveSets[i], std::format("debugview.resolve.set.slot{}", i).c_str());
    }
    for (uint32_t i = 0; i < frameCount; i++) {
        auto* buffer = device->createBuffer({.size = (uint64_t) maxStreamingRoots * sizeof(uint32_t), .usage = RhiBufferUsage::Storage, .memory = RhiMemoryUsage::CpuToGpu});
        device->setDebugName(buffer, std::format("debugview.streaming.slot{}", i).c_str());
        streamingBuffers.push_back(buffer);
        streamingMapped.push_back(device->mapBuffer(buffer));
    }
    // The resolve reads with texelFetch; a point sampler keeps float formats without linear
    // filtering support valid.
    pointSampler = device->createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Nearest, .mipmapMode = RhiMipmapMode::Nearest, .debugName = "debugview.sampler"});
    supported = true;
    return true;
}

auto DebugViewPass::destroy(RhiDevice* dev) -> void {
    if (!supported) {
        return;
    }
    dev->destroySampler(pointSampler);
    for (auto* buffer : streamingBuffers) {
        dev->destroyBuffer(buffer);
    }
    dev->freeDescriptorSets(resolvePool, resolveSets);
    dev->destroyDescriptorPool(resolvePool);
    dev->destroyDescriptorSetLayout(resolveLayout);
}

auto DebugViewPass::ready() -> bool {
    if (!supported) {
        return false;
    }
    std::array<PipelineHandle, 9> handles = {
        pipelines[0],
        pipelines[1],
        overdrawPipelines[0],
        overdrawPipelines[1],
        classifyPipelines[0],
        classifyPipelines[1],
        xrayPipelines[0],
        xrayPipelines[1],
        resolvePipeline,
    };
    return registry->ready(handles);
}

auto DebugViewPass::addPass(
    FrameGraph& fg,
    DebugView view,
    FgTextureHandle depthHandle,
    RhiExtent2D extent,
    uint32_t frameSlot,
    FgBufferHandle instanceBuffer,
    const DrawLists& lists,
    DrawLists::Handles drawHandles,
    const GpuScene& scene,
    RhiDescriptorSet* geometrySet,
    FgTextureHandle hzb,
    bool occlusion,
    std::span<const uint32_t> streamingStates,
    FgTextureHandle litHandle) -> const DebugViewPassData& {
    FgTextureDesc valueDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = valueFormat,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled | RhiTextureUsage::TransferSrc,
    };
    FgTextureDesc colorDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = colorFormat,
        .usage = RhiTextureUsage::Storage | RhiTextureUsage::TransferSrc,
    };
    FgTextureDesc depthDesc = {
        .width = extent.width,
        .height = extent.height,
        .format = depthTargetFormat,
        .usage = RhiTextureUsage::DepthAttachment,
    };
    bool overdraw = view == DebugView::Overdraw;
    // The streaming view lays its colours over the lit image; the other views replace it, so the lit image isn't kept alive for them.
    bool overLit = view == DebugView::Streaming;
    bool meshletCull = view == DebugView::MeshletCull;
    auto mode = (uint32_t) view;

    struct DrawData {
        FgTextureHandle value;
        FgTextureHandle depth;
    };
    const auto& drawData = fg.addPass<DrawData>(
        "DebugViewPass",
        [&](FrameGraphBuilder& builder, DrawData& data) {
            data.value = builder.write(builder.createTexture("debugview.value", valueDesc), FgAccessFlags::ColorAttachment);
            if (meshletCull) {
                data.depth = builder.write(builder.createTexture("debugview.depth", depthDesc), FgAccessFlags::DepthAttachment);
            } else if (!overdraw) {
                data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            }
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            // One draw per region: its RegionWork is the draw's command and the task shaders' range of work items.
            builder.read(drawHandles.regionWork, FgAccessFlags::IndirectRead | FgAccessFlags::StorageRead);
            builder.read(drawHandles.workItems, FgAccessFlags::StorageRead);
            builder.read(hzb, FgAccessFlags::ShaderRead);
            if (drawHandles.lodStates.valid()) {
                builder.read(drawHandles.lodStates, FgAccessFlags::StorageRead);
            }
        },
        [this, overdraw, meshletCull, mode, extent, drawHandles, &scene, geometrySet, occlusion](FrameGraphContext& ctx, const DrawData& data) {
            auto* cmd = ctx.cmd();
            std::array<RhiRenderingAttachmentInfo, 1> colorAtts = {{
                {
                    .texture = ctx.texture(data.value),
                    .state = RhiTextureState::ColorAttachment,
                    .clear = true,
                    .clearColor = {0.0f, 0.0f, 0.0f, 0.0f},
                },
            }};
            RhiRenderingAttachmentInfo depthAtt = {};
            RhiRenderingInfo renderInfo = {
                .extent = extent,
                .colorAttachments = {colorAtts.data(), colorAtts.size()},
            };
            if (!overdraw) {
                // The geometry pass's depth, kept: the test selects its visible surfaces. The meshlet culling view's own, cleared.
                depthAtt = {
                    .texture = ctx.texture(data.depth),
                    .state = RhiTextureState::DepthStencilAttachment,
                    .clear = meshletCull,
                    .clearDepth = 0.0f, // reversed-Z: 0 is infinitely far
                };
                renderInfo.depthAttachment = &depthAtt;
            }
            cmd->beginRendering(renderInfo);
            cmd->setViewport(extent);
            cmd->setScissor(extent);
            DrawPush push = {
                .meshlet = {.flags = meshletFlagSplit},
                .viewport = {(float) extent.width, (float) extent.height},
                .mode = mode,
            };
            auto* regionWork = ctx.buffer(drawHandles.regionWork);
            // Every camera bucket; debugview.frag discards cut-away texels itself, so the cutout
            // buckets draw with the same pipelines.
            // Both of occlusion's phases, their meshlets tested as the geometry pass tests them. The meshlet culling view classifies
            // instead of culling, then draws its x-ray: a second sweep keeping only the meshlets the depth pyramid hides.
            uint32_t sweeps = meshletCull && occlusion ? 2u : 1u;
            uint32_t phases = occlusion ? 2u : 1u;
            for (uint32_t sweep = 0; sweep < sweeps; sweep++) {
                bool xray = sweep == 1;
                for (uint32_t index = 0; index < DrawLists::bucketCount * phases; index++) {
                    if (!scene.poolReady() || geometrySet == nullptr) {
                        continue;
                    }
                    uint32_t bucket = index % DrawLists::bucketCount;
                    bool late = index >= DrawLists::bucketCount;
                    bool doubleSided = bucket % 2 != 0;
                    auto region = late ? DrawLists::cameraLateRegion(doubleSided, bucket >= 2) : DrawLists::cameraRegion(doubleSided, bucket >= 2);
                    push.meshlet.flags = meshletFlagSplit | (occlusion ? meshletFlagOcclusion : 0u);
                    PipelineHandle handle = pipelines[doubleSided ? 1 : 0];
                    if (overdraw) {
                        handle = overdrawPipelines[doubleSided ? 1 : 0];
                    } else if (meshletCull) {
                        push.meshlet.flags |= meshletFlagClassify | (xray ? meshletFlagHiddenOnly : 0u);
                        handle = xray ? xrayPipelines[doubleSided ? 1 : 0] : classifyPipelines[doubleSided ? 1 : 0];
                    }
                    auto* pip = registry->get(handle);
                    cmd->bindPipeline(pip);
                    cmd->bindDescriptorSet(pip, 0, geometrySet);
                    push.meshlet.region = region;
                    cmd->pushConstants(pip, RhiShaderStage::Task | RhiShaderStage::Mesh | RhiShaderStage::Fragment, 0, sizeof(push), &push);
                    PROFILE_GPU_ZONE(cmd, DrawLists::regionName(region));
                    DrawLists::drawRegion(cmd, regionWork, region);
                }
            }
            cmd->endRendering();
        });

    auto valueHandle = drawData.value;
    return fg.addPass<DebugViewPassData>(
        "DebugViewResolve",
        [&](FrameGraphBuilder& builder, DebugViewPassData& data) {
            data.value = builder.read(valueHandle, FgAccessFlags::ShaderRead);
            data.color = builder.write(builder.createTexture("debugview.color", colorDesc), FgAccessFlags::StorageWrite);
            if (overLit) {
                data.lit = builder.read(litHandle, FgAccessFlags::ShaderRead);
            }
        },
        [this, mode, extent, frameSlot, streamingStates, overLit](FrameGraphContext& ctx, const DebugViewPassData& data) {
            // The streaming view's root states, for this frame slot's buffer: what the GPU read from it last was this slot's previous frame.
            auto count = (uint32_t) std::min<size_t>(streamingStates.size(), maxStreamingRoots);
            if (mode == (uint32_t) DebugView::Streaming) {
                auto* states = static_cast<uint32_t*>(streamingMapped[frameSlot]);
                std::copy_n(streamingStates.begin(), count, states);
            }
            auto* lit = overLit ? ctx.texture(data.lit) : ctx.texture(data.value);
            std::array<RhiDescriptorWrite, 4> writes = {{
                {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = ctx.texture(data.value), .sampler = pointSampler},
                {.binding = 1, .type = RhiDescriptorType::StorageImage, .texture = ctx.texture(data.color)},
                {.binding = 2, .type = RhiDescriptorType::StorageBuffer, .buffer = streamingBuffers[frameSlot], .bufferRange = (uint64_t) maxStreamingRoots * sizeof(uint32_t)},
                {.binding = 3, .type = RhiDescriptorType::CombinedImageSampler, .texture = lit, .sampler = pointSampler},
            }};
            device->updateDescriptorSet(resolveSets[frameSlot], writes);
            ResolvePush push = {
                .size = {extent.width, extent.height},
                .mode = mode,
                .overdrawMax = 8.0f,
                .streamingRoots = count,
            };
            auto* cmd = ctx.cmd();
            auto* resolve = registry->get(resolvePipeline);
            cmd->bindPipeline(resolve);
            cmd->bindDescriptorSet(resolve, 0, resolveSets[frameSlot]);
            cmd->pushConstants(resolve, RhiShaderStage::Compute, 0, sizeof(push), &push);
            cmd->dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        });
}

namespace renderconfig {

config::Enum debugView{"render.view.debug",
                       std::vector<std::string>(debugViewNames.begin(), debugViewNames.end()),
                       0,
                       "A debug view drawn in place of the lit image: wireframe, triangle size, overdraw, instance, mesh, material and primitive IDs, UVs, "
                       "streaming state, meshlets and the test that culls each meshlet."};

} // namespace renderconfig
