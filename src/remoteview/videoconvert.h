#pragma once

#include "videotypes.h"

// RGBA as displayed to Y'CbCr 4:2:0 with the BT.709 matrix at limited range (16-235, 16-240), each chroma sample the mean of its four
// pixels. A width or height that is odd loses its last column or row.
auto convertToPlanes(const VideoImage& image, VideoPlanes& planes) -> void;
