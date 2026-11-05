#pragma once

#include "capture.h"

#include <cstddef>
#include <cstdio>
#include <string>

// Writes a capture as files: buffers as JSON rows decoded with their schema (at most
// maxRows), textures as a PNG of the display conversion plus `<path>.json` with format,
// size and channel ranges. Returns false when a file cannot be written.
auto writeCaptureFiles(const std::string& path, const CaptureResult& result, const CaptureDisplay& display, size_t maxRows) -> bool;
// The JSON part of a capture into an open stream: a texture's sidecar (format, size, ranges,
// texels of small regions) or a buffer's decoded rows (at most maxRows).
auto writeCaptureJson(FILE* f, const CaptureResult& result, size_t maxRows) -> void;
