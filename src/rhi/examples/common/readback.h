#pragma once

#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "rhitypes.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <vector>

// GPU-to-CPU readback for examples. Header-only. The RHI only records the copy;
// the example owns the buffer, the barriers, the submit and the wait, same as
// any other integrator.

inline auto createReadbackBuffer(RhiDevice& device, RhiExtent2D extent) -> RhiBuffer* {
    RhiBufferDesc desc = {
        .size = (uint64_t) extent.width * extent.height * 4,
        .usage = RhiBufferUsage::TransferDst,
        .memory = RhiMemoryUsage::CpuToGpu,
    };
    return device.createBuffer(desc);
}

// Records: texture fromLayout -> TransferSrc, copy into buffer, TransferSrc -> toLayout.
inline auto recordReadback(RhiCommandBuffer* cmd, RhiTexture* texture, RhiBuffer* buffer, RhiExtent2D extent, RhiTextureState fromLayout, RhiTextureState toLayout) -> void {
    std::array<RhiTextureBarrierDesc, 1> toTransfer = {{
        {.texture = texture, .oldState = fromLayout, .newState = RhiTextureState::TransferSrc},
    }};
    cmd->pipelineBarrier(toTransfer);
    cmd->copyTextureToBuffer(texture, buffer, {.width = extent.width, .height = extent.height});
    std::array<RhiTextureBarrierDesc, 1> toFinal = {{
        {.texture = texture, .oldState = RhiTextureState::TransferSrc, .newState = toLayout},
    }};
    cmd->pipelineBarrier(toFinal);
}

// Maps the buffer after its submit has completed and returns tightly packed RGBA8,
// swizzled from BGRA when the texture format stores blue first. Bytes are left as
// stored: for an sRGB format they are sRGB-encoded.
inline auto resolveReadback(RhiDevice& device, RhiBuffer* buffer, RhiExtent2D extent, RhiFormat format) -> std::vector<uint8_t> {
    auto count = (size_t) extent.width * extent.height * 4;
    std::vector<uint8_t> rgba(count);
    auto* mapped = static_cast<const uint8_t*>(device.mapBuffer(buffer));
    memcpy(rgba.data(), mapped, count);
    device.unmapBuffer(buffer);

    bool blueFirst = format == RhiFormat::B8G8R8A8_SRGB || format == RhiFormat::B8G8R8A8_UNORM;
    if (blueFirst) {
        for (size_t i = 0; i < count; i += 4) {
            std::swap(rgba[i], rgba[i + 2]);
        }
    }
    return rgba;
}

inline auto isSrgbFormat(RhiFormat format) -> bool {
    return format == RhiFormat::B8G8R8A8_SRGB || format == RhiFormat::R8G8B8A8_SRGB;
}

// Linear [0,1] to the byte an sRGB attachment stores.
inline auto srgbEncode(float linear) -> uint8_t {
    float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return (uint8_t) std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f);
}

// Bytes a shader output of linear `rgb` lands as in `format`.
inline auto expectedBytes(RhiFormat format, std::array<float, 3> rgb) -> std::array<uint8_t, 3> {
    std::array<uint8_t, 3> out = {};
    for (size_t i = 0; i < 3; i++) {
        if (isSrgbFormat(format)) {
            out[i] = srgbEncode(rgb[i]);
        } else {
            out[i] = (uint8_t) std::lround(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f);
        }
    }
    return out;
}

// One check, one line of output. Returns false on mismatch.
inline auto expectPixel(const std::vector<uint8_t>& rgba, RhiExtent2D extent, uint32_t x, uint32_t y, std::array<uint8_t, 3> expected, int tolerance, const char* label) -> bool {
    auto i = ((size_t) y * extent.width + x) * 4;
    std::array<uint8_t, 3> actual = {rgba[i], rgba[i + 1], rgba[i + 2]};
    bool ok = true;
    for (size_t c = 0; c < 3; c++) {
        if (std::abs((int) actual[c] - (int) expected[c]) > tolerance) {
            ok = false;
        }
    }
    std::println("check {}: pixel ({},{}) = ({},{},{}) expected ({},{},{}) +-{} {}", label, x, y, actual[0], actual[1], actual[2], expected[0], expected[1], expected[2], tolerance, ok ? "ok" : "FAIL");
    return ok;
}
