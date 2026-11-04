# 27. Frame-graph buffers are imported only

Date: 2026-09-25

## Status

Accepted

## Context

GPU-driven rendering (ADR 26) puts persistent tables in buffers — the instance buffer first. Frame N+1's upload into such a buffer must wait for
frame N's reads of it, so the frame graph needs to know the buffer's access across frames, not only within one. The graph so far knew only textures.

A pooled transient buffer reused next frame would also need synchronising against the previous frame's reads, which the graph had no way to see.

## Decision

- Buffers share the graph's `resources` list with textures, tagged by kind, so one dependency and barrier path serves both.
- A buffer enters the graph only by `importBuffer(name, buffer, desc, initialAccess)`. After `execute`, `finalAccess(handle)` returns the access it was
  left in; the renderer stores that beside the buffer and passes it as `initialAccess` next frame.
- Per-frame-slot buffers import with no prior access, because the slot's fence already orders them.
- There are no transient buffers.

Rejected: a separate buffer list (duplicates the dependency code); a graph-internal map keyed by buffer address (invisible, and stale when an address
is reused). Transient buffers were planned for GPU culling, then deferred until a variable-size per-frame buffer or memory pressure needs them.

## Consequences

- Easier: cross-frame barriers are correct and visible in the graph's barrier records.
- Easier: buffers and textures read the same in the Frame Graph window.
- Harder: every persistent buffer carries one stored access field that its owner must pass back each frame.
- Harder: per-slot copies of the culling buffers use more memory than transients would.

## Verification

- Sources: [plan_frame_graph_buffers.md](../plan_frame_graph_buffers.md), [plan_gpu_culling.md](../plan_gpu_culling.md) decision 3,
  [src/renderer/README.md](../../src/renderer/README.md) "Buffers".
- Code: `src/renderer/framegraph.h:65` (`importBuffer`), `:67` (`finalAccess`); `src/renderer/framegraph.cpp:47` and `:494` (final access carried to
  the next frame); `src/renderer/framegraphresource.h:51` (`FgResourceKind`).
