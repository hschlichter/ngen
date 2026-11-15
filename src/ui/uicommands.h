#pragma once

#include "consolecommands.h"
#include "rpcrecords.h"
#include "rpcregistry.h"

class ViewUI;

// The UI's commands: view.window and its console command `window` open and close a window by name; the `windows` record says which are open. `ui`
// outlives the registry.
auto registerUiCommands(RpcRegistry& registry, ConsoleCommands& console, RpcRecords& records, ViewUI& ui) -> void;
