#include "shadowpass.h"

#include "rhicommandbuffer.h"

auto addShadowPass(FrameGraph& fg, RhiExtent2D extent, RhiFormat depthFormat) -> const ShadowPassData& {
    FgTextureDesc desc = {
        .width = extent.width,
        .height = extent.height,
        .format = depthFormat,
        .usage = RhiTextureUsage::DepthAttachment | RhiTextureUsage::Sampled,
    };

    return fg.addPass<ShadowPassData>(
        "ShadowPass",
        [&](FrameGraphBuilder& builder, ShadowPassData& data) {
            data.shadowMap = builder.write(builder.createTexture("shadowMap", desc), FgAccessFlags::DepthAttachment);
        },
        [extent](FrameGraphContext& ctx, const ShadowPassData& data) {
            RhiRenderingAttachmentInfo depthAtt = {
                .texture = ctx.texture(data.shadowMap),
                .layout = RhiImageLayout::DepthStencilAttachment,
                .clear = true,
                .clearDepth = 1.0f,
            };
            RhiRenderingInfo info = {
                .extent = extent,
                .depthAttachment = &depthAtt,
            };
            auto* cmd = ctx.cmd();
            cmd->beginRendering(info);
            cmd->endRendering();
        });
}
