#include "shadowpass.h"
#include "renderertypes.h"

#include "mesh.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "shaderloader.h"

#include <array>

namespace {
struct ShadowPush {
    glm::mat4 lightViewProj;
    glm::mat4 model;
};
} // namespace

auto ShadowPass::init(RhiDevice* device, RhiExtent2D extent, RhiFormat depthFormat) -> bool {
    using enum RhiFormat;

    vertShader = loadShaderModule(device, RhiShaderStage::Vertex, "shaders/shadow.vert.spv");
    fragShader = loadShaderModule(device, RhiShaderStage::Fragment, "shaders/shadow.frag.spv");

    std::array<RhiVertexAttribute, 1> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = 0}, // position-only stream
    }};

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayouts = {}, // no descriptors; all data via push constants
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(ShadowPush)},
        .colorFormats = {}, // depth-only
        .depthFormat = depthFormat,
        .vertexStride = sizeof(std::array<float, 3>),
        .vertexAttributes = vertexAttrs,
        .raster = {.cullMode = RhiCullMode::Back},
    };
    pipelineCullBack = device->createGraphicsPipeline(pipelineDesc);
    pipelineDesc.raster.cullMode = RhiCullMode::None;
    pipelineCullNone = device->createGraphicsPipeline(pipelineDesc);
    return pipelineCullBack != nullptr && pipelineCullNone != nullptr;
}

auto ShadowPass::destroy(RhiDevice* device) -> void {
    device->destroyPipeline(pipelineCullBack);
    device->destroyPipeline(pipelineCullNone);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
}

auto ShadowPass::addPass(
    FrameGraph& fg,
    RhiExtent2D extent,
    RhiFormat depthFormat,
    const glm::mat4& lightViewProj,
    std::span<const GpuInstance> instances,
    const std::unordered_map<uint32_t, CachedMesh>& meshCache) -> const ShadowPassData& {
    FgTextureDesc desc = {
        .width = extent.width,
        .height = extent.height,
        .format = depthFormat,
        .usage = RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled,
    };

    auto* cullBack = pipelineCullBack;
    auto* cullNone = pipelineCullNone;

    return fg.addPass<ShadowPassData>(
        "ShadowPass",
        [&](FrameGraphBuilder& builder, ShadowPassData& data) {
            data.shadowMap = builder.write(builder.createTexture("shadowMap", desc), FgAccessFlags::DepthAttachment);
        },
        [cullBack, cullNone, extent, lightViewProj, instances, &meshCache](FrameGraphContext& ctx, const ShadowPassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.shadowMap),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo info = {
                .extent = extent,
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(info);
            cmd->setViewport(extent);
            cmd->setScissor(extent);

            // Single-sided meshes under back-face culling first, then double-sided ones
            // without it: one pipeline bind per group.
            for (bool doubleSided : {false, true}) {
                auto* pip = doubleSided ? cullNone : cullBack;
                bool bound = false;
                for (uint32_t m = 0; m < (uint32_t) instances.size(); m++) {
                    const auto& inst = instances[m];
                    // Instances are expanded per material submesh, but shadows are
                    // material-agnostic — draw the whole mesh once, on the prim's
                    // first submesh instance, and skip the rest.
                    if (!inst.primFirst || inst.doubleSided != doubleSided) {
                        continue;
                    }
                    auto meshIt = meshCache.find(inst.mesh.index);
                    if (meshIt == meshCache.end()) {
                        continue;
                    }
                    const auto& cached = meshIt->second;
                    if (!bound) {
                        cmd->bindPipeline(pip);
                        bound = true;
                    }

                    ShadowPush push{lightViewProj, inst.transform};
                    cmd->pushConstants(pip, RhiShaderStage::Vertex, 0, sizeof(push), &push);
                    cmd->bindVertexBuffer(cached.positionBuffer);
                    cmd->bindIndexBuffer(cached.indexBuffer, RhiIndexType::Uint32);
                    bool heavy = cached.indexCount >= largeDrawIndexCount;
                    if (heavy) {
                        cmd->beginGpuZone("LargeDraw");
                    }
                    ctx.beginDraw({.instance = m, .mesh = inst.mesh.index, .material = inst.material.index, .prim = inst.prim, .indexOffset = 0, .indexCount = cached.indexCount});
                    cmd->drawIndexed(cached.indexCount, 1, 0, 0, 0);
                    ctx.endDraw();
                    if (heavy) {
                        cmd->endGpuZone();
                    }
                }
            }

            cmd->endRendering();
        });
}
