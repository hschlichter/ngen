# 24. One verb set for flags and scripts: headless sessions

Date: 2026-09-20

## Status

Accepted

Amended by [ADR 35](0035_json_rpc_over_loopback_tcp.md) and [ADR 36](0036_one_endpoint_per_process.md): every verb became a parser onto an RPC
method, so scripts and live calls run the same code.

Amended by [ADR 46](0046_records_and_ngen_introspect.md): the CPU-side `--dump-*` flags became records, written with `record <name> <file>`.

## Context

Humans had windows; agents had the observation bus's JSONL stats and grep. An agent could not see a frame, place the camera, choose a debug view or
read the Render Debug and Performance windows. Every check an agent wanted meant a new flag with its own code path, and a sequence of actions —
move, wait, capture — could not be expressed at all.

## Decision

A fixed set of session verbs — `camera`, `select`, `view`, `overlay`, `screenshot`, the `dump-*` verbs, `quit` — is shared by command-line flags and
`--script` files:

- A script is lines of `<frame> <verb> [args]`; a flag is the same verb at frame 0. There is one implementation per verb.
- Screenshots are read from the backbuffer, so they show what a user sees, not an intermediate target.
- Camera bookmarks live in a text file next to the scene, not in USD: they are viewer state, not scene content.
- The profile dump uses the Chrome trace format.

Controlling a running session over a socket was deferred; RPC later delivered it (ADR 35).

## Consequences

- Easier: every run is reproducible headless, which made "run with a script, read the output" the project's verification loop.
- Easier: once verbs mapped onto RPC methods, flags, scripts and live calls could not diverge.
- Harder: verbs remain as short aliases that must each be kept in step with a method's parameters.
- Harder: the verb list in comments drifts from the registered verbs (see Verification).

## Verification

- Sources: [plan_agent_introspection.md](../plan_agent_introspection.md), [plan_rpc.md](../plan_rpc.md), [plan_introspect_tool.md](../plan_introspect_tool.md).
- Code: `src/rpc/rpcverbs.h:14`–`15` (verbs turned into a method's parameters, added per layer), `:24` (failures are `ScriptCommandFailed` trace
  errors); `src/view/viewcommands.cpp:81` (`record` → `introspect.record`).
- Differs from plan: `--dump-render-debug` and `--dump-memory` are gone, replaced by records. `src/session/sessionscript.h:15` still documents
  `record NAME` as putting a record "into the trace as one event"; it now appends to a file, and records never enter the trace (ADR 48).
