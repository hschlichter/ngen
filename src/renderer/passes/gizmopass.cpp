#include "gizmopass.h"
#include "debugdraw.h"
#include "renderertypes.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstring>

auto GizmoPass::init(RhiDevice* device, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool {
    using enum RhiDescriptorType;
    using enum RhiFormat;

    vertShader = device->createShaderModule("shaders/debug.vert.spv");
    fragShader = device->createShaderModule("shaders/debug.frag.spv");

    std::array<RhiDescriptorBinding, 1> bindings = {{
        {.binding = 0, .type = UniformBuffer, .stage = RhiShaderStage::Vertex},
    }};
    descriptorSetLayout = device->createDescriptorSetLayout(bindings);

    std::array<RhiVertexAttribute, 2> vertexAttrs = {{
        {.location = 0, .binding = 0, .format = R32G32B32_SFLOAT, .offset = 0},
        {.location = 1, .binding = 0, .format = R32G32B32A32_SFLOAT, .offset = sizeof(float) * 3},
    }};

    RhiGraphicsPipelineDesc pipelineDesc = {
        .vertexShader = vertShader,
        .fragmentShader = fragShader,
        .descriptorSetLayout = descriptorSetLayout,
        .colorFormats = {&colorFormat, 1},
        .depthFormat = Undefined,
        .vertexStride = sizeof(DebugVertex),
        .vertexAttributes = vertexAttrs,
        .pushConstant = {},
        .viewportExtent = extent,
        .topology = RhiPrimitiveTopology::LineList,
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .backfaceCulling = false,
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);

    // Static vertex buffer: 3 axes from origin outward
    std::array<DebugVertex, 6> axes = {{
        {{0, 0, 0}, {1, 0, 0, 1}},
        {{1, 0, 0}, {1, 0, 0, 1}},
        {{0, 0, 0}, {0, 1, 0, 1}},
        {{0, 1, 0}, {0, 1, 0, 1}},
        {{0, 0, 0}, {0, 0.4f, 1, 1}},
        {{0, 0, 1}, {0, 0.4f, 1, 1}},
    }};

    RhiBufferDesc vbDesc = {
        .size = sizeof(axes),
        .usage = RhiBufferUsage::Vertex,
        .memory = RhiMemoryUsage::CpuToGpu,
    };
    vertexBuffer = device->createBuffer(vbDesc);
    auto* mapped = device->mapBuffer(vertexBuffer);
    memcpy(mapped, axes.data(), sizeof(axes));
    device->unmapBuffer(vertexBuffer);

    // Per-frame UBOs
    uniformBuffers.resize(imageCount);
    uniformBuffersMapped.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        RhiBufferDesc uboDesc = {
            .size = sizeof(UniformBufferObject),
            .usage = RhiBufferUsage::Uniform,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        uniformBuffers[i] = device->createBuffer(uboDesc);
        uniformBuffersMapped[i] = device->mapBuffer(uniformBuffers[i]);
    }

    descriptorPool = device->createDescriptorPool(imageCount, bindings);
    descriptorSets = device->allocateDescriptorSets(descriptorPool, descriptorSetLayout, imageCount);
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

auto GizmoPass::destroy(RhiDevice* device) -> void {
    for (auto* ds : descriptorSets) {
        delete ds;
    }
    device->destroyDescriptorPool(descriptorPool);
    device->destroyDescriptorSetLayout(descriptorSetLayout);

    for (size_t i = 0; i < uniformBuffers.size(); i++) {
        device->unmapBuffer(uniformBuffers[i]);
        device->destroyBuffer(uniformBuffers[i]);
    }

    device->destroyBuffer(vertexBuffer);
    device->destroyPipeline(pipeline);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
}

auto GizmoPass::addPass(FrameGraph& fg, FgTextureHandle color, RhiExtent2D fullExtent, const glm::mat4& viewMatrix, uint32_t imageIndex) -> void {
    constexpr uint32_t gizmoSize = 120;
    constexpr uint32_t margin = 50;

    // Push the gizmo back so it's fully visible with perspective
    auto rotView = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)) * glm::mat4(glm::mat3(viewMatrix));
    auto proj = glm::perspective(glm::radians(30.0f), 1.0f, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;

    UniformBufferObject ubo = {.view = rotView, .proj = proj};
    memcpy(uniformBuffersMapped[imageIndex], &ubo, sizeof(ubo));

    auto vpX = (int32_t) (fullExtent.width - gizmoSize - margin);
    auto vpY = (int32_t) margin;
    RhiExtent2D gizmoExtent = {gizmoSize, gizmoSize};

    auto* vb = vertexBuffer;
    auto* ds = descriptorSets[imageIndex];
    auto* pip = pipeline;

    struct GizmoPassData {
        FgTextureHandle color;
    };

    fg.addPass<GizmoPassData>(
        "GizmoPass",
        [&](FrameGraphBuilder& builder, GizmoPassData& data) {
            data.color = builder.write(color, FgAccessFlags::ColorAttachment);
            builder.setSideEffects(true);
        },
        [pip, ds, vb, fullExtent, gizmoExtent, vpX, vpY](FrameGraphContext& ctx, const GizmoPassData& data) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo colorAtt = {
                .texture = ctx.texture(data.color),
                .layout = RhiImageLayout::ColorAttachment,
                .clear = false,
            };
            RhiRenderingInfo renderInfo = {
                .extent = fullExtent,
                .colorAttachments = {&colorAtt, 1},
            };
            cmd->beginRendering(renderInfo);
            cmd->bindPipeline(pip);
            cmd->setViewport(vpX, vpY, gizmoExtent);
            cmd->setScissor(vpX, vpY, gizmoExtent);
            cmd->bindVertexBuffer(vb);
            cmd->bindDescriptorSet(pip, ds);
            cmd->draw(6, 1, 0, 0);
            cmd->endRendering();
        });
}
