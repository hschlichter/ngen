# 47. A trace library: always on, one clock, streamed over RPC

Date: 2026-10-03

## Status

Accepted

Supersedes [ADR 10](0010_observation_bus.md)

## Context

The observation bus ([ADR 10](0010_observation_bus.md)) was off unless `--obs-output` was given at launch, so nothing could be looked at after the
fact, and a tool could not attach to a running process and see what had happened. Its timestamps counted from each process's start, so two
processes' events could not be merged into one timeline. Only ngen-view used it; the asset server printed unstructured text. The new introspection
tool ([ADR 46](0046_records_and_ngen_introspect.md)) needed traces from every process, and keeping the bus beside a new trace would have meant two
event systems.

## Decision

- `src/trace/` replaces the bus. Events are emitted with `TRACE_EVENT(category, type, name)` and `.field()`/`.text()`.
- Each process keeps an always-on ring of the most recent 65,536 events in memory.
- Timestamps are `CLOCK_MONOTONIC` nanoseconds, which every process on the machine shares.
- A subscriber calls `trace.subscribe` and receives `trace.events` batches every 50 milliseconds or every 1,024 events. At most 16 megabytes are
  queued per subscriber; past that, nothing more is sent and overwritten events are counted as dropped, so a frame never stalls on a slow reader.
- The ring is flushed to subscribers for up to a second on exit.
- Every process uses it, the asset server included. The JSON shape of an event stays the bus's, with a `process` field added.
- Agents verify through `ngen-introspect trace` and `get` instead of observation files.

## Consequences

- Easier: a tool can attach at any time and see recent history; processes merge into one timeline.
- Easier: one event system for every process.
- Harder: every event is always built, so `.field()` arguments must be cheap to compute. The cost measured on Sponza in a debug build was within
  noise (8.24 to 8.27 milliseconds per frame before, 8.24 to 8.39 after); a CPU-bound comparison was never run.
- Harder: a long run before a tool attaches loses its oldest events.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_introspect_tool.md](../plan_introspect_tool.md) (phase B), [src/trace/README.md](../../src/trace/README.md), `AGENTS.md`
  ("Verifying changes").
- Code: `src/trace/tracering.h:18` (`capacity = 65536`), `src/trace/traceevent.h:31` (`CLOCK_MONOTONIC`), `src/trace/tracering.cpp:25`,
  `src/trace/tracestream.h:24` (1,024 events, 50 milliseconds, 16 megabytes), `src/asset/server/assetserver.h:11` (the server includes the trace
  stream), `build.cpp:117` (the `trace` library, linked by `rpc` at `:125`).
- `src/obs/` no longer exists; `rg "OBS_EVENT|obs-output" src` finds nothing.
- Differs from plan: none in the code. `docs/README.md` still calls the observability plan "landed" and points to `obs.md`, which is gone.
- Rejected alternative: keeping the bus beside the trace ("one tool for events instead of two"); off by default.
