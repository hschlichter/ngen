#include "uirender.h"

#include "rendersnapshot.h"

auto UiRender::init(const RenderExtensionContext& context) -> bool {
    frameGraph = &context.frameGraph;
    backend.init({
        .device = &context.device,
        .colorFormat = context.colorFormat,
        .imageCount = context.frameSlots,
    });
    return true;
}

auto UiRender::addOverlayPasses(Renderer& /*renderer*/, RenderSnapshot& snapshot, const RenderFrame& frame) -> void {
    auto* ui = snapshot.extension<UiFrame>(index());
    if (ui == nullptr || frame.hideUi) {
        return;
    }
    pass.addPass(*frameGraph, frame.color, frame.extent, &backend, ui->imgui);
}

auto UiRender::destroy() -> void {
    backend.shutdown();
}
