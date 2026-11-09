#include "depthpyramid.h"

#include "framegraph.h"
#include "framegraphcontext.h"
#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <span>

namespace {

enum class BuildMode : uint32_t {
    FromDepth = 0,
    FromLevel = 1,
};

struct BuildPush {
    uint32_t mode = 0;
    uint32_t pad = 0;
    uint32_t targetSize[2] = {};
    uint32_t sourceSize[2] = {};
};

constexpr std::array<RhiDescriptorBinding, 3> buildBindings = {{
    {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Compute}, // depth
    {.binding = 1, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute},         // the level below
    {.binding = 2, .type = RhiDescriptorType::StorageImage, .stage = RhiShaderStage::Compute},         // the level written
}};

struct BuildPassData {
    FgTextureHandle pyramid;
};

} // namespace

auto DepthPyramid::init(RhiDevice* rhiDevice, PipelineRegistry& pipelines) -> bool {
    device = rhiDevice;
    registry = &pipelines;
    setLayout = device->createDescriptorSetLayout(buildBindings);
    device->setDebugName(setLayout, "hzb.setlayout");
    ComputePipelineRequest request = {
        .shader = {.id = "shaders/hzbbuild.comp", .stage = RhiShaderStage::Compute},
        .descriptorSetLayouts = {setLayout},
        .pushConstant = {.stage = RhiShaderStage::Compute, .offset = 0, .size = sizeof(BuildPush)},
    };
    pipelineHandle = pipelines.registerCompute("hzb.pipeline", request, PipelineUse::AtStart);
    // The tests read with texelFetch; the sampler is only the descriptor's.
    pointSampler = device->createSampler({
        .magFilter = RhiFilter::Nearest,
        .minFilter = RhiFilter::Nearest,
        .mipmapMode = RhiMipmapMode::Nearest,
        .addressU = RhiAddressMode::ClampToEdge,
        .addressV = RhiAddressMode::ClampToEdge,
        .addressW = RhiAddressMode::ClampToEdge,
        .debugName = "hzb.sampler",
    });
    return true;
}

auto DepthPyramid::releaseTexture() -> void {
    if (pool != nullptr) {
        sets.insert(sets.end(), cullSets.begin(), cullSets.end());
        device->freeDescriptorSets(pool, sets);
        cullSets.clear();
        device->destroyDescriptorPool(pool);
        pool = nullptr;
        sets.clear();
    }
    if (pyramid != nullptr) {
        device->destroyTexture(pyramid);
        pyramid = nullptr;
    }
}

auto DepthPyramid::destroy() -> void {
    releaseTexture();
    device->destroySampler(pointSampler);
    device->destroyDescriptorSetLayout(setLayout);
}

auto DepthPyramid::resize(RhiExtent2D depthExtent, RhiTexture* viewDepth, RhiTexture* cullDepth) -> void {
    releaseTexture();
    depthSize = depthExtent;
    size = {std::bit_floor(std::max(1u, depthExtent.width)), std::bit_floor(std::max(1u, depthExtent.height))};
    mips = (uint32_t) std::bit_width(std::max(size.width, size.height));
    pyramid = device->createTexture({
        .width = size.width,
        .height = size.height,
        .format = RhiFormat::R32_SFLOAT,
        // TransferSrc: the capture service reads it back.
        .usage = RhiTextureUsage::Storage | RhiTextureUsage::Sampled | RhiTextureUsage::TransferSrc,
        .mipLevels = mips,
        .debugName = "hzb",
    });
    // A set per level for each source depth.
    pool = device->createDescriptorPool(mips * 2, buildBindings);
    device->setDebugName(pool, "hzb.sets.pool");
    sets.assign(mips * 2, nullptr);
    device->allocateDescriptorSets(pool, setLayout, sets);
    cullSets.assign(sets.begin() + mips, sets.end());
    sets.resize(mips);
    auto writeLevels = [&](std::span<RhiDescriptorSet* const> levelSets, RhiTexture* depth, const char* source) {
        for (uint32_t level = 0; level < mips; level++) {
            device->setDebugName(levelSets[level], std::format("hzb.set.{}.mip{}", source, level).c_str());
            std::array<RhiDescriptorWrite, 3> writes = {{
                {.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .texture = depth, .sampler = pointSampler},
                {.binding = 1, .type = RhiDescriptorType::StorageImage, .texture = pyramid, .mipLevel = level > 0 ? level - 1 : 0},
                {.binding = 2, .type = RhiDescriptorType::StorageImage, .texture = pyramid, .mipLevel = level},
            }};
            device->updateDescriptorSet(levelSets[level], writes);
        }
    };
    writeLevels(sets, viewDepth, "view");
    writeLevels(cullSets, cullDepth, "cull");
}

auto DepthPyramid::import(FrameGraph& fg) -> FgTextureHandle {
    return fg.importTexture("hzb", pyramid, {size.width, size.height, RhiFormat::R32_SFLOAT, RhiTextureUsage::Storage | RhiTextureUsage::Sampled});
}

auto DepthPyramid::addBuildPass(FrameGraph& fg, const char* name, Source source, FgTextureHandle depth, FgTextureHandle pyramidHandle) -> void {
    auto* pip = registry->get(pipelineHandle);
    auto levelSets = source == Source::Culling ? cullSets : sets;
    auto pyramidSize = size;
    auto depthExtent = depthSize;
    fg.addPass<BuildPassData>(
        name,
        profile::Group::Culling,
        [&](FrameGraphBuilder& builder, BuildPassData& data) {
            builder.read(depth, FgAccessFlags::ShaderRead);
            data.pyramid = builder.write(pyramidHandle, FgAccessFlags::StorageWrite);
        },
        [pip, levelSets, pyramidSize, depthExtent](FrameGraphContext& ctx, const BuildPassData& data) {
            auto* cmd = ctx.cmd();
            cmd->bindPipeline(pip);
            RhiExtent2D source = depthExtent;
            RhiExtent2D target = pyramidSize;
            // Static names: a GPU zone keeps its name until the frame is read back.
            constexpr std::array<const char*, 16> levelNames = {
                "HzbLevel0",
                "HzbLevel1",
                "HzbLevel2",
                "HzbLevel3",
                "HzbLevel4",
                "HzbLevel5",
                "HzbLevel6",
                "HzbLevel7",
                "HzbLevel8",
                "HzbLevel9",
                "HzbLevel10",
                "HzbLevel11",
                "HzbLevel12",
                "HzbLevel13",
                "HzbLevel14",
                "HzbLevel15",
            };
            for (uint32_t level = 0; level < levelSets.size(); level++) {
                if (level > 0) {
                    // The level below is written: its texels are read next.
                    ctx.storageBarrier(data.pyramid);
                }
                PROFILE_GPU_ZONE_DETAIL(cmd, levelNames[std::min<size_t>(level, levelNames.size() - 1)], Culling);
                BuildPush push = {
                    .mode = (uint32_t) (level == 0 ? BuildMode::FromDepth : BuildMode::FromLevel),
                    .targetSize = {target.width, target.height},
                    .sourceSize = {source.width, source.height},
                };
                cmd->bindDescriptorSet(pip, 0, levelSets[level]);
                cmd->pushConstants(pip, RhiShaderStage::Compute, 0, sizeof(push), &push);
                cmd->dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);
                source = target;
                target = {std::max(1u, target.width / 2), std::max(1u, target.height / 2)};
            }
        });
}

auto DepthPyramid::ready() -> bool {
    std::array<PipelineHandle, 1> handles = {pipelineHandle};
    return registry->ready(handles);
}
