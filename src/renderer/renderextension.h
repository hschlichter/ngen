#pragma once

#include "framegraphresource.h"
#include "rhitypes.h"

#include <array>
#include <cstdint>

class DeletionQueue;
class FrameGraph;
class GpuUploader;
class PipelineRegistry;
class PreviewTextures;
class Renderer;
class RhiCommandBuffer;
class RhiDevice;
class RhiSampler;
struct RenderSnapshot;

// An extension's data for one frame, made on the main thread and carried in the RenderSnapshot (RenderSnapshot::extensions, at the
// extension's index), so it arrives with the frame it belongs to.
struct RenderExtensionData {
    RenderExtensionData() = default;
    RenderExtensionData(const RenderExtensionData&) = delete;
    auto operator=(const RenderExtensionData&) -> RenderExtensionData& = delete;
    RenderExtensionData(RenderExtensionData&&) = default;
    auto operator=(RenderExtensionData&&) -> RenderExtensionData& = default;
    virtual ~RenderExtensionData() = default;
};

// What the renderer lends its extensions for their life: the device, the pipeline registry (pipelines registered at init are among the
// at-start ones), the frame graph, deferred destruction, the uploader, the sampler previews are drawn with, and where previews are shown.
struct RenderExtensionContext {
    RhiDevice& device;
    PipelineRegistry& pipelines;
    FrameGraph& frameGraph;
    DeletionQueue& deletionQueue;
    GpuUploader& uploader;
    RhiSampler* textureSampler = nullptr;
    PreviewTextures* previews = nullptr; // none: no previews
    uint32_t frameSlots = 0;
    RhiFormat colorFormat = RhiFormat::Undefined;
};

// The frame being recorded, as the extensions see it. The backbuffer and the graph's targets are set once the swapchain image is acquired,
// before addOverlayPasses.
struct RenderFrame {
    uint64_t frame = 0;
    uint32_t slot = 0; // the frame slot: its command buffer, fence and per-slot buffers
    RhiExtent2D extent = {};
    RhiTexture* backbuffer = nullptr;
    FgTextureHandle color; // the backbuffer in the frame graph
    FgTextureHandle depth;
    bool hideUi = false; // a screenshot taken without the UI: set by an extension at beginFrame, read by the UI's
};

// What lies outside the renderer's core joins its frame here (src/renderer/README.md, "Extensions"): the instrumentation (src/rendertools),
// the tools' gizmos (src/tools) and the UI (src/ui). The renderer calls each at fixed points of Renderer::render, in the order they were
// added; each hook does nothing unless overridden. Called on the render thread, except init and destroy, which run where the renderer's do.
class RenderExtension {
public:
    RenderExtension() = default;
    RenderExtension(const RenderExtension&) = delete;
    auto operator=(const RenderExtension&) -> RenderExtension& = delete;
    RenderExtension(RenderExtension&&) = delete;
    auto operator=(RenderExtension&&) -> RenderExtension& = delete;
    virtual ~RenderExtension() = default;

    // At the end of Renderer::init: pipelines, resources. False fails the init.
    virtual auto init(const RenderExtensionContext& /*context*/) -> bool { return true; }
    // First in every call to Renderer::render, whether it records or not: the extension takes its data from the snapshot (what a frame that
    // doesn't record asked for waits for the next that does).
    virtual auto beginFrame(Renderer& /*renderer*/, RenderSnapshot& /*snapshot*/, RenderFrame& /*frame*/) -> void {}
    // A frame slot's fence passed: what that slot's frame recorded can be read.
    virtual auto afterFence(Renderer& /*renderer*/, uint32_t /*slot*/) -> void {}
    // Passes on the backbuffer after the debug lines, before present.
    virtual auto addOverlayPasses(Renderer& /*renderer*/, RenderSnapshot& /*snapshot*/, const RenderFrame& /*frame*/) -> void {}
    // On the frame's command buffer: before it is reset and the graph recorded, and right after the graph is recorded.
    virtual auto beforeExecute(Renderer& /*renderer*/, RhiCommandBuffer* /*cmd*/, const RenderFrame& /*frame*/) -> void {}
    virtual auto afterExecute(Renderer& /*renderer*/, RhiCommandBuffer* /*cmd*/, const RenderFrame& /*frame*/) -> void {}
    // The frame is submitted, not yet presented; it may wait for the frame's fence (Renderer::waitForFrame).
    virtual auto afterSubmit(Renderer& /*renderer*/, const RenderFrame& /*frame*/) -> void {}
    // Every call to Renderer::render ends here, whether it presented or not.
    virtual auto endFrame(Renderer& /*renderer*/) -> void {}
    // Before the renderer destroys its own resources, in the order added.
    virtual auto destroy() -> void {}

    // The extension's place among the renderer's: its frame data's index in RenderSnapshot::extensions.
    auto index() const -> uint32_t { return extensionIndex; }

private:
    friend class Renderer;
    // The hooks the renderer times, one detail zone each, named "<extension>.<hook>" at addExtension.
    enum Hook : uint32_t {
        HookBeginFrame,
        HookAfterFence,
        HookAddOverlayPasses,
        HookBeforeExecute,
        HookAfterExecute,
        HookAfterSubmit,
        HookEndFrame,
        HookCount,
    };
    uint32_t extensionIndex = 0;
    std::array<uint32_t, HookCount> hookZoneIds = {};
};
