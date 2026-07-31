#include "rhitesthelpers.h"

#include "rhicommandbuffer.h"
#include "rhidevice.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstring>
#include <format>
#include <print>

auto renderToPixels(RhiDevice* device, RhiExtent2D extent, const RenderToPixelsOptions& options) -> PixelImage {
    auto byteSize = (uint64_t) extent.width * extent.height * 4;

    auto* color = device->createTexture({
        .width = extent.width,
        .height = extent.height,
        .format = RhiFormat::R8G8B8A8_UNORM,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::TransferSrc,
    });
    RhiTexture* depth = nullptr;
    if (options.withDepth) {
        depth = device->createTexture({
            .width = extent.width,
            .height = extent.height,
            .format = RhiFormat::D32_SFLOAT,
            .usage = RhiTextureUsage::DepthAttachment,
        });
    }
    auto* readback = device->createBuffer({.size = byteSize, .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::CpuToGpu});
    auto* cmd = device->createCommandBuffer();
    auto* fence = device->createFence(false);

    cmd->begin();

    std::vector<RhiBarrierDesc> toRenderable;
    toRenderable.push_back({.texture = color, .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::ColorAttachment});
    if (depth != nullptr) {
        toRenderable.push_back({.texture = depth, .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::DepthStencilAttachment});
    }
    cmd->pipelineBarrier(toRenderable);

    std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {
        RhiRenderingAttachmentInfo{
            .texture = color,
            .layout = RhiImageLayout::ColorAttachment,
            .clear = true,
            .clearColor = options.clearColor,
        },
    };
    RhiRenderingAttachmentInfo depthAttachment = {
        .texture = depth,
        .layout = RhiImageLayout::DepthStencilAttachment,
        .clear = true,
        .clearDepth = 1.0f,
    };
    RhiRenderingInfo renderingInfo = {
        .extent = extent,
        .colorAttachments = colorAttachments,
        .depthAttachment = depth != nullptr ? &depthAttachment : nullptr,
    };
    cmd->beginRendering(renderingInfo);
    cmd->setViewport(extent);
    cmd->setScissor(extent);
    if (options.record) {
        options.record(*cmd);
    }
    cmd->endRendering();

    std::array<RhiBarrierDesc, 1> toReadable = {
        RhiBarrierDesc{.texture = color, .oldLayout = RhiImageLayout::ColorAttachment, .newLayout = RhiImageLayout::TransferSrc},
    };
    cmd->pipelineBarrier(toReadable);
    cmd->copyTextureToBuffer(color, readback, extent);

    cmd->end();
    device->submitCommandBuffer(cmd, {.waitSemaphore = nullptr, .signalSemaphore = nullptr, .fence = fence});
    device->waitForFence(fence);

    PixelImage image = {.extent = extent, .pixels = std::vector<uint8_t>(byteSize)};
    void* mapped = device->mapBuffer(readback);
    memcpy(image.pixels.data(), mapped, byteSize);
    device->unmapBuffer(readback);

    device->destroyFence(fence);
    device->destroyCommandBuffer(cmd);
    device->destroyBuffer(readback);
    if (depth != nullptr) {
        device->destroyTexture(depth);
    }
    device->destroyTexture(color);

    return image;
}

auto expectPixel(ngentest::TestContext& ctx, const PixelImage& image, uint32_t x, uint32_t y, std::array<uint8_t, 4> expected) -> void {
    auto actual = image.at(x, y);
    if (actual == expected) {
        return;
    }

    // Dump the whole image once per test, on the first failing expectation.
    if (!ctx.dumpDir().empty() && !ctx.failed()) {
        auto path = std::format("{}/{}.png", ctx.dumpDir(), ctx.name());
        if (stbi_write_png(path.c_str(), (int) image.extent.width, (int) image.extent.height, 4, image.pixels.data(), (int) image.extent.width * 4) != 0) {
            std::println(stderr, "Dumped failing image to {}", path);
        }
    }

    ctx.expect(
        false,
        std::format(
            "pixel ({}, {}) is ({}, {}, {}, {}), expected ({}, {}, {}, {})",
            x,
            y,
            actual[0],
            actual[1],
            actual[2],
            actual[3],
            expected[0],
            expected[1],
            expected[2],
            expected[3]));
}

auto createVertexBuffer(RhiDevice* device, const float* data, size_t floatCount) -> RhiBuffer* {
    auto byteSize = floatCount * sizeof(float);
    auto* buffer = device->createBuffer({.size = byteSize, .usage = RhiBufferUsage::Vertex, .memory = RhiMemoryUsage::CpuToGpu});
    if (buffer == nullptr) {
        return nullptr;
    }
    void* mapped = device->mapBuffer(buffer);
    memcpy(mapped, data, byteSize);
    device->unmapBuffer(buffer);
    return buffer;
}
