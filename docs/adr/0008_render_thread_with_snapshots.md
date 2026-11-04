# 8. A render thread fed by snapshots, one-slot back-pressure

Date: 2026-04-12

## Status

Accepted

Amended by [ADR 54](0054_async_scene_and_incremental_gpu_scene.md): the scene reaches the render thread as incremental `SceneChanges`, not as a
whole uploaded `RenderWorld`.

## Context

The main thread's update, UI and command recording ran one after another. Recording waits on fences and on swapchain acquire, so the main thread
stalled on the GPU. ImGui is single-threaded and must stay on the main thread.

## Decision

- A dedicated `std::jthread` render thread owns the GPU: it waits on the frame slot's fence, acquires, builds and executes the frame graph, submits and
  presents. The main thread never records or submits.
- The main thread hands over a self-contained `RenderSnapshot` through a single slot guarded by a mutex and two condition variables. This is
  back-pressure, not a queue: the main thread is at most one frame ahead.
- ImGui draw data is deep-copied into the snapshot.
- Results come back latest-only and without blocking (debug snapshots, cull results).
- There is one frame slot per swapchain image.

Alternatives considered:

- Running rendering as jobs on the job system: rejected, because blocking on fences would tie up a worker thread.

## Consequences

- The CPU work of two frames overlaps, and ownership is clear: only the render thread touches the GPU.
- Everything the CPU shows about the GPU arrives a frame-slot cycle late.
- The snapshot grows with every feature; it now also carries cascades, LOD and streaming state.

## Verification

- Sources: [plan_threaded_rendering.md](../plan_threaded_rendering.md), [src/renderer/README.md](../../src/renderer/README.md) "Threads and handoff".
- Code:
  - `src/renderer/renderthread.h:100` (`std::jthread thread`), `:104-107` (the two condition variables and `hasSnapshot`).
  - `src/renderer/rendersnapshot.h:28` (`struct RenderSnapshot`).
  - `src/renderer/renderworld.h:184` (`SceneChanges`).
- Differs from plan:
  - The plan's `RenderUpload` channel (a whole `RenderWorld` plus shared libraries) was replaced by `SceneChanges`. The diagram in
    `src/renderer/README.md:77` still shows `--RenderUpload-->`.
