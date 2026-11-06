#pragma once

#include "assetlibrary.h"
#include "scenehandles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <vector>

enum class TextureFormat : uint8_t {
    Rgba8Srgb, // an image's colour
    Rgb9e5,    // HDR linear radiance, shared exponent (a dome light's lat-long sky)
};

// A texture: 4-byte texels, every mip level packed in order, level 0 first (as the texture packer writes it, or a generated 1x1). An HDR
// lat-long image carries its irradiance: 9 spherical-harmonic coefficients of the light it casts, in its own frame (packedtexture.h).
struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    TextureFormat format = TextureFormat::Rgba8Srgb;
    std::vector<std::byte> levels;
    std::optional<std::array<glm::vec3, 9>> irradiance;
};

struct MaterialDesc {
    glm::vec4 baseColorFactor = glm::vec4(1.0f);
    // The base colour texture: a texture asset, or the generated 1x1 of the constant colour.
    TextureHandle baseColorTexture;
    // Cutout: drawn only where the opacity reaches opacityThreshold; 0 is opaque. The opacity is a channel of the opacity texture, of the
    // base colour texture when opacityFromBaseColor, or the constant when there is neither.
    float opacityThreshold = 0.0f;
    float opacity = 1.0f;
    uint32_t opacityChannel = 0; // 0 r, 1 g, 2 b, 3 a
    bool opacityRaw = false;     // authored raw: the sampler's sRGB decode is undone before the comparison
    bool opacityFromBaseColor = false;
    TextureHandle opacityTexture;

    auto cutout() const -> bool { return opacityThreshold > 0.0f; }
    auto operator==(const MaterialDesc&) const -> bool = default;
};

// Materials by material asset id, or by display colour for prims without one.
using MaterialLibrary = AssetLibrary<MaterialHandle, MaterialDesc>;
// Textures by texture asset id, or by colour for the generated 1x1s; shared by the materials naming them.
using TextureLibrary = AssetLibrary<TextureHandle, TextureDesc>;
