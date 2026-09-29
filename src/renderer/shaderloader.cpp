#include "shaderloader.h"

#include "rhidevice.h"

#include <cstddef>
#include <fstream>
#include <ios>
#include <print>
#include <string>
#include <vector>

static std::string shaderPackRoot = "packs";

auto setShaderPackRoot(std::string dir) -> void {
    shaderPackRoot = std::move(dir);
}

auto loadShaderModule(RhiDevice* device, RhiShaderStage stage, const char* id) -> RhiShaderModule* {
    auto resolved = shaderPackRoot + "/" + id;
    std::ifstream file(resolved, std::ios::binary | std::ios::ate);
    if (!file) {
        std::println(stderr, "Failed to open packed shader {}: {}", id, resolved);
        return nullptr;
    }

    auto size = (size_t) file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> code(size);
    if (!file.read((char*) code.data(), (std::streamsize) size)) {
        std::println(stderr, "Failed to read packed shader: {}", resolved);
        return nullptr;
    }

    RhiShaderDesc desc = {
        .stage = stage,
        .code = code,
        .debugName = id,
    };
    auto* module = device->createShaderModule(desc);
    if (module == nullptr) {
        std::println(stderr, "Failed to create shader module: {}", id);
        return nullptr;
    }

    std::println("Loaded shader: {}", id);
    return module;
}
