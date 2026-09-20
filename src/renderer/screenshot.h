#pragma once

#include <cstdint>
#include <vector>

// PNG writer for engine screenshots. Input is tightly packed RGBA8.
auto writeScreenshotPng(const char* path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) -> bool;
