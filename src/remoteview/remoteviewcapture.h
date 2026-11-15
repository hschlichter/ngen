#pragma once

#include "renderextension.h"
#include "rhitypes.h"

#include <cstdint>
#include <vector>

class RemoteView;
class RhiBuffer;
class RhiDevice;
class RhiTexture;

// The remote view's data for one frame, in RenderSnapshot::extensions: whether this frame is captured, and at what height.
struct RemoteViewRequest : RenderExtensionData {
    bool capture = false;
    uint32_t height = 0;
};

// The remote view's part of the renderer's frame (src/remoteview/README.md): the presented image, UI included, scaled to the video's
// size on the GPU and read back in its frame's command buffer, then handed to the remote view once the GPU has finished that frame. Nothing
// waits: each frame the extension asks which captured frames are done (Renderer::frameContextDone), and a frame still not done when its
// context comes round is read then, when the renderer has waited for it anyway. Added after the UI's extension. A frame is captured
// only when the main thread asked (a watcher is connected and a frame is due); with no watcher there is no blit, and the targets and buffers are
// given back five seconds after the last capture.
class RemoteViewCapture : public RenderExtension {
public:
    explicit RemoteViewCapture(RemoteView& remoteView);

    // Main thread, with the other extensions' fillSnapshot.
    auto fillSnapshot(RenderSnapshot& snapshot) -> void;

    auto init(const RenderExtensionContext& context) -> bool override;
    auto beginFrame(Renderer& renderer, RenderSnapshot& snapshot, RenderFrame& frame) -> void override;
    auto afterFence(Renderer& renderer, uint32_t frameContext) -> void override;
    auto afterExecute(Renderer& renderer, RhiCommandBuffer* cmd, const RenderFrame& frame) -> void override;
    auto destroy() -> void override;

private:
    // One frame context's target and readback buffer, made on first use and remade when the video's size changes.
    struct Slot {
        RhiTexture* target = nullptr;
        RhiBuffer* readback = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        RhiFormat format = RhiFormat::Undefined;
        bool pending = false;
        uint64_t captureNs = 0;
    };
    auto release(Slot& slot) -> void;
    // Copies a finished capture out of its buffer and hands it to the remote view.
    auto read(Slot& slot) -> void;

    RemoteView& remoteView;
    RhiDevice* device = nullptr;
    std::vector<Slot> slots;
    uint64_t lastCaptureNs = 0;
    RemoteViewRequest request;
};
