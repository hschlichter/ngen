#include "aapass.h"

#include "rhicommandbuffer.h"

auto addAAPass(FrameGraph& fg, FgTextureHandle input, RhiExtent2D extent, RhiFormat format) -> const AAPassData& {
    FgTextureDesc desc = {
        .width = extent.width,
        .height = extent.height,
        .format = format,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::Sampled,
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
