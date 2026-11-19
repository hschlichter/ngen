#pragma once

#include "videotypes.h"

#include <array>
#include <cstdint>

// The test pattern, the remote view's second source (remoteview.source pattern): what a watcher gets with no renderer involved, and what
// the watch page measures against. From the top: eight colour bars over half the height; a white bar moving over dark grey; and, over
// the bottom eighth, the time the image was drawn as 32 cells, black for 0 and white for 1, the most significant bit on the left.
namespace videopattern {

constexpr uint32_t timeCells = 32;
constexpr uint32_t barCount = 8;
// The bars' colours, left to right, as drawn.
extern const std::array<std::array<uint8_t, 3>, barCount> barColors;

// `image` keeps its width and height; `unixMs` is the time drawn into the cells, its low 32 bits.
auto draw(VideoImage& image, uint64_t unixMs) -> void;

} // namespace videopattern
