#pragma once

#include "rhitypes.h"
#include "testharness.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class RhiDevice;
class RhiCommandBuffer;

// A deterministic non-trivial byte pattern for upload/readback comparisons.
inline auto makeBytePattern(size_t size) -> std::vector<uint8_t> {
    std::vector<uint8_t> pattern(size);
    for (size_t i = 0; i < size; i++) {
        pattern[i] = (uint8_t) (((i * 7) + 13) & 0xFF);
    }
    return pattern;
}

// RGBA8 readback of a render target, tightly packed, row 0 at the top.
struct PixelImage {
    RhiExtent2D extent = {.width = 0, .height = 0};
    std::vector<uint8_t> pixels;

    [[nodiscard]] auto at(uint32_t x, uint32_t y) const -> std::array<uint8_t, 4> {
        auto offset = ((size_t) y * extent.width + x) * 4;
        return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
    }
};

struct RenderToPixelsOptions {
    std::array<float, 4> clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    bool withDepth = false;
    // Draw commands recorded between beginRendering and endRendering. Viewport
    // and scissor are already set to the full extent.
    std::function<void(RhiCommandBuffer&)> record;
};

// Renders one pass into a fresh R8G8B8A8_UNORM target (UNORM so expected byte
// values are exact) and reads it back: barrier to ColorAttachment, clear,
// record(), barrier to TransferSrc, copyTextureToBuffer, fence-wait, map.
auto renderToPixels(RhiDevice* device, RhiExtent2D extent, const RenderToPixelsOptions& options) -> PixelImage;

// Exact-match pixel expectation. On the first mismatch in a test the whole
// image is dumped as <dump-dir>/<test-name>.png when --dump-dir is set.
auto expectPixel(ngentest::TestContext& ctx, const PixelImage& image, uint32_t x, uint32_t y, std::array<uint8_t, 4> expected) -> void;

// Host-visible vertex buffer filled from raw float data.
auto createVertexBuffer(RhiDevice* device, const float* data, size_t floatCount) -> RhiBuffer*;
