#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A packed texture, as ngen-packer-texture writes it and the engine reads it: a header, then every mip level tightly
// packed, level 0 first. Level n is max(1, width >> n) x max(1, height >> n) texels of 4 bytes, the layout
// GpuUploader::uploadTexture takes. Little-endian.
//
//   offset  size  field
//   0       4     magic "NGTX"
//   4       4     u32 version (1)
//   8       4     u32 format (PackedTextureFormat)
//   12      4     u32 width
//   16      4     u32 height
//   20      4     u32 mipLevels (the full chain: floor(log2(max(width, height))) + 1)
//   24      ...   the levels

inline constexpr uint32_t packedTextureVersion = 1;
inline constexpr size_t packedTextureHeaderSize = 24;

enum class PackedTextureFormat : uint32_t {
    Rgba8Srgb = 1,
};

struct PackedTextureView {
    PackedTextureFormat format = PackedTextureFormat::Rgba8Srgb;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    std::span<const std::byte> levels; // every level, level 0 first
};

// Total bytes of a full RGBA8 mip chain of this size.
auto packedTextureLevelsSize(uint32_t width, uint32_t height, uint32_t mipLevels) -> size_t;

// The header followed by `levels`.
auto writePackedTexture(PackedTextureFormat format, uint32_t width, uint32_t height, uint32_t mipLevels, std::span<const std::byte> levels)
    -> std::vector<std::byte>;

// Parses a packed texture; nullopt when the header is invalid or the sizes don't add up. The view points into `bytes`.
auto readPackedTexture(std::span<const std::byte> bytes) -> std::optional<PackedTextureView>;
