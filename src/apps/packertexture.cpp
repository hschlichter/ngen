// ngen-packer-texture: packs an image as an engine-ready texture (src/asset/pack/packedtexture.h).
//
// Decodes the source with stb_image to RGBA8, builds the full mip chain with the filter the renderer uses (colour averaged
// in linear space), and writes the packed texture: RGBA8 sRGB, every level, ready to upload. The view no longer decodes or
// mip-maps. The depfile lists only the source.

#include "mipchain.h"
#include "packedtexture.h"
#include "packer.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstddef>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace {

// Make escapes spaces, '#' and '\' in depfile paths.
auto depfileEscape(const std::string& path) -> std::string {
    std::string out;
    for (char ch : path) {
        if (ch == ' ' || ch == '#' || ch == '\\') {
            out += '\\';
        }
        out += ch;
    }
    return out;
}

} // namespace

auto main(int argc, char** argv) -> int {
    auto args = parsePackerArgs(argc, argv);
    if (!args) {
        std::println(stderr, "ngen-packer-texture: {}", args.error());
        return 2;
    }
    auto source = readPackerFile(args->source);
    if (!source) {
        std::println(stderr, "ngen-packer-texture: {}", source.error());
        return 1;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    auto* pixels = stbi_load_from_memory((const stbi_uc*) source->data(), (int) source->size(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        std::println(stderr, "ngen-packer-texture: cannot decode {}: {}", args->source, stbi_failure_reason());
        return 1;
    }
    auto level0 = std::span<const uint8_t>(pixels, (size_t) width * (size_t) height * 4);
    auto chain = buildMipChain((uint32_t) width, (uint32_t) height, level0, true);
    auto levels = packMipChain(level0, chain);
    stbi_image_free(pixels);
    auto mipLevels = mipLevelCount((uint32_t) width, (uint32_t) height);
    auto packed = writePackedTexture(PackedTextureFormat::Rgba8Srgb, (uint32_t) width, (uint32_t) height, mipLevels, levels);

    std::ofstream out(args->out, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(packed.data()), (std::streamsize) packed.size());
    if (!out) {
        std::println(stderr, "ngen-packer-texture: cannot write {}", args->out);
        return 1;
    }
    std::ofstream depfile(args->depfile, std::ios::trunc);
    depfile << depfileEscape(args->out) << ": " << depfileEscape(args->source) << '\n';
    if (!depfile) {
        std::println(stderr, "ngen-packer-texture: cannot write {}", args->depfile);
        return 1;
    }
    return 0;
}
