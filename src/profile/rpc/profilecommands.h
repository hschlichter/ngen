#pragma once

#include "rpcprotocol.h"
#include "rpcrecords.h"

#include <expected>
#include <string>

// The profiler's records and dump for any process with an RPC endpoint (src/profile/README.md, "At runtime"): ngen-view registers
// them with its other records and methods, the asset server with its own.
namespace profile {

// `profile` (frame history, the history window and what it holds) and `zones` (where the time goes per lane and on the GPU).
auto registerProfileRecords(RpcRecords& records) -> void;

// introspect.profile {path, seconds?, level?}: the history as a Chrome trace file; the last `seconds` (default all), level "guide" for
// guide zones only (default everything). Answers {path}.
auto dumpProfile(const rpc::Json& params) -> std::expected<rpc::Json, std::string>;

} // namespace profile
