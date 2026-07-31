#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhitestenv.h"
#include "rhitesthelpers.h"
#include "testharness.h"

#include <array>
#include <cstring>

NGEN_TEST("texture-upload-readback") {
    auto* device = rhiTestDevice();
    constexpr uint32_t width = 4;
    constexpr uint32_t height = 4;
    constexpr uint64_t byteSize = width * height * 4;
    auto pattern = makeBytePattern(byteSize);

    auto* texture = device->createTexture({
        .width = width,
        .height = height,
        .format = RhiFormat::R8G8B8A8_UNORM,
        .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst | RhiTextureUsage::TransferSrc,
        .initialData = pattern.data(),
        .initialDataSize = byteSize,
    });
    ctx.expect(texture != nullptr, "createTexture returned null");
    if (texture == nullptr) {
        return;
    }

    auto* readback = device->createBuffer({.size = byteSize, .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::CpuToGpu});
    auto* cmd = device->createCommandBuffer();
    auto* fence = device->createFence(false);

    // createTexture with initialData leaves the image in ShaderReadOnly.
    cmd->begin();
    std::array<RhiBarrierDesc, 1> barriers = {
        RhiBarrierDesc{.texture = texture, .oldLayout = RhiImageLayout::ShaderReadOnly, .newLayout = RhiImageLayout::TransferSrc},
    };
    cmd->pipelineBarrier(barriers);
    cmd->copyTextureToBuffer(texture, readback, {.width = width, .height = height});
    cmd->end();
    device->submitCommandBuffer(cmd, {.waitSemaphore = nullptr, .signalSemaphore = nullptr, .fence = fence});
    device->waitForFence(fence);

    void* mapped = device->mapBuffer(readback);
    ctx.expect(memcmp(mapped, pattern.data(), byteSize) == 0, "texture readback does not match the uploaded pattern");
    device->unmapBuffer(readback);

    device->destroyFence(fence);
    device->destroyCommandBuffer(cmd);
    device->destroyBuffer(readback);
    device->destroyTexture(texture);
}
