#include "mipchain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// Both directions through tables: pow per texel made a 4096x4096 chain take most of a
// second in a debug build. Decode is exact per byte; encode quantises linear to 12 bits,
// finer than the 8-bit output can show.
constexpr int encodeSteps = 4096;

auto srgbToLinearTable() -> const std::array<float, 256>& {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; i++) {
            float c = (float) i / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

auto linearToSrgbTable() -> const std::array<uint8_t, encodeSteps>& {
    static const std::array<uint8_t, encodeSteps> table = [] {
        std::array<uint8_t, encodeSteps> t{};
        for (int i = 0; i < encodeSteps; i++) {
            float c = (float) i / (float) (encodeSteps - 1);
            float s = c <= 0.0031308f ? c * 12.92f : (1.055f * std::pow(c, 1.0f / 2.4f)) - 0.055f;
            t[i] = (uint8_t) std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f);
        }
        return t;
    }();
    return table;
}

auto encodeSrgb(float linear) -> uint8_t {
    int i = (int) std::lround(std::clamp(linear, 0.0f, 1.0f) * (float) (encodeSteps - 1));
    return linearToSrgbTable()[i];
}

auto encodeUnorm(float value) -> uint8_t {
    return (uint8_t) std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
}

// Rows [rowBegin, rowEnd) of one level down: each output texel averages the 2x2 block
// above it. A 1-wide or 1-high source has no second column or row to pair with.
auto downsampleRows(uint32_t srcW, uint32_t srcH, std::span<const uint8_t> src, MipLevel& out, uint32_t rowBegin, uint32_t rowEnd, bool srgb) -> void {
    const auto& toLinear = srgbToLinearTable();
    uint32_t stepX = srcW > 1 ? 2 : 1;
    uint32_t stepY = srcH > 1 ? 2 : 1;
    float inv = 1.0f / (float) (stepX * stepY);
    for (uint32_t y = rowBegin; y < rowEnd; y++) {
        for (uint32_t x = 0; x < out.width; x++) {
            std::array<float, 4> sum = {0.0f, 0.0f, 0.0f, 0.0f};
            for (uint32_t dy = 0; dy < stepY; dy++) {
                for (uint32_t dx = 0; dx < stepX; dx++) {
                    size_t i = (((size_t) (y * stepY) + dy) * srcW + ((size_t) (x * stepX) + dx)) * 4;
                    if (srgb) {
                        sum[0] += toLinear[src[i + 0]];
                        sum[1] += toLinear[src[i + 1]];
                        sum[2] += toLinear[src[i + 2]];
                    } else {
                        sum[0] += (float) src[i + 0] * (1.0f / 255.0f);
                        sum[1] += (float) src[i + 1] * (1.0f / 255.0f);
                        sum[2] += (float) src[i + 2] * (1.0f / 255.0f);
                    }
                    sum[3] += (float) src[i + 3] * (1.0f / 255.0f);
                }
            }
            size_t o = (((size_t) y * out.width) + x) * 4;
            if (srgb) {
                out.pixels[o + 0] = encodeSrgb(sum[0] * inv);
                out.pixels[o + 1] = encodeSrgb(sum[1] * inv);
                out.pixels[o + 2] = encodeSrgb(sum[2] * inv);
            } else {
                out.pixels[o + 0] = encodeUnorm(sum[0] * inv);
                out.pixels[o + 1] = encodeUnorm(sum[1] * inv);
                out.pixels[o + 2] = encodeUnorm(sum[2] * inv);
            }
            out.pixels[o + 3] = encodeUnorm(sum[3] * inv);
        }
    }
}

// One level down, rows split across threads for the large levels. The renderer library
// does not see the engine's job system, and this runs once per texture at load.
auto downsample(uint32_t srcW, uint32_t srcH, std::span<const uint8_t> src, bool srgb) -> MipLevel {
    MipLevel out;
    out.width = std::max(1u, srcW / 2);
    out.height = std::max(1u, srcH / 2);
    out.pixels.resize((size_t) out.width * out.height * 4);
    constexpr uint32_t rowsPerThreadMin = 64;
    uint32_t threads = std::max(1u, std::min(std::thread::hardware_concurrency(), out.height / rowsPerThreadMin));
    if (threads <= 1) {
        downsampleRows(srcW, srcH, src, out, 0, out.height, srgb);
        return out;
    }
    std::vector<std::jthread> workers;
    workers.reserve(threads);
    uint32_t rowsPerThread = (out.height + threads - 1) / threads;
    for (uint32_t t = 0; t < threads; t++) {
        uint32_t rowBegin = t * rowsPerThread;
        uint32_t rowEnd = std::min(out.height, rowBegin + rowsPerThread);
        if (rowBegin >= rowEnd) {
            break;
        }
        workers.emplace_back([&, rowBegin, rowEnd] { downsampleRows(srcW, srcH, src, out, rowBegin, rowEnd, srgb); });
    }
    workers.clear(); // joins
    return out;
}

} // namespace

auto mipLevelCount(uint32_t width, uint32_t height) -> uint32_t {
    uint32_t levels = 1;
    while (width > 1 || height > 1) {
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        levels++;
    }
    return levels;
}

auto buildMipChain(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, bool srgb) -> std::vector<MipLevel> {
    std::vector<MipLevel> chain;
    uint32_t w = width;
    uint32_t h = height;
    std::span<const uint8_t> src = rgba;
    while (w > 1 || h > 1) {
        chain.push_back(downsample(w, h, src, srgb));
        const auto& level = chain.back();
        w = level.width;
        h = level.height;
        src = level.pixels;
    }
    return chain;
}

auto packMipChain(std::span<const uint8_t> level0, std::span<const MipLevel> chain) -> std::vector<std::byte> {
    size_t total = level0.size();
    for (const auto& level : chain) {
        total += level.pixels.size();
    }
    std::vector<std::byte> packed(total);
    size_t offset = 0;
    std::memcpy(packed.data(), level0.data(), level0.size());
    offset += level0.size();
    for (const auto& level : chain) {
        std::memcpy(packed.data() + offset, level.pixels.data(), level.pixels.size());
        offset += level.pixels.size();
    }
    return packed;
}
