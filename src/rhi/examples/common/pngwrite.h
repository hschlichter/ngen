#pragma once

// PNG output for example screenshots. Header-only; include from exactly one
// translation unit per example because it carries the stb implementation.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdint>
#include <print>
#include <vector>

inline auto writePng(const char* path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) -> bool {
    auto ok = stbi_write_png(path, (int) width, (int) height, 4, rgba.data(), (int) width * 4) != 0;
    if (!ok) {
        std::println(stderr, "failed to write {}", path);
    } else {
        std::println("screenshot written: {} ({}x{})", path, width, height);
    }
    return ok;
}
