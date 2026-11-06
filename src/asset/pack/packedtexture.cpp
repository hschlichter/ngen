#include "packedtexture.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace {

auto putU32(std::vector<std::byte>& out, uint32_t value) -> void {
    for (int i = 0; i < 4; i++) {
        out.push_back((std::byte) ((value >> (8 * i)) & 0xff));
    }
}

auto getU32(std::span<const std::byte> bytes, size_t offset) -> uint32_t {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= (uint32_t) bytes[offset + i] << (8 * i);
    }
    return value;
}

} // namespace

auto packedTextureLevelsSize(uint32_t width, uint32_t height, uint32_t mipLevels) -> size_t {
    size_t size = 0;
    for (uint32_t level = 0; level < mipLevels; level++) {
        size_t w = std::max(1u, width >> level);
        size_t h = std::max(1u, height >> level);
        size += w * h * 4;
    }
    return size;
}

auto writePackedTexture(PackedTextureFormat format, uint32_t width, uint32_t height, uint32_t mipLevels, std::span<const std::byte> levels, const std::optional<PackedIrradiance>& irradiance) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(packedTextureHeaderSize + (irradiance ? packedIrradianceFloats * 4 : 0) + levels.size());
    for (char ch : {'N', 'G', 'T', 'X'}) {
        out.push_back((std::byte) ch);
    }
    putU32(out, packedTextureVersion);
    putU32(out, (uint32_t) format);
    putU32(out, width);
    putU32(out, height);
    putU32(out, mipLevels);
    putU32(out, irradiance ? packedIrradianceFloats : 0);
    if (irradiance) {
        for (float value : *irradiance) {
            putU32(out, std::bit_cast<uint32_t>(value));
        }
    }
    out.insert(out.end(), levels.begin(), levels.end());
    return out;
}

auto readPackedTexture(std::span<const std::byte> bytes) -> std::optional<PackedTextureView> {
    if (bytes.size() < packedTextureHeaderSize || std::memcmp(bytes.data(), "NGTX", 4) != 0 || getU32(bytes, 4) != packedTextureVersion) {
        return std::nullopt;
    }
    PackedTextureView view;
    auto format = getU32(bytes, 8);
    if (format != (uint32_t) PackedTextureFormat::Rgba8Srgb && format != (uint32_t) PackedTextureFormat::Rgb9e5) {
        return std::nullopt;
    }
    view.format = (PackedTextureFormat) format;
    view.width = getU32(bytes, 12);
    view.height = getU32(bytes, 16);
    view.mipLevels = getU32(bytes, 20);
    auto irradianceFloats = getU32(bytes, 24);
    if (view.width == 0 || view.height == 0 || view.mipLevels == 0 || view.mipLevels > 32) {
        return std::nullopt;
    }
    if (irradianceFloats != 0 && irradianceFloats != packedIrradianceFloats) {
        return std::nullopt;
    }
    auto levelsOffset = packedTextureHeaderSize + (size_t) irradianceFloats * 4;
    auto size = packedTextureLevelsSize(view.width, view.height, view.mipLevels);
    if (bytes.size() != levelsOffset + size) {
        return std::nullopt;
    }
    if (irradianceFloats != 0) {
        PackedIrradiance irradiance;
        for (uint32_t i = 0; i < packedIrradianceFloats; i++) {
            irradiance[i] = std::bit_cast<float>(getU32(bytes, packedTextureHeaderSize + (size_t) i * 4));
        }
        view.irradiance = irradiance;
    }
    view.levels = bytes.subspan(levelsOffset, size);
    return view;
}

auto encodeRgb9e5(float r, float g, float b) -> uint32_t {
    // Vulkan's shared-exponent encoding: 9 mantissa bits, exponent bias 15, the exponent chosen for the largest channel.
    constexpr int mantissaBits = 9;
    constexpr int exponentBias = 15;
    constexpr int maxExponent = 31;
    constexpr float maxValue = (float) ((1 << mantissaBits) - 1) / (float) (1 << mantissaBits) * (float) (1 << (maxExponent - exponentBias));
    auto clampChannel = [maxValue](float value) {
        return std::isfinite(value) ? std::clamp(value, 0.0f, maxValue) : 0.0f;
    };
    r = clampChannel(r);
    g = clampChannel(g);
    b = clampChannel(b);
    float largest = std::max({r, g, b});
    if (largest <= 0.0f) {
        return 0;
    }
    int exponent = std::max(-exponentBias - 1, (int) std::floor(std::log2(largest))) + 1 + exponentBias;
    auto scale = std::exp2((float) (exponent - exponentBias - mantissaBits));
    if ((int) std::floor(largest / scale + 0.5f) == (1 << mantissaBits)) {
        scale *= 2.0f;
        exponent++;
    }
    auto mantissa = [scale](float value) {
        return (uint32_t) std::floor(value / scale + 0.5f);
    };
    return mantissa(r) | (mantissa(g) << 9) | (mantissa(b) << 18) | ((uint32_t) exponent << 27);
}

auto decodeRgb9e5(uint32_t texel) -> std::array<float, 3> {
    auto scale = std::exp2((float) ((int) (texel >> 27) - 15 - 9));
    return {(float) (texel & 0x1ff) * scale, (float) ((texel >> 9) & 0x1ff) * scale, (float) ((texel >> 18) & 0x1ff) * scale};
}
