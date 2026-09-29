#pragma once

#include "rhitypes.h"

#include <string>

class RhiDevice;

// Create an RHI shader module from a packed shader. `id` is the shader's asset id, its project-relative source
// path (e.g. "shaders/gbuffer.vert"); the bytecode is the packed file <packs root>/<id>, in whatever format the
// active backend consumes (SPIR-V for Vulkan). Returns nullptr and logs on failure.
auto loadShaderModule(RhiDevice* device, RhiShaderStage stage, const char* id) -> RhiShaderModule*;

// The directory packed assets are read from: <out_dir>/packs, next to the executable. The application passes it
// at start-up; the default is "packs" in the working directory.
auto setShaderPackRoot(std::string path) -> void;
