#pragma once

#include "consolecommands.h"
#include "rpcrecords.h"
#include "rpcregistry.h"

class RemoteControl;
class RemoteView;

// The remote view's commands (src/remoteview/README.md): the `remoteview` record (remote control's part in it as `control`), and
// remoteview.keyframe, remoteview.disconnect, remoteview.grab and remoteview.screenshot with their console commands
// (remoteview-keyframe, remoteview-disconnect, remoteview-grab, remoteview-screenshot). Its settings are cvars (remoteview.*), set as
// every cvar is. Registered by a program that has a remote view; `remoteView`, `remoteControl` and the registry outlive the commands.
auto registerRemoteViewCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, RemoteView& remoteView, const RemoteControl& remoteControl) -> void;
