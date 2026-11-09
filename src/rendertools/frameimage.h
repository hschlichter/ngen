#pragma once

#include "renderextension.h"
#include "rhitypes.h"

#include <cstdint>
#include <vector>

class RhiBuffer;
class RhiCommandBuffer;
class RhiDevice;
class RhiTexture;

// Tracy's frame images: while a Tracy server is connected, the presented image (UI included) scaled down to 320 pixels wide and read back in
// its frame's command buffer, then handed to Tracy at the slot's next fence, under the main-thread frame that asked for it. Nothing waits: the
// readback is read once the renderer has waited for that slot anyway. Each slot keeps its own target and buffer, made on first use and remade
// when the swapchain's size changes.
class FrameImages {
public:
    auto init(RhiDevice* device, uint32_t frameSlots) -> void;
    // Render thread, afterExecute: the backbuffer is in PresentSrc and goes back there. `profileFrame` is the main thread's frame (frameMarks).
    auto record(RhiCommandBuffer* cmd, const RenderFrame& frame, RhiFormat backbufferFormat, uint64_t profileFrame) -> void;
    // Render thread, afterFence: the slot's image, if it recorded one, goes to Tracy.
    auto afterFence(uint32_t slot) -> void;
    auto destroy() -> void;

    static constexpr uint32_t width = 320;

private:
    struct Slot {
        RhiTexture* target = nullptr;
        RhiBuffer* readback = nullptr;
        uint32_t height = 0;
        RhiFormat format = RhiFormat::Undefined;
        bool pending = false;
        uint64_t profileFrame = 0;
    };
    auto release(Slot& slot) -> void;

    RhiDevice* device = nullptr;
    std::vector<Slot> slots;
};
