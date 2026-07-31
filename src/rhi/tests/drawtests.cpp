#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhitestenv.h"
#include "rhitesthelpers.h"
#include "testharness.h"

#include <array>
#include <cstring>

namespace {

constexpr std::array<uint8_t, 4> black = {0, 0, 0, 255};
constexpr std::array<uint8_t, 4> red = {255, 0, 0, 255};
constexpr std::array<uint8_t, 4> green = {0, 255, 0, 255};
constexpr std::array<uint8_t, 4> blue = {0, 0, 255, 255};
constexpr std::array<uint8_t, 4> white = {255, 255, 255, 255};

struct TestPipeline {
    RhiShaderModule* vert = nullptr;
    RhiShaderModule* frag = nullptr;
    RhiPipeline* pipeline = nullptr;
};

auto destroyTestPipeline(RhiDevice* device, TestPipeline& p) -> void {
    if (p.pipeline != nullptr) {
        device->destroyPipeline(p.pipeline);
    }
    if (p.frag != nullptr) {
        device->destroyShaderModule(p.frag);
    }
    if (p.vert != nullptr) {
        device->destroyShaderModule(p.vert);
    }
    p = {};
}

// Solid-color pipeline: vec3 NDC positions, fragment color from a push constant.
auto createFlatPipeline(RhiDevice* device, RhiExtent2D extent, bool depthTest) -> TestPipeline {
    TestPipeline p;
    p.vert = device->createShaderModule(rhiTestShaderPath("testflat.vert.spv").c_str());
    p.frag = device->createShaderModule(rhiTestShaderPath("testflat.frag.spv").c_str());
    if (p.vert == nullptr || p.frag == nullptr) {
        return p;
    }
    std::array<RhiFormat, 1> colorFormats = {RhiFormat::R8G8B8A8_UNORM};
    std::array<RhiVertexAttribute, 1> attributes = {
        RhiVertexAttribute{.location = 0, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = 0},
    };
    p.pipeline = device->createGraphicsPipeline({
        .vertexShader = p.vert,
        .fragmentShader = p.frag,
        .descriptorSetLayout = nullptr,
        .colorFormats = colorFormats,
        .depthFormat = depthTest ? RhiFormat::D32_SFLOAT : RhiFormat::Undefined,
        .vertexStride = 3 * sizeof(float),
        .vertexAttributes = attributes,
        .pushConstant = {.stage = RhiShaderStage::Fragment, .offset = 0, .size = 16},
        .viewportExtent = extent,
        .depthTestEnable = depthTest,
        .depthWriteEnable = depthTest,
        .backfaceCulling = false,
    });
    return p;
}

// Textured pipeline: vec3 NDC positions + vec2 UVs, one combined image sampler.
auto createTexturedPipeline(RhiDevice* device, RhiExtent2D extent, RhiDescriptorSetLayout* layout) -> TestPipeline {
    TestPipeline p;
    p.vert = device->createShaderModule(rhiTestShaderPath("testtextured.vert.spv").c_str());
    p.frag = device->createShaderModule(rhiTestShaderPath("testtextured.frag.spv").c_str());
    if (p.vert == nullptr || p.frag == nullptr) {
        return p;
    }
    std::array<RhiFormat, 1> colorFormats = {RhiFormat::R8G8B8A8_UNORM};
    std::array<RhiVertexAttribute, 2> attributes = {
        RhiVertexAttribute{.location = 0, .binding = 0, .format = RhiFormat::R32G32B32_SFLOAT, .offset = 0},
        RhiVertexAttribute{.location = 1, .binding = 0, .format = RhiFormat::R32G32_SFLOAT, .offset = 3 * sizeof(float)},
    };
    p.pipeline = device->createGraphicsPipeline({
        .vertexShader = p.vert,
        .fragmentShader = p.frag,
        .descriptorSetLayout = layout,
        .colorFormats = colorFormats,
        .depthFormat = RhiFormat::Undefined,
        .vertexStride = 5 * sizeof(float),
        .vertexAttributes = attributes,
        .pushConstant = {.stage = RhiShaderStage::Fragment, .offset = 0, .size = 0},
        .viewportExtent = extent,
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .backfaceCulling = false,
    });
    return p;
}

// A large triangle covering the framebuffer center; corners stay uncovered.
auto triangleVerts(float z) -> std::array<float, 9> {
    return {-0.9f, -0.9f, z, 0.9f, -0.9f, z, 0.0f, 0.9f, z};
}

auto pushColor(RhiCommandBuffer& cmd, RhiPipeline* pipeline, std::array<float, 4> color) -> void {
    cmd.pushConstants(pipeline, RhiShaderStage::Fragment, 0, sizeof(color), color.data());
}

} // namespace

NGEN_TEST("clear-color") {
    auto* device = rhiTestDevice();
    constexpr RhiExtent2D extent = {.width = 64, .height = 64};

    auto image = renderToPixels(device, extent, {.clearColor = {1.0f, 0.0f, 0.0f, 1.0f}});

    size_t mismatches = 0;
    for (uint32_t y = 0; y < extent.height; y++) {
        for (uint32_t x = 0; x < extent.width; x++) {
            if (image.at(x, y) != red) {
                mismatches++;
            }
        }
    }
    ctx.expect(mismatches == 0, "not every pixel matches the clear color");
    expectPixel(ctx, image, 0, 0, red);
}

NGEN_TEST("triangle-coverage") {
    auto* device = rhiTestDevice();
    constexpr RhiExtent2D extent = {.width = 64, .height = 64};

    auto pipeline = createFlatPipeline(device, extent, false);
    ctx.expect(pipeline.pipeline != nullptr, "failed to create flat pipeline");
    if (pipeline.pipeline == nullptr) {
        destroyTestPipeline(device, pipeline);
        return;
    }
    auto verts = triangleVerts(0.5f);
    auto* vb = createVertexBuffer(device, verts.data(), verts.size());

    auto image = renderToPixels(
        device,
        extent,
        {
            .record =
                [&](RhiCommandBuffer& cmd) -> void {
                cmd.bindPipeline(pipeline.pipeline);
                cmd.bindVertexBuffer(vb);
                pushColor(cmd, pipeline.pipeline, {0.0f, 1.0f, 0.0f, 1.0f});
                cmd.draw(3, 1, 0, 0);
            },
        });

    expectPixel(ctx, image, 32, 32, green);
    expectPixel(ctx, image, 1, 1, black);
    expectPixel(ctx, image, 62, 62, black);

    device->destroyBuffer(vb);
    destroyTestPipeline(device, pipeline);
}

NGEN_TEST("push-constant-color") {
    auto* device = rhiTestDevice();
    constexpr RhiExtent2D extent = {.width = 64, .height = 64};

    auto pipeline = createFlatPipeline(device, extent, false);
    ctx.expect(pipeline.pipeline != nullptr, "failed to create flat pipeline");
    if (pipeline.pipeline == nullptr) {
        destroyTestPipeline(device, pipeline);
        return;
    }
    auto verts = triangleVerts(0.5f);
    auto* vb = createVertexBuffer(device, verts.data(), verts.size());

    auto drawWith = [&](std::array<float, 4> color) -> PixelImage {
        return renderToPixels(
            device,
            extent,
            {
                .record =
                    [&](RhiCommandBuffer& cmd) -> void {
                    cmd.bindPipeline(pipeline.pipeline);
                    cmd.bindVertexBuffer(vb);
                    pushColor(cmd, pipeline.pipeline, color);
                    cmd.draw(3, 1, 0, 0);
                },
            });
    };

    auto redImage = drawWith({1.0f, 0.0f, 0.0f, 1.0f});
    expectPixel(ctx, redImage, 32, 32, red);

    auto blueImage = drawWith({0.0f, 0.0f, 1.0f, 1.0f});
    expectPixel(ctx, blueImage, 32, 32, blue);

    device->destroyBuffer(vb);
    destroyTestPipeline(device, pipeline);
}

NGEN_TEST("depth-test") {
    auto* device = rhiTestDevice();
    constexpr RhiExtent2D extent = {.width = 64, .height = 64};

    auto nearVerts = triangleVerts(0.25f);
    auto farVerts = triangleVerts(0.75f);
    auto* nearVb = createVertexBuffer(device, nearVerts.data(), nearVerts.size());
    auto* farVb = createVertexBuffer(device, farVerts.data(), farVerts.size());

    // Near triangle (green) drawn first, far triangle (red) second: with depth
    // testing the far one is rejected regardless of draw order.
    auto drawBoth = [&](TestPipeline& pipeline, bool withDepth) -> PixelImage {
        return renderToPixels(
            device,
            extent,
            {
                .withDepth = withDepth,
                .record =
                    [&](RhiCommandBuffer& cmd) -> void {
                    cmd.bindPipeline(pipeline.pipeline);
                    cmd.bindVertexBuffer(nearVb);
                    pushColor(cmd, pipeline.pipeline, {0.0f, 1.0f, 0.0f, 1.0f});
                    cmd.draw(3, 1, 0, 0);
                    cmd.bindVertexBuffer(farVb);
                    pushColor(cmd, pipeline.pipeline, {1.0f, 0.0f, 0.0f, 1.0f});
                    cmd.draw(3, 1, 0, 0);
                },
            });
    };

    auto depthOn = createFlatPipeline(device, extent, true);
    ctx.expect(depthOn.pipeline != nullptr, "failed to create depth-testing pipeline");
    if (depthOn.pipeline != nullptr) {
        auto image = drawBoth(depthOn, true);
        expectPixel(ctx, image, 32, 32, green);
    }
    destroyTestPipeline(device, depthOn);

    auto depthOff = createFlatPipeline(device, extent, false);
    ctx.expect(depthOff.pipeline != nullptr, "failed to create depth-ignoring pipeline");
    if (depthOff.pipeline != nullptr) {
        auto image = drawBoth(depthOff, false);
        expectPixel(ctx, image, 32, 32, red);
    }
    destroyTestPipeline(device, depthOff);

    device->destroyBuffer(farVb);
    device->destroyBuffer(nearVb);
}

NGEN_TEST("descriptor-sampled-texture") {
    auto* device = rhiTestDevice();
    // 2x2 target with a full-screen quad and UVs 0..1: every pixel center maps
    // exactly onto a texel center, so linear filtering returns exact texels.
    constexpr RhiExtent2D extent = {.width = 2, .height = 2};

    constexpr std::array<uint8_t, 16> checker = {
        255,
        0,
        0,
        255, // (0,0) red
        0,
        255,
        0,
        255, // (1,0) green
        0,
        0,
        255,
        255, // (0,1) blue
        255,
        255,
        255,
        255, // (1,1) white
    };
    auto* texture = device->createTexture({
        .width = 2,
        .height = 2,
        .format = RhiFormat::R8G8B8A8_UNORM,
        .usage = RhiTextureUsage::Sampled | RhiTextureUsage::TransferDst,
        .initialData = checker.data(),
        .initialDataSize = checker.size(),
    });
    auto* sampler = device->createSampler({});

    std::array<RhiDescriptorBinding, 1> bindings = {
        RhiDescriptorBinding{.binding = 0, .type = RhiDescriptorType::CombinedImageSampler, .stage = RhiShaderStage::Fragment},
    };
    auto* layout = device->createDescriptorSetLayout(bindings);
    auto* pool = device->createDescriptorPool(1, bindings);
    auto sets = device->allocateDescriptorSets(pool, layout, 1);
    std::array<RhiDescriptorWrite, 1> writes = {
        RhiDescriptorWrite{
            .binding = 0,
            .type = RhiDescriptorType::CombinedImageSampler,
            .buffer = nullptr,
            .bufferRange = 0,
            .texture = texture,
            .sampler = sampler,
        },
    };
    device->updateDescriptorSet(sets[0], writes);

    auto pipeline = createTexturedPipeline(device, extent, layout);
    ctx.expect(pipeline.pipeline != nullptr, "failed to create textured pipeline");

    if (pipeline.pipeline != nullptr) {
        // Full-screen quad, position xyz + uv, two triangles.
        constexpr std::array<float, 30> quad = {
            -1.0f,
            -1.0f,
            0.5f,
            0.0f,
            0.0f, //
            1.0f,
            -1.0f,
            0.5f,
            1.0f,
            0.0f, //
            1.0f,
            1.0f,
            0.5f,
            1.0f,
            1.0f, //
            -1.0f,
            -1.0f,
            0.5f,
            0.0f,
            0.0f, //
            1.0f,
            1.0f,
            0.5f,
            1.0f,
            1.0f, //
            -1.0f,
            1.0f,
            0.5f,
            0.0f,
            1.0f, //
        };
        auto* vb = createVertexBuffer(device, quad.data(), quad.size());

        auto image = renderToPixels(
            device,
            extent,
            {
                .record =
                    [&](RhiCommandBuffer& cmd) -> void {
                    cmd.bindPipeline(pipeline.pipeline);
                    cmd.bindDescriptorSet(pipeline.pipeline, sets[0]);
                    cmd.bindVertexBuffer(vb);
                    cmd.draw(6, 1, 0, 0);
                },
            });

        expectPixel(ctx, image, 0, 0, red);
        expectPixel(ctx, image, 1, 0, green);
        expectPixel(ctx, image, 0, 1, blue);
        expectPixel(ctx, image, 1, 1, white);

        device->destroyBuffer(vb);
    }

    destroyTestPipeline(device, pipeline);
    device->destroyDescriptorPool(pool);
    device->destroyDescriptorSetLayout(layout);
    device->destroySampler(sampler);
    device->destroyTexture(texture);
    for (auto* set : sets) {
        delete set; // sets are freed with the pool; only the wrappers remain
    }
}

NGEN_TEST("blit-roundtrip") {
    auto* device = rhiTestDevice();
    constexpr RhiExtent2D extent = {.width = 64, .height = 64};
    constexpr uint64_t byteSize = (uint64_t) extent.width * extent.height * 4;

    auto pipeline = createFlatPipeline(device, extent, false);
    ctx.expect(pipeline.pipeline != nullptr, "failed to create flat pipeline");
    if (pipeline.pipeline == nullptr) {
        destroyTestPipeline(device, pipeline);
        return;
    }
    auto verts = triangleVerts(0.5f);
    auto* vb = createVertexBuffer(device, verts.data(), verts.size());

    auto* source = device->createTexture({
        .width = extent.width,
        .height = extent.height,
        .format = RhiFormat::R8G8B8A8_UNORM,
        .usage = RhiTextureUsage::ColorAttachment | RhiTextureUsage::TransferSrc,
    });
    auto* target = device->createTexture({
        .width = extent.width,
        .height = extent.height,
        .format = RhiFormat::R8G8B8A8_UNORM,
        .usage = RhiTextureUsage::TransferDst | RhiTextureUsage::TransferSrc,
    });
    auto* readSource = device->createBuffer({.size = byteSize, .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::CpuToGpu});
    auto* readTarget = device->createBuffer({.size = byteSize, .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::CpuToGpu});
    auto* cmd = device->createCommandBuffer();
    auto* fence = device->createFence(false);

    cmd->begin();
    std::array<RhiBarrierDesc, 1> toRenderable = {
        RhiBarrierDesc{.texture = source, .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::ColorAttachment},
    };
    cmd->pipelineBarrier(toRenderable);

    std::array<RhiRenderingAttachmentInfo, 1> colorAttachments = {
        RhiRenderingAttachmentInfo{
            .texture = source,
            .layout = RhiImageLayout::ColorAttachment,
            .clear = true,
            .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
        },
    };
    cmd->beginRendering({.extent = extent, .colorAttachments = colorAttachments});
    cmd->setViewport(extent);
    cmd->setScissor(extent);
    cmd->bindPipeline(pipeline.pipeline);
    cmd->bindVertexBuffer(vb);
    pushColor(*cmd, pipeline.pipeline, {0.0f, 1.0f, 0.0f, 1.0f});
    cmd->draw(3, 1, 0, 0);
    cmd->endRendering();

    std::array<RhiBarrierDesc, 2> toBlittable = {
        RhiBarrierDesc{.texture = source, .oldLayout = RhiImageLayout::ColorAttachment, .newLayout = RhiImageLayout::TransferSrc},
        RhiBarrierDesc{.texture = target, .oldLayout = RhiImageLayout::Undefined, .newLayout = RhiImageLayout::TransferDst},
    };
    cmd->pipelineBarrier(toBlittable);
    cmd->blitTexture(source, target, extent, extent);

    std::array<RhiBarrierDesc, 1> toReadable = {
        RhiBarrierDesc{.texture = target, .oldLayout = RhiImageLayout::TransferDst, .newLayout = RhiImageLayout::TransferSrc},
    };
    cmd->pipelineBarrier(toReadable);
    cmd->copyTextureToBuffer(source, readSource, extent);
    cmd->copyTextureToBuffer(target, readTarget, extent);
    cmd->end();

    device->submitCommandBuffer(cmd, {.waitSemaphore = nullptr, .signalSemaphore = nullptr, .fence = fence});
    device->waitForFence(fence);

    void* sourcePixels = device->mapBuffer(readSource);
    void* targetPixels = device->mapBuffer(readTarget);
    // Sanity: the render actually drew — center of the source is the triangle color.
    auto centerOffset = (((size_t) 32 * extent.width) + 32) * 4;
    auto* sourceBytes = (const uint8_t*) sourcePixels;
    ctx.expect(sourceBytes[centerOffset + 1] == 255, "source center is not the drawn triangle color");
    ctx.expect(memcmp(sourcePixels, targetPixels, byteSize) == 0, "blit destination does not match the source");
    device->unmapBuffer(readTarget);
    device->unmapBuffer(readSource);

    device->destroyFence(fence);
    device->destroyCommandBuffer(cmd);
    device->destroyBuffer(readTarget);
    device->destroyBuffer(readSource);
    device->destroyTexture(target);
    device->destroyTexture(source);
    device->destroyBuffer(vb);
    destroyTestPipeline(device, pipeline);
}
