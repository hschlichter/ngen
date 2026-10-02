#include "packedtexture.h"

#include <algorithm>
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

auto writePackedTexture(PackedTextureFormat format, uint32_t width, uint32_t height, uint32_t mipLevels, std::span<const std::byte> levels)
    -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(packedTextureHeaderSize + levels.size());
    for (char ch : {'N', 'G', 'T', 'X'}) {
        out.push_back((std::byte) ch);
    }
    putU32(out, packedTextureVersion);
    putU32(out, (uint32_t) format);
    putU32(out, width);
    putU32(out, height);
    putU32(out, mipLevels);
    out.insert(out.end(), levels.begin(), levels.end());
    return out;
}

auto readPackedTexture(std::span<const std::byte> bytes) -> std::optional<PackedTextureView> {
    if (bytes.size() < packedTextureHeaderSize || std::memcmp(bytes.data(), "NGTX", 4) != 0) {
        return std::nullopt;
    }
    if (getU32(bytes, 4) != packedTextureVersion || getU32(bytes, 8) != (uint32_t) PackedTextureFormat::Rgba8Srgb) {
        return std::nullopt;
    }
    PackedTextureView view;
    view.format = PackedTextureFormat::Rgba8Srgb;
    view.width = getU32(bytes, 12);
    view.height = getU32(bytes, 16);
    view.mipLevels = getU32(bytes, 20);
    if (view.width == 0 || view.height == 0 || view.mipLevels == 0 || view.mipLevels > 32) {
        return std::nullopt;
    }
    auto size = packedTextureLevelsSize(view.width, view.height, view.mipLevels);
    if (bytes.size() != packedTextureHeaderSize + size) {
        return std::nullopt;
    }
    view.levels = bytes.subspan(packedTextureHeaderSize, size);
    return view;
}
