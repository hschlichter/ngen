#pragma once

#include "consolecommands.h"
#include "rpcrecords.h"
#include "rpcregistry.h"

class RemoteView;

// The remote view's commands (src/remoteview/README.md): the `remoteview` record, and remoteview.keyframe, remoteview.disconnect and
// remoteview.grab with their console commands (remoteview-keyframe, remoteview-disconnect, remoteview-grab). Its settings are cvars
// (remoteview.*), set as every cvar is. Registered by a program that has a remote view; `remoteView` outlives the registry.
auto registerRemoteViewCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, RemoteView& remoteView) -> void;
