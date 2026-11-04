# 5. A frame graph owns pass ordering, culling, barriers and transients

Date: 2026-04-03

## Status

Accepted

Amended by [ADR 27](0027_frame_graph_buffers_imported_only.md): buffers join the graph, imported only.

## Context

`Renderer::render()` was one hard-coded forward pass with hand-written synchronisation. A G-buffer, a lighting pass, shadows, debug passes and the
UI were about to arrive, and retrofitting a graph after them would cost more than building it first. The first phase also moved the renderer from
render-pass objects to dynamic rendering, so a new pass needs no render pass or framebuffer boilerplate.

## Decision

- Each frame, passes are added with `addPass<Data>(name, setup, execute)`. Setup declares what the pass reads and writes.
- `compile()` builds edges (a writer to its readers, consecutive writers in order), sorts the passes with Kahn's algorithm, culls passes that have no
  side effects and whose outputs nothing reads, and computes the lifetime of every transient texture.
- `execute()` takes each transient from a `ResourcePool` keyed by size, format and usage at its first use, derives and records every barrier, and
  returns the transient to the pool after its last use.
- Passes never record barriers for graph resources themselves.

Alternatives considered:

- Real memory aliasing (binding several images to one allocation): deferred as a future optimisation; "aliasing" today is pool reuse within a frame.
- Multi-queue scheduling, priority classes, parallel recording, persistent tokens and early graphs: listed in `frame_graph_next.md`, each gated on a
  "when needed" trigger; none is built. Debug labels per pass and a visual inspector were built.

## Consequences

- New passes slot in without touching the others, and pass order and barriers can be inspected (the Frame Graph window, barrier records).
- One queue only.
- Barriers are computed at execute time rather than compile time, because transient physical resources don't exist until then.
- No transient memory aliasing yet, so transients cost their full size.

## Verification

- Sources: [frame_graph_implementation_plan.md](../frame_graph_implementation_plan.md), [frame_graph_phase1_summary.md](../frame_graph_phase1_summary.md),
  [frame_graph_phase2_summary.md](../frame_graph_phase2_summary.md), [frame_graph_phase3_summary.md](../frame_graph_phase3_summary.md),
  [frame_graph_next.md](../frame_graph_next.md), [src/renderer/README.md](../../src/renderer/README.md) "The frame graph".
- Code:
  - `src/renderer/framegraph.cpp:194` (`compile`), `:240` (Kahn's sort), `:287` (culled passes), `:338` (`execute`).
  - `src/renderer/resourcepool.h:10` (`ResourcePoolKey`), `:21` (`ResourcePool`).
- Differs from plan:
  - Phase 3's "memory aliasing" is pool reuse, not aliased memory.
  - The plan's separate writable texture handle type does not exist in the current headers.
