# 23. Profiling markers as their own system, ready for Tracy

Date: 2026-09-20

## Status

Accepted

## Context

There was no CPU timing, so it was unclear whether the main thread, the render thread or the GPU limited a frame. The only instrumentation was the
observation bus (ADR 10), an event log: one event per zone per frame would flood it, and it had no notion of thread lanes or GPU time. The frame
graph had its own timestamp query pools, which could not nest.

Main and render frames overlap by design (ADR 8), so seeing a frame means seeing three lanes — main, render, GPU — on one timeline.

## Decision

Profiling is its own library, `src/profile/`, separate from any event log:

- Own macros: `PROFILE_ZONE`, `PROFILE_ZONE_VALUE`, `PROFILE_FRAME_MARK` and `PROFILE_GPU_ZONE`. Zone names are interned once per call site.
- A built-in collector keeps completed zones in a lock-free ring per thread: 16,384 zones per lane and 1,024 frames of history.
- GPU zones go through the RHI (`beginGpuZone`/`endGpuZone`), replacing the frame graph's own query pools, so zones nest.
- The GPU clock is aligned to the CPU's with `VK_EXT_calibrated_timestamps` (`calibrateGpuClock`); a separate GPU time axis shipped first and was
  replaced.
- A Performance window draws the three-lane timeline; `dump-profile` writes the history as Chrome trace JSON.
- The macros are shaped so that Tracy can later be a second expansion behind them.

Rejected: Tracy's macros directly (Tracy shows nothing inside the engine's own UI), raw query pools in the renderer (no nesting), and carrying zones
in the render snapshot (the render thread's zones would lag a frame).

## Consequences

- Easier: cheap enough to leave on in every build, and the trace (ADR 47) stays free of timing data.
- Easier: a frame's main, render and GPU work can be read on one aligned timeline.
- Harder: a second instrumentation channel beside the trace. Profiler zones are not on the trace stream, and the profile history leaves the
  process only as a file (`dump-profile`), not as a record.
- Harder: Tracy has not been integrated; the "Tracy-ready" shape is untested.

## Verification

- Sources: [plan_performance_window.md](../plan_performance_window.md), [plan_gpu_timestamps.md](../plan_gpu_timestamps.md).
- Code: `src/profile/profile.h:121` (`PROFILE_ZONE`), `:125`–`:126`, `:25` (16,384 zones per lane), `:27` (1,024 frames);
  `src/profile/profilegpu.h:21` (`PROFILE_GPU_ZONE`); `src/rhi/rhicommandbuffer.h:46` (`beginGpuZone`); `src/rhi/rhidevice.h:60` (`calibrateGpuClock`).
- Differs from plan: no Tracy integration exists (`rg -i tracy src` finds only comments).
