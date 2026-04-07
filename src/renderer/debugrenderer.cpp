#include "debugrenderer.h"
#include "debugdraw.h"
#include "rhicommandbuffer.h"

#include <algorithm>
#include <cstring>

struct DebugLinePassData {
    FgTextureHandle color;
    FgTextureHandle depth;
};

auto DebugRenderer::addPass(FrameGraph& fg, FgTextureHandle color, FgTextureHandle depth, RhiExtent2D extent,
                            const DebugDrawData& data, const DebugLinePassResources& res) -> void {
    auto vertexCount = std::min((uint32_t) data.lines.size(), res.maxVertices);
    if (vertexCount == 0) {
        return;
    }

    memcpy(res.vertexBufferMapped, data.lines.data(), vertexCount * sizeof(DebugVertex));

    fg.addPass<DebugLinePassData>(
        "DebugLinePass",
        [&](FrameGraphBuilder& builder, DebugLinePassData& passData) {
            passData.color = builder.write(color, FgAccessFlags::ColorAttachment);
            passData.depth = builder.write(depth, FgAccessFlags::DepthAttachment);
            builder.setSideEffects(true);
        },
        [res, vertexCount, extent](FrameGraphContext& ctx, const DebugLinePassData& passData) {
            auto* cmd = ctx.cmd();

            RhiRenderingAttachmentInfo colorAtt = {
                .texture = ctx.texture(passData.color),
                .layout = RhiImageLayout::ColorAttachment,
                .clear = false,
            };
            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(passData.depth),
                .layout = RhiImageLayout::DepthStencilAttachment,
                .clear = false,
            };
            RhiRenderingInfo renderInfo = {
                .extent = extent,
                .colorAttachments = {&colorAtt, 1},
                .depthAttachment = &depthAtt,
            };
            cmd->beginRendering(renderInfo);
            cmd->bindPipeline(res.pipeline);
            cmd->bindVertexBuffer(res.vertexBuffer);
            cmd->bindDescriptorSet(res.pipeline, res.descriptorSet);
            cmd->draw(vertexCount, 1, 0, 0);
            cmd->endRendering();
        });
}
