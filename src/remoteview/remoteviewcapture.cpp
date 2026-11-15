#include "remoteviewcapture.h"

#include "profile.h"
#include "profilegpu.h"
#include "remoteview.h"
#include "renderer.h"
#include "rendersnapshot.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"

#include <array>
#include <cstring>
#include <memory>

namespace {

constexpr uint64_t releaseAfterNs = 5'000'000'000ull;

} // namespace

RemoteViewCapture::RemoteViewCapture(RemoteView& view) : remoteView(view) {
}

auto RemoteViewCapture::fillSnapshot(RenderSnapshot& snapshot) -> void {
    auto due = remoteView.takeFrameDue();
    if (!due) {
        return;
    }
    auto data = std::make_unique<RemoteViewRequest>();
    data->capture = true;
    data->height = *due;
    snapshot.setExtension(index(), std::move(data));
}

auto RemoteViewCapture::init(const RenderExtensionContext& context) -> bool {
    device = &context.device;
    slots.resize(context.frameContexts);
    return true;
}

auto RemoteViewCapture::release(Slot& slot) -> void {
    if (slot.target != nullptr) {
        device->destroyTexture(slot.target);
    }
    if (slot.readback != nullptr) {
        device->destroyBuffer(slot.readback);
    }
    slot = {};
}

auto RemoteViewCapture::beginFrame(Renderer& /*renderer*/, RenderSnapshot& snapshot, RenderFrame& /*frame*/) -> void {
    // A request a frame that doesn't record carried is dropped: the next due frame asks again.
    request = {};
    if (auto* data = snapshot.extension<RemoteViewRequest>(index())) {
        request.capture = data->capture;
        request.height = data->height;
    }
}

auto RemoteViewCapture::afterExecute(Renderer& renderer, RhiCommandBuffer* cmd, const RenderFrame& frame) -> void {
    if (!request.capture || frame.frameContext >= slots.size() || frame.backbuffer == nullptr || frame.extent.width == 0 || frame.extent.height == 0) {
        return;
    }
    // The video's height, and the width the view's shape gives it; the encoder takes even sizes only. Never larger than the view.
    uint32_t height = std::min(request.height, frame.extent.height) & ~1u;
    uint32_t width = (uint32_t) ((uint64_t) height * frame.extent.width / frame.extent.height) & ~1u;
    if (width == 0 || height == 0) {
        return;
    }
    // The blit decodes an sRGB backbuffer to linear and the target encodes it again, so the bytes read back are what was shown; it also
    // turns BGRA into the RGBA the conversion takes.
    auto backbufferFormat = renderer.state().swapchainFormat;
    bool srgb = backbufferFormat == RhiFormat::B8G8R8A8_SRGB || backbufferFormat == RhiFormat::R8G8B8A8_SRGB;
    auto format = srgb ? RhiFormat::R8G8B8A8_SRGB : RhiFormat::R8G8B8A8_UNORM;
    // This context's fence has passed (afterFence came first), so its target and buffer are free to remake.
    auto& slot = slots[frame.frameContext];
    slot.pending = false;
    if (slot.target == nullptr || slot.width != width || slot.height != height || slot.format != format) {
        release(slot);
        RhiTextureDesc textureDesc = {
            .width = width,
            .height = height,
            .format = format,
            // Sampled although nothing samples it: every RHI texture has a view, and a view needs a usage it can serve.
            .usage = RhiTextureUsage::TransferDst | RhiTextureUsage::TransferSrc | RhiTextureUsage::Sampled,
            .debugName = "remoteview.capture",
        };
        slot.target = device->createTexture(textureDesc);
        RhiBufferDesc bufferDesc = {
            .size = (uint64_t) width * height * 4,
            .usage = RhiBufferUsage::TransferDst,
            .memory = RhiMemoryUsage::GpuToCpu,
            .debugName = "remoteview.capture.readback",
        };
        slot.readback = device->createBuffer(bufferDesc);
        slot.width = width;
        slot.height = height;
        slot.format = format;
        if (slot.target == nullptr || slot.readback == nullptr) {
            release(slot);
            return;
        }
    }
    PROFILE_GPU_ZONE_DETAIL(cmd, "RemoteViewCapture", Upload);
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
    slot.captureNs = profile::now();
    lastCaptureNs = slot.captureNs;
}

auto RemoteViewCapture::afterFence(Renderer& /*renderer*/, uint32_t frameContext) -> void {
    if (frameContext >= slots.size()) {
        return;
    }
    auto& slot = slots[frameContext];
    if (!slot.pending) {
        // Nothing has been captured for a while (the watcher left, or the remote view was turned off): this context's fence has passed,
        // so its target and buffer can go.
        if (slot.target != nullptr && profile::now() - lastCaptureNs > releaseAfterNs) {
            release(slot);
        }
        return;
    }
    slot.pending = false;
    PROFILE_ZONE_DETAIL("RemoteViewCopy", Upload);
    uint64_t start = profile::now();
    auto* pixels = static_cast<const uint8_t*>(device->mapBuffer(slot.readback));
    if (pixels == nullptr) {
        return;
    }
    VideoImage image;
    image.width = slot.width;
    image.height = slot.height;
    image.captureNs = slot.captureNs;
    image.rgba.resize((size_t) slot.width * slot.height * 4);
    std::memcpy(image.rgba.data(), pixels, image.rgba.size());
    device->unmapBuffer(slot.readback);
    remoteView.submit(std::move(image), (double) (profile::now() - start) / 1e6);
}

auto RemoteViewCapture::destroy() -> void {
    for (auto& slot : slots) {
        release(slot);
    }
    slots.clear();
}
