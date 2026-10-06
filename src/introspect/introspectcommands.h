#pragma once

#include <span>
#include <string>

// ngen-introspect's command line, for agents and scripts. Every command is an RPC call on the processes found
// through .ngen-discovery/ in the working directory:
//
//   list                                 every live process with its records
//   get <target> <record>                one record as JSON (introspect.get)
//   describe <target>                    the target's methods with parameter schemas (rpc.describe)
//   call <target> <method> [params]      call a method; params is a JSON object
//   trace [options]                      every process's trace events as JSON lines (introspecttrace.h)
//
// Output is JSON on stdout. Returns the exit code: 0 success, 1 the call returned an error, 2 no such endpoint or it
// can't be reached, 3 bad usage.
auto runIntrospectCommand(std::span<const std::string> args) -> int;

// True when `command` is one of the above.
auto isIntrospectCommand(const std::string& command) -> bool;
