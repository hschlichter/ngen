#pragma once

#include "framedebug.h"
#include "framegraphdebug.h"
#include "framegraphpreviews.h"
#include "gpucounters.h"
#include "renderdebug.h"
#include "renderextension.h"
#include "screenshot.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class RhiBuffer;
class RhiTexture;

// One mip level of a material texture written as PNG after its frame (`dump-texture`, the Render Debug window).
struct TextureDumpRequest {
    uint32_t texture = 0; // TextureHandle::index
    uint32_t level = 0;
    std::string path;
};

// What the main thread asks of the render tools for one frame, carried in its snapshot: the frame graph's debug snapshot and previews, the
// render debug snapshot (and the draw log), GPU counters, a frame debug capture, the texture inspector, a texture dump, a screenshot.
struct RenderToolsFrame : RenderExtensionData {
    bool frameGraphDebug = false;
    bool renderDebug = false;
    bool counters = false;
    bool frameDebug = false; // this frame recorded with the command log, kept as a FrameDebugCapture
    TextureInspectRequest textureInspect;
    std::vector<TextureDumpRequest> textureDumps;
    std::vector<std::string> screenshots; // paths
};

// What came back from the render thread since the main thread last took it.
struct RenderToolsResults {
    std::optional<FrameGraphDebugSnapshot> frameGraph;
    std::optional<RenderDebugSnapshot> renderDebug;
    std::optional<FrameDebugCapture> frameDebug;
    std::vector<ScreenshotResult> screenshots;
    std::vector<GpuCounters> counters;
};

// The renderer's instrumentation (src/rendertools/README.md): the frame graph's debug snapshot and previews, the render debug snapshot, the
// frame debugger, GPU counters, the texture inspector and dumps, screenshots. A render extension: it joins the renderer's frame at its hooks
// on the render thread, and its main-thread side gathers each frame's requests into the snapshot and hands back what came back. Nothing runs
// unless asked for.
class RenderTools : public RenderExtension {
public:
    // Main thread, before the renderer starts: screenshots keep the UI (--show-ui), the device runs with validation (for the render debug
    // snapshot).
    auto setScreenshotsShowUi(bool show) -> void { screenshotsShowUi = show; }
    auto setValidationEnabled(bool enabled) -> void { validationEnabled = enabled; }

    // Main thread, once a frame: startFrame takes what came back since the last frame into results() and starts this frame's requests;
    // the readers (the dumps, the UI) add theirs to request() and take theirs from results(); fillSnapshot puts the requests in the
    // snapshot.
    auto startFrame() -> void;
    auto request() -> RenderToolsFrame& { return next; }
    auto results() -> RenderToolsResults& { return taken; }
    auto fillSnapshot(RenderSnapshot& snapshot) -> void;
    // Main thread, any time: asked for in the next snapshot. The inspector's selection holds until changed.
    auto requestScreenshot(std::string path) -> void;
    auto requestTextureDump(uint32_t texture, uint32_t level, std::string path) -> void;
    auto setTextureInspect(const TextureInspectRequest& request) -> void { textureInspect = request; }

    // The render extension's hooks (renderextension.h), on the render thread.
    auto init(const RenderExtensionContext& context) -> bool override;
    auto afterFence(Renderer& renderer, uint32_t slot) -> void override;
    auto beginFrame(Renderer& renderer, RenderSnapshot& snapshot, RenderFrame& frame) -> void override;
    auto beforeExecute(Renderer& renderer, RhiCommandBuffer* cmd, const RenderFrame& frame) -> void override;
    auto afterExecute(Renderer& renderer, RhiCommandBuffer* cmd, const RenderFrame& frame) -> void override;
    auto afterSubmit(Renderer& renderer, const RenderFrame& frame) -> void override;
    auto endFrame(Renderer& renderer) -> void override;
    auto destroy() -> void override;

private:
    // The frame graph's debug snapshot: its passes and resources, the previews, the GPU's timings.
    auto buildFrameGraphDebugSnapshot(const Renderer& renderer) const -> FrameGraphDebugSnapshot;
    auto buildRenderDebugSnapshot(const Renderer& renderer) const -> RenderDebugSnapshot;
    auto recordTextureInspect(const Renderer& renderer, RhiCommandBuffer* cmd) -> void;
    auto releaseTexturePreview(uint64_t frame) -> void;
    auto recordTextureDump(const Renderer& renderer, RhiCommandBuffer* cmd) -> void;
    auto recordScreenshot(RhiCommandBuffer* cmd, const RenderFrame& frame) -> void;

    // Main thread.
    bool screenshotsShowUi = false;
    bool validationEnabled = false;
    RenderToolsFrame next;
    RenderToolsResults taken;
    TextureInspectRequest textureInspect;
    std::vector<TextureDumpRequest> pendingDumps;
    std::vector<std::string> pendingScreenshots;

    // Render thread.
    std::optional<RenderExtensionContext> context;
    RenderToolsFrame frameRequest;         // this frame's, from the snapshot
    bool frameDebugPending = false;        // asked for, waiting for a frame that records
    std::vector<std::string> screenshots;  // waiting for a frame that presents
    std::vector<TextureDumpRequest> dumps; // waiting for a frame that records
    bool frameGraphDebugOn = false;
    uint64_t frameGraphDebugFrames = 0;
    uint64_t countersFrame = 0; // the last GPU frame counters were made for
    std::vector<RhiPipelineStatsZone> pipelineStatsScratch;
    FrameGraphPreviews previews;
    // Texture inspector: one level of one material texture blitted into a preview the UI draws; replaced when the request changes.
    struct TexturePreview {
        RhiTexture* texture = nullptr;
        uint64_t id = 0; // PreviewTextures id
        uint32_t width = 0;
        uint32_t height = 0;
        bool everCaptured = false;
    };
    TexturePreview texturePreview;
    RenderDebugTextureInspect textureInspectResult;
    // This frame's readbacks into host memory, written after its fence: the screenshot and the texture dumps.
    struct Readback {
        RhiBuffer* buffer = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string path;
        uint32_t texture = 0; // dumps
        uint32_t level = 0;
        bool screenshot = false;
    };
    std::vector<Readback> readbacks;
    std::optional<FrameDebugCapture> frameDebugCapture; // this frame's, published at endFrame

    // Render thread to main thread: latest-only snapshots, every screenshot and counters frame.
    std::mutex resultsMutex;
    RenderToolsResults published;
};
