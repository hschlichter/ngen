#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A packed texture, as ngen-packer-texture writes it and the engine reads it: a header, an optional irradiance block, then every mip level
// tightly packed, level 0 first. Level n is max(1, width >> n) x max(1, height >> n) texels of 4 bytes, the layout GpuUploader::uploadTexture
// takes. Little-endian.
//
//   offset  size  field
//   0       4     magic "NGTX"
//   4       4     u32 version (2)
//   8       4     u32 format (PackedTextureFormat)
//   12      4     u32 width
//   16      4     u32 height
//   20      4     u32 mipLevels (an image: the full chain, floor(log2(max(width, height))) + 1; an HDR lat-long: 1)
//   24      4     u32 irradianceFloats (0, or 27: the irradiance block)
//   28      4·n   f32 irradiance block
//   ...           the levels

inline constexpr uint32_t packedTextureVersion = 2;
inline constexpr size_t packedTextureHeaderSize = 28;
inline constexpr uint32_t packedIrradianceFloats = 27;

enum class PackedTextureFormat : uint32_t {
    Rgba8Srgb = 1, // an image: colour encoded as sRGB, alpha linear
    Rgb9e5 = 2,    // HDR linear radiance, Vulkan's E5B9G9R9_UFLOAT_PACK32: R in bits 0-8, G 9-17, B 18-26, the shared exponent 27-31
};

// An HDR lat-long texture (a dome light's) is packed with its irradiance: the light arriving at a surface facing each direction from the whole
// image, as 9 spherical-harmonic coefficients per colour channel (bands 0 to 2, real basis, in the order 00, 1-1, 10, 11, 2-2, 2-1, 20, 21, 22;
// rgb per coefficient), already convolved with the cosine lobe: E(n) = sum c_i Y_i(n), and a Lambert surface reflects albedo · E(n) / π.
// Directions are in the image's own frame, its pole +Z: row 0 is the zenith, column 0 is longitude +π, longitude 0 is −Y and π/2 is +X
// (UsdLux's lat-long with the pole on Z).
using PackedIrradiance = std::array<float, packedIrradianceFloats>;

struct PackedTextureView {
    PackedTextureFormat format = PackedTextureFormat::Rgba8Srgb;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 0;
    std::optional<PackedIrradiance> irradiance;
    std::span<const std::byte> levels; // every level, level 0 first
};

// Total bytes of a full mip chain of 4-byte texels of this size.
auto packedTextureLevelsSize(uint32_t width, uint32_t height, uint32_t mipLevels) -> size_t;

// The header, the irradiance block when given, then `levels`.
auto writePackedTexture(PackedTextureFormat format, uint32_t width, uint32_t height, uint32_t mipLevels, std::span<const std::byte> levels, const std::optional<PackedIrradiance>& irradiance = std::nullopt) -> std::vector<std::byte>;

// Parses a packed texture; nullopt when the header is invalid or the sizes don't add up. The view points into `bytes`.
auto readPackedTexture(std::span<const std::byte> bytes) -> std::optional<PackedTextureView>;

// Linear RGB radiance (non-negative; larger than the format's 65408 is clamped) to an Rgb9e5 texel, and back.
auto encodeRgb9e5(float r, float g, float b) -> uint32_t;
auto decodeRgb9e5(uint32_t texel) -> std::array<float, 3>;
