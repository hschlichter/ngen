#include "gizmorender.h"

#include "framegraph.h"
#include "profile.h"
#include "rendersnapshot.h"

auto GizmoRender::init(const RenderExtensionContext& context) -> bool {
    device = &context.device;
    frameGraph = &context.frameGraph;
    return pass.init(device, context.pipelines, context.frameSlots, context.colorFormat);
}

auto GizmoRender::addOverlayPasses(Renderer& /*renderer*/, RenderSnapshot& snapshot, const RenderFrame& frame) -> void {
    auto* gizmos = snapshot.extension<GizmoFrame>(index());
    if (gizmos == nullptr || !pass.ready()) {
        return;
    }
    PROFILE_ZONE("GizmoUpdate", Tools);
    // The requests point into the snapshot's vertices, which outlive the frame's recording.
    requests.clear();
    for (const auto& draw : gizmos->draws) {
        requests.push_back({
            .vertices = draw.vertices,
            .viewProj = draw.viewProj,
            .vpX = draw.vpX,
            .vpY = draw.vpY,
            .vpExtent = draw.vpExtent.width > 0 ? draw.vpExtent : frame.extent,
        });
    }
    pass.addPass(*frameGraph, frame.color, frame.extent, requests, frame.slot);
}

auto GizmoRender::destroy() -> void {
    pass.destroy(device);
}
