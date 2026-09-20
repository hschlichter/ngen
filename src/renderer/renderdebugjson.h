#pragma once

#include "renderdebug.h"

#include <functional>
#include <string>

// Writes a RenderDebugSnapshot as JSON. primPath resolves a prim index to its USD path
// (may return an empty string). Returns false if the file cannot be written.
auto writeRenderDebugJson(const char* path, const RenderDebugSnapshot& snap, const std::function<std::string(uint32_t)>& primPath) -> bool;
