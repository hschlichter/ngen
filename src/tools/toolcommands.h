#pragma once

#include "tools.h"
#include "viewcommands.h"

// The tools' commands: the debug camera's actions (view.camera.debug), the LOD and Streaming tools' inspections (view.lod.inspect,
// view.streaming.inspect), their verbs (debug-camera, lod-show, streaming-show) and records (debugcamera, lodtool, streamingtool). They also
// give the engine's camera commands the debug camera (`camera: "debug"`). `tools` and `view` outlive the registry.
auto registerToolCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewContext& view, ToolsState& tools) -> void;
