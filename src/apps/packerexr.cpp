// ngen-packer-exr: packs an OpenEXR image (a dome light's lat-long sky) as an engine-ready HDR texture (src/asset/pack/packedtexture.h).
//
// Decodes the source with tinyexr to linear float RGB and writes one Rgb9e5 level with its irradiance: 9 spherical-harmonic coefficients per
// channel of the light it casts as a lat-long dome, so the view neither decodes nor integrates it. The depfile lists only the source.

#include "packedtexture.h"
#include "packer.h"
#include "profile.h"

// tinyexr inflates through stb's zlib: stb_image's decoder, and stb_image_write's encoder, which it names but the packer never calls.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include <tinyexr.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <numbers>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace {

// The irradiance of a lat-long HDR image (linear RGB, `width` x `height`) in its own frame (packedtexture.h): the radiance projected on
// the 9 real spherical harmonics of bands 0 to 2, each texel weighted by its solid angle, then convolved with the cosine lobe.
auto latLongIrradiance(std::span<const float> rgb, uint32_t width, uint32_t height) -> PackedIrradiance {
    std::array<std::array<double, 3>, 9> radiance = {};
    auto pi = std::numbers::pi;
    for (uint32_t row = 0; row < height; row++) {
        double theta = pi * ((double) row + 0.5) / (double) height;
        double sinTheta = std::sin(theta);
        double z = std::cos(theta);
        double solidAngle = (2.0 * pi / (double) width) * (pi / (double) height) * sinTheta;
        for (uint32_t column = 0; column < width; column++) {
            double phi = pi - (2.0 * pi * ((double) column + 0.5) / (double) width);
            double x = sinTheta * std::sin(phi);
            double y = -sinTheta * std::cos(phi);
            std::array<double, 9> basis = {
                0.282095,
                0.488603 * y,
                0.488603 * z,
                0.488603 * x,
                1.092548 * x * y,
                1.092548 * y * z,
                0.315392 * ((3.0 * z * z) - 1.0),
                1.092548 * x * z,
                0.546274 * ((x * x) - (y * y)),
            };
            const float* texel = &rgb[(((size_t) row * width) + column) * 3];
            for (size_t i = 0; i < basis.size(); i++) {
                for (size_t c = 0; c < 3; c++) {
                    radiance[i][c] += (double) texel[c] * basis[i] * solidAngle;
                }
            }
        }
    }
    // The cosine lobe's band weights: π, 2π/3 and π/4.
    std::array<double, 9> band = {pi, 2.0 * pi / 3.0, 2.0 * pi / 3.0, 2.0 * pi / 3.0, pi / 4.0, pi / 4.0, pi / 4.0, pi / 4.0, pi / 4.0};
    PackedIrradiance out;
    for (size_t i = 0; i < 9; i++) {
        for (size_t c = 0; c < 3; c++) {
            out[(i * 3) + c] = (float) (radiance[i][c] * band[i]);
        }
    }
    return out;
}

// An OpenEXR image's packed texture: one Rgb9e5 level, and its irradiance as a lat-long dome.
auto packExr(const std::vector<std::byte>& source, const std::string& name) -> std::expected<std::vector<std::byte>, std::string> {
    float* rgba = nullptr;
    int width = 0;
    int height = 0;
    const char* error = nullptr;
    if (LoadEXRFromMemory(&rgba, &width, &height, (const unsigned char*) source.data(), source.size(), &error) != TINYEXR_SUCCESS) {
        std::string message = error != nullptr ? error : "unknown error";
        FreeEXRErrorMessage(error);
        return std::unexpected(std::format("cannot decode {}: {}", name, message));
    }
    auto texels = (size_t) width * (size_t) height;
    std::vector<float> rgb(texels * 3);
    std::vector<std::byte> level(texels * 4);
    for (size_t i = 0; i < texels; i++) {
        for (size_t c = 0; c < 3; c++) {
            rgb[(i * 3) + c] = std::isfinite(rgba[(i * 4) + c]) ? std::max(0.0f, rgba[(i * 4) + c]) : 0.0f;
        }
        auto texel = encodeRgb9e5(rgb[i * 3], rgb[(i * 3) + 1], rgb[(i * 3) + 2]);
        std::memcpy(&level[i * 4], &texel, 4);
    }
    std::free(rgba);
    auto irradiance = latLongIrradiance(rgb, (uint32_t) width, (uint32_t) height);
    return writePackedTexture(PackedTextureFormat::Rgb9e5, (uint32_t) width, (uint32_t) height, 1, level, irradiance);
}

} // namespace

auto main(int argc, char** argv) -> int {
    // A packer runs for milliseconds: Tracy only when NGEN_TRACY=1 asks (its start-up alone takes 200 ms).
    profile::startTracyIfAsked();
    profile::registerThread("Main");
    PROFILE_ZONE("PackerMain", Assets);
    auto args = parsePackerArgs(argc, argv);
    if (!args) {
        std::println(stderr, "ngen-packer-exr: {}", args.error());
        return 2;
    }
    auto source = readPackerFile(args->source);
    if (!source) {
        std::println(stderr, "ngen-packer-exr: {}", source.error());
        return 1;
    }
    auto packed = packExr(*source, args->source);
    if (!packed) {
        std::println(stderr, "ngen-packer-exr: {}", packed.error());
        return 1;
    }

    std::ofstream out(args->out, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(packed->data()), (std::streamsize) packed->size());
    if (!out) {
        std::println(stderr, "ngen-packer-exr: cannot write {}", args->out);
        return 1;
    }
    std::ofstream depfile(args->depfile, std::ios::trunc);
    depfile << depfileEscape(args->out) << ": " << depfileEscape(args->source) << '\n';
    if (!depfile) {
        std::println(stderr, "ngen-packer-exr: cannot write {}", args->depfile);
        return 1;
    }
    return 0;
}
