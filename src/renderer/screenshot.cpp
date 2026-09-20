#include "screenshot.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

auto writeScreenshotPng(const char* path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) -> bool {
    return stbi_write_png(path, (int) width, (int) height, 4, rgba.data(), (int) width * 4) != 0;
}
