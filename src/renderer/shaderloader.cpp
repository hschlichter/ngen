#include "shaderloader.h"

#include "assetclient.h"
#include "rhidevice.h"
#include "trace.h"

#include <algorithm>
#include <array>
#include <format>
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
        TRACE_ERROR("Render", "ShaderLoadFailed", id).text(std::format("shader {} is not in startupShaderIds(); add it there so it is requested at start-up", id));
        return nullptr;
    }
    if (shaderSource == nullptr) {
        TRACE_ERROR("Render", "ShaderLoadFailed", id).text(std::format("no shader source set; cannot load {}", id));
        return nullptr;
    }
    const auto* asset = shaderSource->find(id);
    if (asset == nullptr) {
        std::string text = std::format("shader {} was not packed", id);
        for (const auto& line : shaderSource->errors(id)) {
            text += "\n  " + line;
        }
        TRACE_ERROR("Render", "ShaderLoadFailed", id).text(text);
        return nullptr;
    }

    RhiShaderDesc desc = {
        .stage = stage,
        .code = asset->bytes,
        .debugName = id,
    };
    auto* module = device->createShaderModule(desc);
    if (module == nullptr) {
        TRACE_ERROR("Render", "ShaderLoadFailed", id).text(std::format("cannot create the shader module {}", id));
        return nullptr;
    }

    TRACE_EVENT("Render", "ShaderLoaded", id).text(std::format("loaded shader: {}", id));
    return module;
}
