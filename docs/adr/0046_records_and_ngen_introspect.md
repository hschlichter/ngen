# 46. Records on every endpoint, and `ngen-introspect`

Date: 2026-10-03

## Status

Accepted

## Context

A running process's data was reached in three ways that didn't line up: bespoke RPC methods, `--dump-*` flags that wrote files, and windows built per
data type. A command-line RPC client (`ngen-rpc`) and a planned introspection tool overlapped. Nothing showed more than one process: the view and the
asset server were looked at separately.

The tool architecture's step 4 ([ADR 34](0034_process_split.md)) planned to move ngen-view's introspection windows into `ngen-introspect`, with
views connecting to an open tool.

## Decision

- Every endpoint serves `introspect.list` and `introspect.get`: a table of named JSON records (`RpcRecords`, in the RPC core). A record is for
  reading only; anything that changes state is a method.
- `ngen-introspect` is one tool with a window (SDL's renderer and Dear ImGui, no Vulkan) and a command line: `list`, `get`, `describe`, `call` and
  `trace`. It finds processes through their discovery files ([ADR 36](0036_one_endpoint_per_process.md)) and connects to them; processes know
  nothing of the tool. `ngen-rpc` is folded into it.
- ngen-view keeps its own windows: data is best looked at where it is used. GPU-native data (buffers, textures, captures) stays in the view.
- A record at an exact frame comes from a script's `record <name> <file>` (the `introspect.record` method), appended to a file.

## Consequences

- Easier: new data and new processes show up in the tool without changing the tool; one generic JSON viewer serves every record.
- Easier: agents and humans read the same records the same way; verification runs through the tool.
- Harder: the asset server has no `rpc.describe`, so its methods are not self-describing.
- Harder: the tool doesn't reconnect when a process restarts, and table cells holding objects show compact JSON that can't be expanded.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_introspect_tool.md](../plan_introspect_tool.md), [plan_tool_architecture.md](../plan_tool_architecture.md) (step 4, decision 8),
  [plan_rpc.md](../plan_rpc.md), [src/introspect/README.md](../../src/introspect/README.md).
- Code: `src/rpc/rpcregistry.cpp:162` (`registerRpcRecords`: `introspect.list` and `introspect.get`), `src/rpc/core/rpcrecords.h`,
  `src/view/viewcommands.cpp:44` (`introspect.record`) and `:81` (the `record` verb), `src/introspect/introspectsession.cpp:77`
  (`trace.subscribe`), `build.cpp:490` (the `introspect` library links only `rpccore` and `imgui`), `build.cpp:497` (`ngen-introspect`).
- Differs from plan: the umbrella plan's move of the view's windows into the tool, and views connecting to the tool, were both reversed. The planned
  `ngen-rpc` program was deleted; [plan_tool_architecture.md](../plan_tool_architecture.md) still names it.
- Rejected alternatives: a window per data type; a Vulkan device in the tool.
