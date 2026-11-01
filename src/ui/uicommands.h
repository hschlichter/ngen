#pragma once

#include "rpcrecords.h"
#include "rpcregistry.h"
#include "rpcverbs.h"

class ViewUI;

// The UI's commands: view.window and its verb `window` open and close a window by name; the `windows` record says which are open. `ui`
// outlives the registry.
auto registerUiCommands(RpcRegistry& registry, RpcVerbs& verbs, RpcRecords& records, ViewUI& ui) -> void;
