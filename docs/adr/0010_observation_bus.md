# 10. An observation bus (JSONL) for agent verification

Date: 2026-04-22

## Status

Superseded by [ADR 47](0047_trace_library.md) and [ADR 48](0048_trace_is_flow_data_is_record.md)

## Context

The engine had become multi-threaded and asynchronous, with USD edits on fast and slow paths, and "run it and look at it" no longer showed whether a
change worked. There was no logging, only scattered prints. The main consumer was an AI agent that needed machine-readable evidence that a change did
what it was meant to.

An earlier abstract design proposed per-frame report containers, resource-state and execution-summary types, and a verifier with an expectation
language.

## Decision

- One flat `Observation` type — category, type, name, fields — emitted with `OBS_EVENT` onto a thread-safe bus.
- The bus streams through a lock-free queue and a writer thread to a sink (JSON lines in a file). Nothing is kept in memory.
- It is off unless `--obs-output` is given; with no sink, every `OBS_EVENT` costs nothing.
- Categories are fixed at start-up; timestamps count from the process's own start.
- Scene objects are named by USD prim paths, never by handles.

Alternatives considered:

- Per-frame report containers and grouping scopes: rejected; each subsystem has its own idea of scope, so scope became an ordinary field.
- Resource-state and execution-summary types: folded into ordinary observations.
- A verifier and predicate language: rejected — "Claude is the verifier".
- Keeping events in memory: rejected in favour of streaming.

## Consequences

- Free when off, and agents got their first structured evidence loop.
- Events existed only if the flag was set at launch; nothing could be looked at afterwards.
- Per-process clocks meant two processes' events could not be merged.
- Per-frame narration (passes executed, render stats) mixed data into the event stream.
- Only ngen-view used it; the asset server printed text lines. These limits led to the trace library ([ADR 47](0047_trace_library.md)).

## Verification

- Sources: [observability_api_design_engine_agnostic.md](../observability_api_design_engine_agnostic.md),
  [plan_observability.md](../plan_observability.md), [plan_render_observations.md](../plan_render_observations.md).
- Code: `src/obs/` no longer exists, and nothing in `src/` mentions `OBS_EVENT` or `--obs-output`.
- Differs from plan:
  - [docs/README.md](../README.md) still annotates `plan_observability.md` as landed, with a usage reference to `obs.md`, which is gone; it is not
    marked superseded.
