#include "presentpass.h"

namespace {
struct PresentPassData {
    FgTextureHandle target;
};
} // namespace

void addPresentPass(FrameGraph& fg, FgTextureHandle target) {
    fg.addPass<PresentPassData>(
        "PresentPass",
        [&](FrameGraphBuilder& builder, PresentPassData& data) {
            data.target = builder.read(target, FgAccessFlags::Present);
            builder.setSideEffects(true);
        },
        [](FrameGraphContext&, const PresentPassData&) {});
}
