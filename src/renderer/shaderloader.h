#pragma once

#include "rhitypes.h"

#include <string>

class RhiDevice;

// Read a compiled shader from disk and create an RHI shader module from it.
// Returns nullptr and logs on failure. File format is whatever the active backend
// consumes (SPIR-V for Vulkan); the build's `shaders` tool produces it.
auto loadShaderModule(RhiDevice* device, RhiShaderStage stage, const char* filepath) -> RhiShaderModule*;

// Directory prepended to relative shader paths. The build writes compiled shaders next to the
// executable (<out dir>/shaders/), so the application passes the executable's directory here;
// without it, loads resolve against the working directory and pick up whatever happens to be there.
auto setShaderSearchPath(std::string dir) -> void;
