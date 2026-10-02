#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// A full mip chain for an RGBA8 texture, level 0 first, down to 1x1.
// Built on the CPU with a 2x2 box filter.
struct MipLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // RGBA8, tightly packed
};

// Number of levels in a full chain for the given size.
auto mipLevelCount(uint32_t width, uint32_t height) -> uint32_t;

// Builds every level below level 0. With srgb set the colour channels are averaged in
// linear space, which is what an sRGB-encoded texel requires; alpha is averaged as is.
// Odd sizes round down: the last row or column does not feed the next level.
auto buildMipChain(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) -> std::vector<MipLevel>;

// Concatenates level 0 and the chain into one tightly packed byte block, in level order,
// the layout GpuUploader::uploadTexture expects when the texture desc has mipLevels > 1.
auto packMipChain(std::span<const uint8_t> level0, std::span<const MipLevel> chain) -> std::vector<std::byte>;
