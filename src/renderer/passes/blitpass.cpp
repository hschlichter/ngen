#include "blitpass.h"

#include "rhicommandbuffer.h"

namespace {
struct BlitPassData {
    FgTextureHandle src;
    FgTextureHandle dst;
};
} // namespace

void addBlitPass(FrameGraph& fg, const char* name, FgTextureHandle src, FgTextureHandle dst, RhiExtent2D srcExtent, RhiExtent2D dstExtent) {
    fg.addPass<BlitPassData>(
        name,
        [&](FrameGraphBuilder& builder, BlitPassData& data) {
            data.src = builder.read(src, FgAccessFlags::TransferSrc);
            data.dst = builder.write(dst, FgAccessFlags::TransferDst);
            builder.setSideEffects(true);
        },
        [srcExtent, dstExtent](FrameGraphContext& ctx, const BlitPassData& data) {
            ctx.cmd()->blitTexture(ctx.texture(data.src), ctx.texture(data.dst), srcExtent, dstExtent);
        });
}
