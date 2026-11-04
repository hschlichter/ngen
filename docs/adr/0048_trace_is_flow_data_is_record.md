# 48. The trace is flow and messages; data is a record

Date: 2026-10-06

## Status

Accepted

Supersedes [ADR 10](0010_observation_bus.md)

## Context

Moving the observation bus into the always-on trace ([ADR 47](0047_trace_library.md)) carried its per-frame narration along: `PassExecuted`,
`FrameEnd`, `RenderStats`. In an always-on ring that became a constant stream, which pushed out everything worth reading. Engine diagnostics —
Vulkan validation messages, USD warnings, failures — went to stderr outside any structure, so the console and the trace told different stories. The
plan also had records enter the trace as `Record` events.

## Decision

- The trace is for flow and messages: what happened, and what went wrong. Data that can be asked for is a record
  ([ADR 46](0046_records_and_ngen_introspect.md)), never an event.
- Events have a level and a message: `TRACE_EVENT` (info), `TRACE_WARNING`, `TRACE_ERROR`, each with `.text()`.
- Nothing is traced per frame; a steady state is silent. Start-up reads as a timeline.
- Records never enter the trace. The `record` verb appends a record to a file; the `Record` category and `introspect.trace` were removed.
- Every event is printed with the local time — info on stdout, warnings and errors on stderr — and engine code prints nothing else, so the console
  and the trace never differ.
- RHI messages arrive through `RhiDeviceOptions::onMessage`; OpenUSD diagnostics through a `TfDiagnosticMgr` delegate.
- Events added for a change stay in the code; there is no "remove when done" step.

## Consequences

- Easier: a clean run of the view has about eleven events, so "no warnings or errors in the trace" is a binary verification check.
- Easier: the console is the trace, for humans and agents alike.
- Harder: every author has to judge whether something is data or flow.
- Harder: data over time needs `record` at chosen frames; polling a record repeatedly (`get --every`) is deferred.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_introspect_tool.md](../plan_introspect_tool.md) ("Phase B as built"), [src/trace/README.md](../../src/trace/README.md), `AGENTS.md`
  ("Verifying changes").
- Code: `src/trace/trace.h:10` (the macros and the flow-not-data rule), `src/trace/trace.h:71` (`TRACE_WARNING`, `TRACE_ERROR`),
  `src/trace/tracering.cpp:64` (printing by level to stdout and stderr), `src/rhi/rhidevice.h:20` (`onMessage`), `src/view/viewcommands.cpp:81`
  (`record` writes to a file).
- Differs from plan: the `TfDiagnosticMgr` delegate now lives only in the unbuilt editor code (`src/editor/scene/usddiagnostics.cpp`), since the
  view links no OpenUSD. `src/session/sessionscript.h:15` still documents `record NAME` as putting a record into the trace.
- Rejected alternatives: records as `Record` events in the trace (planned, reversed); per-frame statistics events.
