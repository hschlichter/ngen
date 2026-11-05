#pragma once

#include "renderdebug.h"

#include <cstdio>
#include <functional>
#include <string>

// Writes a RenderDebugSnapshot as JSON. primPath resolves a prim index to its USD path
// (may return an empty string). Returns false if the file cannot be written.
auto writeRenderDebugJson(const char* path, const RenderDebugSnapshot& snap, const std::function<std::string(uint32_t)>& primPath) -> bool;
// The same JSON into an open stream (an RPC reply through open_memstream).
auto writeRenderDebugJson(FILE* f, const RenderDebugSnapshot& snap, const std::function<std::string(uint32_t)>& primPath) -> void;

// Writes the GPU memory part of a snapshot (heaps, allocations, totals by category) as JSON.
auto writeMemoryJson(const char* path, const RenderDebugSnapshot& snap) -> bool;
auto writeMemoryJson(FILE* f, const RenderDebugSnapshot& snap) -> void;
