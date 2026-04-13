#include "gizmopass.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>
#include <cstring>

auto GizmoPass::init(RhiDevice* device, uint32_t imageCount, RhiExtent2D extent, RhiFormat colorFormat) -> bool {
    using enum RhiFormat;

    vertShader = device->createShaderModule("shaders/gizmo.vert.spv");
    fragShader = device->createShaderModule("shaders/gizmo.frag.spv");

    // Empty descriptor set layout: pipeline has no descriptors, viewProj is a push constant.
    std::array<RhiDescriptorBinding, 0> bindings = {};
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
        .vertexStride = sizeof(GizmoVertex),
        .vertexAttributes = vertexAttrs,
        .pushConstant = {.stage = RhiShaderStage::Vertex, .offset = 0, .size = sizeof(glm::mat4)},
        .viewportExtent = extent,
        .topology = RhiPrimitiveTopology::LineList,
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .backfaceCulling = false,
        .lineWidth = 4.0f,
    };
    pipeline = device->createGraphicsPipeline(pipelineDesc);

    vertexBuffers.resize(imageCount);
    vertexBuffersMapped.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++) {
        RhiBufferDesc vbDesc = {
            .size = maxVertices * sizeof(GizmoVertex),
            .usage = RhiBufferUsage::Vertex,
            .memory = RhiMemoryUsage::CpuToGpu,
        };
        vertexBuffers[i] = device->createBuffer(vbDesc);
        vertexBuffersMapped[i] = device->mapBuffer(vertexBuffers[i]);
    }

    return pipeline != nullptr;
}

auto GizmoPass::destroy(RhiDevice* device) -> void {
    device->destroyDescriptorSetLayout(descriptorSetLayout);

    for (size_t i = 0; i < vertexBuffers.size(); i++) {
        device->unmapBuffer(vertexBuffers[i]);
        device->destroyBuffer(vertexBuffers[i]);
    }

    device->destroyPipeline(pipeline);
    device->destroyShaderModule(vertShader);
    device->destroyShaderModule(fragShader);
}

struct GizmoPassData {
    FgTextureHandle color;
};

auto GizmoPass::addPass(FrameGraph& fg, FgTextureHandle color, RhiExtent2D fullExtent, std::span<GizmoDrawRequest> requests, uint32_t imageIndex) -> void {
    if (requests.empty()) {
        return;
    }

    struct DrawCmd {
        uint32_t vertexOffset;
        uint32_t vertexCount;
        glm::mat4 viewProj;
        int32_t vpX, vpY;
        RhiExtent2D vpExtent;
    };

    std::vector<DrawCmd> drawCmds;
    uint32_t totalVertices = 0;
    auto* vbMapped = static_cast<GizmoVertex*>(vertexBuffersMapped[imageIndex]);

    for (auto& req : requests) {
        auto count = (uint32_t) req.vertices.size();
        if (count == 0 || totalVertices + count > maxVertices) {
            continue;
        }
        memcpy(vbMapped + totalVertices, req.vertices.data(), count * sizeof(GizmoVertex));
        drawCmds.push_back({
            .vertexOffset = totalVertices,
            .vertexCount = count,
            .viewProj = req.viewProj,
            .vpX = req.vpX,
            .vpY = req.vpY,
            .vpExtent = req.vpExtent,
        });
        totalVertices += count;
    }

    if (drawCmds.empty()) {
        return;
    }

    auto* vb = vertexBuffers[imageIndex];
    auto* pip = pipeline;

    fg.addPass<GizmoPassData>(
        "GizmoPass",
        [&](FrameGraphBuilder& builder, GizmoPassData& data) {
            data.color = builder.write(color, FgAccessFlags::ColorAttachment);
            builder.setSideEffects(true);
        },
        [pip, vb, fullExtent, drawCmds = std::move(drawCmds)](FrameGraphContext& ctx, const GizmoPassData& data) {
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
            cmd->bindVertexBuffer(vb);

            for (const auto& dc : drawCmds) {
                cmd->pushConstants(pip, RhiShaderStage::Vertex, 0, sizeof(glm::mat4), &dc.viewProj);
                cmd->setViewport(dc.vpX, dc.vpY, dc.vpExtent);
                cmd->setScissor(dc.vpX, dc.vpY, dc.vpExtent);
                cmd->draw(dc.vertexCount, 1, dc.vertexOffset, 0);
            }

            cmd->endRendering();
        });
}
