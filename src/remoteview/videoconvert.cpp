#include "videoconvert.h"

#include <cstddef>

namespace {

// BT.709, limited range, as 8-bit fixed point: Y' = 16 + 0.1826 R + 0.6142 G + 0.0620 B, and the two colour differences around 128.
auto luma(int r, int g, int b) -> uint8_t {
    return (uint8_t) (((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
}

auto chromaBlue(int r, int g, int b) -> uint8_t {
    return (uint8_t) (((-26 * r - 87 * g + 112 * b + 128) >> 8) + 128);
}

auto chromaRed(int r, int g, int b) -> uint8_t {
    return (uint8_t) (((112 * r - 102 * g - 10 * b + 128) >> 8) + 128);
}

} // namespace

auto convertToPlanes(const VideoImage& image, VideoPlanes& planes) -> void {
    uint32_t width = image.width & ~1u;
    uint32_t height = image.height & ~1u;
    planes.width = width;
    planes.height = height;
    planes.y.resize((size_t) width * height);
    planes.cb.resize((size_t) (width / 2) * (height / 2));
    planes.cr.resize((size_t) (width / 2) * (height / 2));
    size_t stride = (size_t) image.width * 4;
    for (uint32_t row = 0; row < height; row += 2) {
        const uint8_t* top = image.rgba.data() + (size_t) row * stride;
        const uint8_t* bottom = top + stride;
        uint8_t* lumaTop = planes.y.data() + (size_t) row * width;
        uint8_t* lumaBottom = lumaTop + width;
        uint8_t* blue = planes.cb.data() + (size_t) (row / 2) * (width / 2);
        uint8_t* red = planes.cr.data() + (size_t) (row / 2) * (width / 2);
        for (uint32_t column = 0; column < width; column += 2) {
            const uint8_t* a = top + (size_t) column * 4;
            const uint8_t* b = a + 4;
            const uint8_t* c = bottom + (size_t) column * 4;
            const uint8_t* d = c + 4;
            lumaTop[column] = luma(a[0], a[1], a[2]);
            lumaTop[column + 1] = luma(b[0], b[1], b[2]);
            lumaBottom[column] = luma(c[0], c[1], c[2]);
            lumaBottom[column + 1] = luma(d[0], d[1], d[2]);
            int r = (a[0] + b[0] + c[0] + d[0] + 2) / 4;
            int g = (a[1] + b[1] + c[1] + d[1] + 2) / 4;
            int bl = (a[2] + b[2] + c[2] + d[2] + 2) / 4;
            blue[column / 2] = chromaBlue(r, g, bl);
            red[column / 2] = chromaRed(r, g, bl);
        }
    }
}
