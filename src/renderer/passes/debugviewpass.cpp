#include "debugviewpass.h"

#include "mesh.h"
#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "shaderloader.h"

#include <array>
#include <format>
#include <print>

namespace {

constexpr auto valueFormat = RhiFormat::R32G32B32A32_SFLOAT;
constexpr auto colorFormat = RhiFormat::R16G16B16A16_SFLOAT;

struct DrawPush {
    float viewport[2];
    uint32_t mode;
    uint32_t pad;
};

struct ResolvePush {
    uint32_t size[2];
    uint32_t mode;
    float overdrawMax;
};

} // namespace

auto DebugViewPass::init(RhiDevice* dev, uint32_t frameCount, RhiFormat depthFormat, RhiDescriptorSetLayout* geometrySetLayout) -> bool {
    using enum RhiFormat;
    device = dev;
    if (!device->limits().geometryShaders) {
        std::println(stderr, "DebugViewPass: no geometry shaders on this device; debug views are off");
        return true;
    }
    if (!device->supportsTextureFormat(valueFormat, RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled | RhiTextureUsage::TransferSrc) ||
        !device->supportsTextureFormat(colorFormat, RhiTextureUsage::Storage | RhiTextureUsage::TransferSrc)) {
        std::println(stderr, "DebugViewPass: value or colour format unsupported; debug views are off");
        return true;
    }

    vertShader = loadShaderModule(device, RhiShaderStage::Vertex, "shaders/debugview.vert.spv");
    geomShader = loadShaderModule(device, RhiShaderStage::Geometry, "shaders/debugview.geom.spv");
    fragShader = loadShaderModule(device, RhiShaderStage::Fragment, "shaders/debugview.frag.spv");
    resolveShader = loadShaderModule(device, RhiShaderStage::Compute, "shaders/debugview.comp.spv");
    if (vertShader == nullptr || geomShader == nullptr || fragShader == nullptr || resolveShader == nullptr) {
        return false;
    }

    std::array<RhiVertexAttribute, 4> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, position)},
        {.location = 1, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, normal)},
        {.location = 2, .binding = 0, .format = R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, color)},
        {.location = 3, .binding = 0, .format = R32G32_SFLOAT, .offset = offsetof(struct Vertex, texCoord)},
    }};
    std::array<RhiFormat, 1> colorFormats = {valueFormat};
    RhiGraphicsPipelineDesc desc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .geometryShader = geomShader,
        .descriptorSetLayouts = {&geometrySetLayout, 1},
        .pushConstant = {.stage = RhiShaderStage::Geometry | RhiShaderStage::Fragment, .offset = 0, .size = sizeof(DrawPush)},
        .colorFormats = colorFormats,
        .depthFormat = depthFormat,
        .vertexStride = sizeof(Vertex),
        .vertexAttributes = vertexAttrs,
    };
    for (int doubleSided = 0; doubleSided < 2; doubleSided++) {
        const char* cull = doubleSided != 0 ? "cullnone" : "cullback";
        desc.raster.cullMode = doubleSided != 0 ? RhiCullMode::None : RhiCullMode::Back;
        // Same position expression as the geometry pass: LessOrEqual keeps its visible surfaces.
        desc.depthFormat = depthFormat;
        desc.depth = {.testEnable = true, .writeEnable = false, .compareOp = RhiCompareOp::LessOrEqual};
        desc.blend = {};
        pipelines[doubleSided] = device->createGraphicsPipeline(desc);
        device->setDebugName(pipelines[doubleSided], std::format("debugview.pipeline.{}", cull).c_str());
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
        overdrawPipelines[doubleSided] = device->createGraphicsPipeline(desc);
        device->setDebugName(overdrawPipelines[doubleSided], std::format("debugview.overdraw.{}", cull).c_str());
        if (pipelines[doubleSided] == nullptr || overdrawPipelines[doubleSided] == nullptr) {
            return false;
        }
    }

    std::array<RhiDescriptorBinding, 2> bindings = {{
        {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Compute},
        {.binding = 1, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute},
    }};
    resolveLayout = device->createDescriptorSetLayout(bindings);
    device->setDebugName(resolveLayout, "debugview.resolve.setlayout");
    RhiComputePipelineDesc resolveDesc = {
        .shader = resolveShader,
        .descriptorSetLayouts = {&resolveLayout, 1},
        .pushConstant = {.stage = RhiShaderStage::Compute, .offset = 0, .size = sizeof(ResolvePush)},
    };
    resolvePipeline = device->createComputePipeline(resolveDesc);
    device->setDebugName(resolvePipeline, "debugview.resolve.pipeline");
    if (resolvePipeline == nullptr) {
        return false;
    }
    resolvePool = device->createDescriptorPool(frameCount, bindings);
    device->setDebugName(resolvePool, "debugview.resolve.pool");
    resolveSets.assign(frameCount, nullptr);
    if (!device->allocateDescriptorSets(resolvePool, resolveLayout, resolveSets)) {
        return false;
    }
    for (size_t i = 0; i < resolveSets.size(); i++) {
        device->setDebugName(resolveSets[i], std::format("debugview.resolve.set.slot{}", i).c_str());
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
    dev->freeDescriptorSets(resolvePool, resolveSets);
    dev->destroyDescriptorPool(resolvePool);
    dev->destroyPipeline(resolvePipeline);
    dev->destroyDescriptorSetLayout(resolveLayout);
    for (int i = 0; i < 2; i++) {
        dev->destroyPipeline(pipelines[i]);
        dev->destroyPipeline(overdrawPipelines[i]);
    }
    dev->destroyShaderModule(vertShader);
    dev->destroyShaderModule(geomShader);
    dev->destroyShaderModule(fragShader);
    dev->destroyShaderModule(resolveShader);
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
    RhiDescriptorSet* geometrySet) -> const DebugViewPassData& {
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
    bool overdraw = view == DebugView::Overdraw;
    auto mode = (uint32_t) view;

    struct DrawData {
        FgTextureHandle value;
        FgTextureHandle depth;
    };
    const auto& drawData = fg.addPass<DrawData>(
        "DebugViewPass",
        [&](FrameGraphBuilder& builder, DrawData& data) {
            data.value = builder.write(builder.createTexture("debugview.value", valueDesc), FgAccessFlags::ColorAttachment);
            if (!overdraw) {
                data.depth = builder.write(depthHandle, FgAccessFlags::DepthAttachment);
            }
            builder.read(instanceBuffer, FgAccessFlags::StorageRead);
            builder.read(drawHandles.commands, FgAccessFlags::IndirectRead);
            builder.read(drawHandles.counts, FgAccessFlags::IndirectRead);
        },
        [this, overdraw, mode, extent, &lists, drawHandles, &scene, geometrySet](FrameGraphContext& ctx, const DrawData& data) {
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
                // The geometry pass's depth, kept: the test selects its visible surfaces.
                depthAtt = {
                    .texture = ctx.texture(data.depth),
                    .state = RhiTextureState::DepthStencilAttachment,
                    .clear = false,
                };
                renderInfo.depthAttachment = &depthAtt;
            }
            cmd->beginRendering(renderInfo);
            cmd->setViewport(extent);
            cmd->setScissor(extent);
            DrawPush push = {
                .viewport = {(float) extent.width, (float) extent.height},
                .mode = mode,
            };
            auto* commands = ctx.buffer(drawHandles.commands);
            auto* counts = ctx.buffer(drawHandles.counts);
            for (bool doubleSided : {false, true}) {
                if (scene.indexBuffer() == nullptr) {
                    continue;
                }
                auto region = DrawLists::cameraRegion(doubleSided);
                auto* pip = overdraw ? overdrawPipelines[doubleSided ? 1 : 0] : pipelines[doubleSided ? 1 : 0];
                cmd->bindPipeline(pip);
                cmd->bindDescriptorSet(pip, 0, geometrySet);
                cmd->bindVertexBuffer(scene.vertexBuffer());
                cmd->bindIndexBuffer(scene.indexBuffer(), RhiIndexType::Uint32);
                cmd->pushConstants(pip, RhiShaderStage::Geometry | RhiShaderStage::Fragment, 0, sizeof(push), &push);
                PROFILE_GPU_ZONE(cmd, DrawLists::regionName(region));
                cmd->drawIndexedIndirectCount(commands, lists.commandOffset(region), counts, DrawLists::countOffset(region), lists.regionCapacity());
            }
            cmd->endRendering();
        });

    auto valueHandle = drawData.value;
    return fg.addPass<DebugViewPassData>(
        "DebugViewResolve",
        [&](FrameGraphBuilder& builder, DebugViewPassData& data) {
            data.value = builder.read(valueHandle, FgAccessFlags::ShaderRead);
            data.color = builder.write(builder.createTexture("debugview.color", colorDesc), FgAccessFlags::StorageWrite);
        },
        [this, mode, extent, frameSlot](FrameGraphContext& ctx, const DebugViewPassData& data) {
            std::array<RhiDescriptorWrite, 2> writes = {{
                {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = ctx.texture(data.value), .sampler = pointSampler},
                {.binding = 1, .type = RhiDescriptorType::StorageImage, .texture = ctx.texture(data.color)},
            }};
            device->updateDescriptorSet(resolveSets[frameSlot], writes);
            ResolvePush push = {
                .size = {extent.width, extent.height},
                .mode = mode,
                .overdrawMax = 8.0f,
            };
            auto* cmd = ctx.cmd();
            cmd->bindPipeline(resolvePipeline);
            cmd->bindDescriptorSet(resolvePipeline, 0, resolveSets[frameSlot]);
            cmd->pushConstants(resolvePipeline, RhiShaderStage::Compute, 0, sizeof(push), &push);
            cmd->dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        });
}
