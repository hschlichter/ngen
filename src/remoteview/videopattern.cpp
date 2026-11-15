#include "videopattern.h"

#include <cstddef>

namespace videopattern {

// At three quarters, so a conversion that overshoots shows as a wrong value, not as a clipped one.
const std::array<std::array<uint8_t, 3>, barCount> barColors = {{
    {191, 191, 191},
    {191, 191, 0},
    {0, 191, 191},
    {0, 191, 0},
    {191, 0, 191},
    {191, 0, 0},
    {0, 0, 191},
    {16, 16, 16},
}};

namespace {

auto fill(VideoImage& image, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, const std::array<uint8_t, 3>& color) -> void {
    for (uint32_t y = y0; y < y1; y++) {
        uint8_t* pixel = image.rgba.data() + ((size_t) y * image.width + x0) * 4;
        for (uint32_t x = x0; x < x1; x++) {
            pixel[0] = color[0];
            pixel[1] = color[1];
            pixel[2] = color[2];
            pixel[3] = 255;
            pixel += 4;
        }
    }
}

} // namespace

auto draw(VideoImage& image, uint64_t unixMs) -> void {
    uint32_t width = image.width;
    uint32_t height = image.height;
    image.rgba.resize((size_t) width * height * 4);
    if (width == 0 || height == 0) {
        return;
    }
    uint32_t barsEnd = height / 2;
    uint32_t cellsStart = height - height / 8;
    for (uint32_t bar = 0; bar < barCount; bar++) {
        fill(image, bar * width / barCount, 0, (bar + 1) * width / barCount, barsEnd, barColors[bar]);
    }
    fill(image, 0, barsEnd, width, cellsStart, {40, 40, 40});
    // Across the image in four seconds.
    uint32_t markWidth = width / 64 + 1;
    uint32_t markX = (uint32_t) ((unixMs % 4000) * (width - markWidth) / 4000);
    fill(image, markX, barsEnd, markX + markWidth, cellsStart, {235, 235, 235});
    auto stamp = (uint32_t) unixMs;
    for (uint32_t cell = 0; cell < timeCells; cell++) {
        bool one = ((stamp >> (timeCells - 1 - cell)) & 1u) != 0;
        std::array<uint8_t, 3> color = one ? std::array<uint8_t, 3>{235, 235, 235} : std::array<uint8_t, 3>{16, 16, 16};
        fill(image, cell * width / timeCells, cellsStart, (cell + 1) * width / timeCells, height, color);
    }
}

} // namespace videopattern
