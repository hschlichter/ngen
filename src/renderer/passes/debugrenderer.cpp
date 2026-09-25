#include "debugrenderer.h"
#include "debugdraw.h"
#include "renderertypes.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "shaderloader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

auto DebugRenderer::init(
    RhiDevice* device, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat, RhiFormat depthFormat, std::span<RhiBuffer*> uniformBuffers) -> bool {
    using enum RhiDescriptorType;
    using enum RhiFormat;

    vertShader = loadShaderModule(device, RhiShaderStage::Vertex, "shaders/debug.vert.spv");
    fragShader = loadShaderModule(device, RhiShaderStage::Fragment, "shaders/debug.frag.spv");

    std::array<RhiDescriptorBinding, 1> bindings = {{
        {.binding = 0, .type = UniformBuffer, .stage = RhiShaderStage::Vertex},
    }};
    descriptorSetLayout = device->createDescriptorSetLayout(bindings);
    device->setDebugName(descriptorSetLayout, "debuglines.setlayout");

    std::array<RhiVertexAttribute, 2> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = R32G32B32A32_SFLOAT, .offset = sizeof(float) * 3},
    }};

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayouts = {&descriptorSetLayout, 1},
        .colorFormats = {&colorFormat, 1},
        .depthFormat = depthFormat,
        .vertexStride = sizeof(DebugVertex),
        .vertexAttributes = vertexAttrs,
        .topology = RhiPrimitiveTopology::LineList,
        .depth = {.testEnable = true, .writeEnable = false},
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);
    device->setDebugName(pipeline, "debuglines.pipeline");

    vertexBuffers.resize(imageCount);
    vertexBuffersMapped.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        RhiBufferDesc vbDesc = {
            .size = maxVertices * sizeof(DebugVertex),
            .usage = RhiBufferUsage::Vertex,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        vertexBuffers[i] = device->createBuffer(vbDesc);
        device->setDebugName(vertexBuffers[i], "debuglines.vertices");
        vertexBuffersMapped[i] = device->mapBuffer(vertexBuffers[i]);
    }

    descriptorPool = device->createDescriptorPool(imageCount, bindings);
    device->setDebugName(descriptorPool, "debuglines.sets.pool");
    descriptorSets.assign(imageCount, nullptr);
    device->allocateDescriptorSets(descriptorPool, descriptorSetLayout, descriptorSets);
    for (size_t i = 0; i < descriptorSets.size(); i++) {
        device->setDebugName(descriptorSets[i], std::format("debuglines.set.slot{}", i).c_str());
    }
    for (uint32_t i = 0; i < imageCount; i++) {
        std::array<RhiDescriptorWrite, 1> writes = {{
            {
                .binding = 0,
                .type = UniformBuffer,
                .buffer = uniformBuffers[i],
                .bufferRange = sizeof(UniformBufferObject),
            },
        }};
        device->updateDescriptorSet(descriptorSets[i], writes);
    }

    return pipeline != nullptr;
}

auto DebugRenderer::destroy(RhiDevice* device) -> void {
    device->freeDescriptorSets(descriptorPool, descriptorSets);
    device->destroyDescriptorPool(descriptorPool);
    device->destroyDescriptorSetLayout(descriptorSetLayout);

    for (size_t i = 0; i < vertexBuffers.size(); i++) {
        device->unmapBuffer(vertexBuffers[i]);
        device->destroyBuffer(vertexBuffers[i]);
    }

    device->destroyPipeline(pipeline);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
}

struct DebugLinePassData {
    FgTextureHandle color;
    FgTextureHandle depth;
};

auto DebugRenderer::addPass(FrameGraph& fg, FgTextureHandle color, FgTextureHandle depth, RhiExtent2D extent, const DebugDrawData& data, uint32_t imageIndex)
    -> void {
    auto vertexCount = std::min((uint32_t) data.lines.size(), maxVertices);
    if (vertexCount == 0) {
        return;
    }

    memcpy(vertexBuffersMapped[imageIndex], data.lines.data(), vertexCount * sizeof(DebugVertex));

    auto* vb = vertexBuffers[imageIndex];
    auto* ds = descriptorSets[imageIndex];
    auto* pip = pipeline;

    fg.addPass<DebugLinePassData>(
        "DebugLinePass",
        [&](FrameGraphBuilder& builder, DebugLinePassData& passData) {
            passData.color = builder.write(color, FgAccessFlags::ColorAttachment);
            passData.depth = builder.write(depth, FgAccessFlags::DepthAttachment);
            builder.setSideEffects(true);
        },
        [pip, ds, vb, vertexCount, extent](FrameGraphContext& ctx, const DebugLinePassData& passData) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo colorAtt = {
                .texture = ctx.texture(passData.color),
                .state = RhiTextureState::ColorAttachment,
                .clear = false,
            };
            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(passData.depth),
                .state = RhiTextureState::DepthStencilAttachment,
                .clear = false,
            };
            RhiRenderingInfo renderInfo = {
                .extent = extent,
                .colorAttachments = {&colorAtt, 1},
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(renderInfo);
            cmd->bindPipeline(pip);
            cmd->setViewport(extent);
            cmd->setScissor(extent);
            cmd->bindVertexBuffer(vb);
            cmd->bindDescriptorSet(pip, 0, ds);
            cmd->draw(vertexCount, 1, 0, 0);
            cmd->endRendering();
        });
}
