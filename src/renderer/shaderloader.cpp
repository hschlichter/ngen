#include "shaderloader.h"

#include "rhidevice.h"

#include <cstddef>
#include <fstream>
#include <ios>
#include <print>
#include <vector>

auto loadShaderModule(RhiDevice* device, RhiShaderStage stage, const char* filepath) -> RhiShaderModule* {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::println(stderr, "Failed to open shader file: {}", filepath);
        return nullptr;
    }

    auto size = (size_t) file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> code(size);
    if (!file.read((char*) code.data(), (std::streamsize) size)) {
        std::println(stderr, "Failed to read shader file: {}", filepath);
        return nullptr;
    }

    RhiShaderDesc desc = {
        .stage = stage,
        .code = code,
    };
    auto* module = device->createShaderModule(desc);
    if (module == nullptr) {
        std::println(stderr, "Failed to create shader module: {}", filepath);
        return nullptr;
    }

    std::println("Loaded shader: {}", filepath);
    return module;
}
