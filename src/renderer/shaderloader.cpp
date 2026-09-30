#include "shaderloader.h"

#include "assetclient.h"
#include "rhidevice.h"

#include <algorithm>
#include <array>
#include <print>
#include <string>

static const AssetClient* shaderSource = nullptr;

static const std::array<std::string, 17> startupShaders = {
    "shaders/debug.frag",
    "shaders/debug.vert",
    "shaders/debugview.comp",
    "shaders/debugview.frag",
    "shaders/debugview.geom",
    "shaders/debugview.vert",
    "shaders/depthonly.vert",
    "shaders/fxaa.comp",
    "shaders/gbuffer.frag",
    "shaders/gbuffer.vert",
    "shaders/gizmo.frag",
    "shaders/gizmo.vert",
    "shaders/instancecull.comp",
    "shaders/lighting.frag",
    "shaders/lighting.vert",
    "shaders/shadow.frag",
    "shaders/shadow.vert",
};

auto startupShaderIds() -> std::span<const std::string> {
    return startupShaders;
}

auto setShaderSource(const AssetClient* source) -> void {
    shaderSource = source;
}

auto loadShaderModule(RhiDevice* device, RhiShaderStage stage, const char* id) -> RhiShaderModule* {
    if (std::find(startupShaders.begin(), startupShaders.end(), id) == startupShaders.end()) {
        std::println(stderr, "Shader {} is not in startupShaderIds(); add it there so it is requested at start-up", id);
        return nullptr;
    }
    if (shaderSource == nullptr) {
        std::println(stderr, "No shader source set; cannot load {}", id);
        return nullptr;
    }
    const auto* asset = shaderSource->find(id);
    if (asset == nullptr) {
        std::println(stderr, "Shader {} was not packed:", id);
        for (const auto& line : shaderSource->errors(id)) {
            std::println(stderr, "  {}", line);
        }
        return nullptr;
    }

    RhiShaderDesc desc = {
        .stage = stage,
        .code = asset->bytes,
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
