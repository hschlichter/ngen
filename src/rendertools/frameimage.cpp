#include "frameimage.h"

#include "profile.h"
#include "profilegpu.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>

auto FrameImages::init(RhiDevice* rhiDevice, uint32_t frameSlots) -> void {
    device = rhiDevice;
    slots.resize(frameSlots);
}

auto FrameImages::release(Slot& slot) -> void {
    if (slot.target != nullptr) {
        device->destroyTexture(slot.target);
    }
    if (slot.readback != nullptr) {
        device->destroyBuffer(slot.readback);
    }
    slot = {};
}

auto FrameImages::record(RhiCommandBuffer* cmd, const RenderFrame& frame, RhiFormat backbufferFormat, uint64_t profileFrame) -> void {
    if (frame.slot >= slots.size() || frame.backbuffer == nullptr || frame.extent.width == 0) {
        return;
    }
    // Tracy takes images whose sides are multiples of 4.
    uint32_t height = ((width * frame.extent.height) / frame.extent.width) & ~3u;
    if (height == 0 || height > UINT16_MAX) {
        return;
    }
    // The blit decodes an sRGB backbuffer to linear and the target encodes it again, so the bytes read back are what was shown; it also turns
    // BGRA into the RGBA Tracy wants.
    bool srgb = backbufferFormat == RhiFormat::B8G8R8A8_SRGB || backbufferFormat == RhiFormat::R8G8B8A8_SRGB;
    auto format = srgb ? RhiFormat::R8G8B8A8_SRGB : RhiFormat::R8G8B8A8_UNORM;
    // This slot's fence has passed (afterFence came first), so its target and buffer are free to remake.
    auto& slot = slots[frame.slot];
    slot.pending = false;
    if (slot.target == nullptr || slot.height != height || slot.format != format) {
        release(slot);
        RhiTextureDesc textureDesc = {
            .width = width,
            .height = height,
            .format = format,
            // Sampled although nothing samples it: every RHI texture has a view, and a view needs a usage it can serve.
            .usage = RhiTextureUsage::TransferDst | RhiTextureUsage::TransferSrc | RhiTextureUsage::Sampled,
            .debugName = "tracy.frameimage",
        };
        slot.target = device->createTexture(textureDesc);
        RhiBufferDesc bufferDesc = {
            .size = (uint64_t) width * height * 4,
            .usage = RhiBufferUsage::TransferDst,
            .memory = RhiMemoryUsage::GpuToCpu,
            .debugName = "tracy.frameimage.readback",
        };
        slot.readback = device->createBuffer(bufferDesc);
        slot.height = height;
        slot.format = format;
        if (slot.target == nullptr || slot.readback == nullptr) {
            release(slot);
            return;
        }
    }
    PROFILE_GPU_ZONE_DETAIL(cmd, "FrameImage", Upload);
    std::array<RhiTextureBarrierDesc, 2> toBlit = {{
        {.texture = frame.backbuffer, .oldState = RhiTextureState::PresentSrc, .newState = RhiTextureState::TransferSrc},
        {.texture = slot.target, .oldState = RhiTextureState::Undefined, .newState = RhiTextureState::TransferDst},
    }};
    cmd->pipelineBarrier(toBlit);
    cmd->blitTexture(frame.backbuffer, slot.target, frame.extent, {width, height});
    std::array<RhiTextureBarrierDesc, 2> toCopy = {{
        {.texture = frame.backbuffer, .oldState = RhiTextureState::TransferSrc, .newState = RhiTextureState::PresentSrc},
        {.texture = slot.target, .oldState = RhiTextureState::TransferDst, .newState = RhiTextureState::TransferSrc},
    }};
    cmd->pipelineBarrier(toCopy);
    cmd->copyTextureToBuffer(slot.target, slot.readback, {.width = width, .height = height});
    slot.pending = true;
    slot.profileFrame = profileFrame;
}

auto FrameImages::afterFence(uint32_t slotIndex) -> void {
    if (slotIndex >= slots.size() || !slots[slotIndex].pending) {
        return;
    }
    auto& slot = slots[slotIndex];
    slot.pending = false;
    PROFILE_ZONE_DETAIL("FrameImage", Upload);
    auto* pixels = static_cast<const uint8_t*>(device->mapBuffer(slot.readback));
    if (pixels == nullptr) {
        return;
    }
    profile::frameImage(pixels, (uint16_t) width, (uint16_t) slot.height, slot.profileFrame);
    device->unmapBuffer(slot.readback);
}

auto FrameImages::destroy() -> void {
    for (auto& slot : slots) {
        release(slot);
    }
    slots.clear();
}
