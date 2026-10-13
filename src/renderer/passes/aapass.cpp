#include "aapass.h"

#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "trace.h"

#include <array>
#include <format>

namespace {
struct FxaaPush {
    float inverseSize[2];
    uint32_t enabled;
};
} // namespace

auto AAPass::init(RhiDevice* dev, PipelineRegistry& pipelines, uint32_t frameCount, RhiFormat inputFormat) -> bool {
    registry = &pipelines;
    device = dev;

    // Storage images cannot be sRGB; the blit to the backbuffer re-encodes.
    constexpr auto floatFormat = RhiFormat::R16G16B16A16_SFLOAT;
    auto neededUsage = RhiTextureUsage::Storage | RhiTextureUsage::TransferSrc;
    computeSupported = device->supportsTextureFormat(floatFormat, neededUsage);
    if (!computeSupported) {
        TRACE_WARNING("Render", "FormatFallback", "AAPass").text("R16G16B16A16_SFLOAT not usable as storage image; falling back to blit");
        format = inputFormat;
        return true;
    }
    format = floatFormat;

    std::array<RhiDescriptorBinding, 2> bindings = {{
        {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Compute},
        {.binding = 1, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute},
    }};
    setLayout = device->createDescriptorSetLayout(bindings);
    device->setDebugName(setLayout, "aa.setlayout");

    ComputePipelineRequest request = {
        .shader = {.id = "shaders/fxaa.comp", .stage = RhiShaderStage::Compute},
        .descriptorSetLayouts = {setLayout},
        .pushConstant = {.stage = RhiShaderStage::Compute, .offset = 0, .size = sizeof(FxaaPush)},
    };
    pipelineHandle = pipelines.registerCompute("aa.pipeline", request, PipelineUse::Startup);

    pool = device->createDescriptorPool(frameCount, bindings);
    device->setDebugName(pool, "aa.sets.pool");
    sets.assign(frameCount, nullptr);
    if (!device->allocateDescriptorSets(pool, setLayout, sets)) {
        return false;
    }
    for (size_t i = 0; i < sets.size(); i++) {
        device->setDebugName(sets[i], std::format("aa.set.slot{}", i).c_str());
    }
    return true;
}

auto AAPass::destroy(RhiDevice* dev) -> void {
    if (!computeSupported) {
        return;
    }
    dev->freeDescriptorSets(pool, sets);
    dev->destroyDescriptorPool(pool);
    dev->destroyDescriptorSetLayout(setLayout);
}

auto AAPass::addPass(FrameGraph& fg, FgTextureHandle input, RhiExtent2D extent, uint32_t frameSlot, RhiSampler* sampler, bool enabled) -> const AAPassData& {
    if (!computeSupported) {
        FgTextureDesc desc = {
            .width = extent.width,
            .height = extent.height,
            .format = format,
            .usage = RhiTextureUsage::TransferSrc | RhiTextureUsage::TransferDst,
        };
        return fg.addPass<AAPassData>(
            "AAPass",
            [&](FrameGraphBuilder& builder, AAPassData& data) {
                data.sceneColor = builder.read(input, FgAccessFlags::TransferSrc);
                data.sceneColorAA = builder.write(builder.createTexture("sceneColorAA", desc), FgAccessFlags::TransferDst);
            },
            [extent](FrameGraphContext& ctx, const AAPassData& data) {
                ctx.cmd()->blitTexture(ctx.texture(data.sceneColor), ctx.texture(data.sceneColorAA), extent, extent);
            });
    }

    FgTextureDesc desc = {
        .width = extent.width,
        .height = extent.height,
        .format = format,
        .usage = RhiTextureUsage::Storage | RhiTextureUsage::TransferSrc,
    };
    return fg.addPass<AAPassData>(
        "AAPass",
        [&](FrameGraphBuilder& builder, AAPassData& data) {
            data.sceneColor = builder.read(input, FgAccessFlags::ShaderRead);
            data.sceneColorAA = builder.write(builder.createTexture("sceneColorAA", desc), FgAccessFlags::StorageWrite);
        },
        [this, pipeline = registry->get(pipelineHandle), extent, frameSlot, sampler, enabled](FrameGraphContext& ctx, const AAPassData& data) {
            // Transient textures come from the pool and may differ per frame, so the
            // slot's set is rewritten here. The slot's fence guarantees nothing in flight binds it.
            std::array<RhiDescriptorWrite, 2> writes = {{
                {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = ctx.texture(data.sceneColor), .sampler = sampler},
                {.binding = 1, .type = RhiDescriptorType::StorageImage, .texture = ctx.texture(data.sceneColorAA)},
            }};
            device->updateDescriptorSet(sets[frameSlot], writes);

            FxaaPush push = {
                .inverseSize = {1.0f / (float) extent.width, 1.0f / (float) extent.height},
                .enabled = enabled ? 1u : 0u,
            };
            auto* cmd = ctx.cmd();
            cmd->bindPipeline(pipeline);
            cmd->bindDescriptorSet(pipeline, 0, sets[frameSlot]);
            cmd->pushConstants(pipeline, RhiShaderStage::Compute, 0, sizeof(push), &push);
            cmd->dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        });
}
