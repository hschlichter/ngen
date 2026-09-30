#pragma once

#include "rhitypes.h"

#include <span>
#include <string>

class AssetClient;
class RhiDevice;

// Create an RHI shader module from a packed shader. `id` is the shader's asset id, its project-relative source
// path (e.g. "shaders/gbuffer.vert"); the bytecode is what the asset server streamed for it, in whatever format
// the active backend consumes (SPIR-V for Vulkan). `id` must be one of startupShaderIds(). Returns nullptr and
// logs on failure.
auto loadShaderModule(RhiDevice* device, RhiShaderStage stage, const char* id) -> RhiShaderModule*;

// Every shader the passes load in init(). The application requests them all in one batch and waits for them
// before the renderer initialises.
auto startupShaderIds() -> std::span<const std::string>;

// Where loadShaderModule takes the bytes from. The application connects the client and passes it at start-up.
auto setShaderSource(const AssetClient* source) -> void;
