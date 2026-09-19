#pragma once

#include "rhitypes.h"

class RhiDevice;

// Read a compiled shader from disk and create an RHI shader module from it.
// Returns nullptr and logs on failure. File format is whatever the active backend
// consumes (SPIR-V for Vulkan); the build's `shaders` tool produces it.
auto loadShaderModule(RhiDevice* device, RhiShaderStage stage, const char* filepath) -> RhiShaderModule*;
