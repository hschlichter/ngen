#pragma once

#include <cstdint>
#include <string>
#include <vector>

// PNG writer for engine screenshots. Input is tightly packed RGBA8.
auto writeScreenshotPng(const char* path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) -> bool;

// A screenshot the renderer has written (after its frame's fence), reported back to the main
// thread through RenderThread::takeScreenshotResults.
struct ScreenshotResult {
    std::string path;
    bool ok = false;
    uint64_t frame = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};
