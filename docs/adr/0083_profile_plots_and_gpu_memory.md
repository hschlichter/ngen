# 83. Profile plots, GPU usage from two sources, GPU memory as a Tracy pool

Date: 2026-11-09

## Status

Accepted

Extends [ADR 81](0081_tracy_as_second_sink.md) and [ADR 82](0082_detailed_profiling.md), which stay Accepted.

## Context

With detailed zones in place ([ADR 82](0082_detailed_profiling.md)), the profiler showed where time goes but not the numbers that explain it:
renderer stats, GPU usage and memory were in records as single values, not over time, and Tracy had none of them. Henrik wanted them as graphs
going up and down beside the zones, with everything still going through the profile system and readable by agents without Tracy.

## Decision

Locked with Henrik on 2026-11-09:

- **`PROFILE_PLOT(name, group, format, value)` goes to both sinks.** The built-in collector keeps each plot as a time series for the history
  window (`profile.history.seconds`), read through the `plots` record, `introspect.plot` and `dump-profile` (counter events); Tracy gets a plot
  coloured by its group. Plots are fed once a frame where their numbers are already produced (`ViewFrame::plotFrame`, the last step of the
  frame), not sampled by a thread.
- **GPU usage from two sources, each saying what it measures.** Portable: the share of the time between GPU frames that this process's GPU
  zones fill, from data the profiler already has. The driver's: amdgpu's `gpu_busy_percent` from sysfs, for the card whose PCI vendor and device
  match the Vulkan device; the whole GPU, other processes included. Absent elsewhere, and the `profile` record says why.
- **Memory as plots, and the GPU allocations as a Tracy memory pool.** Process resident memory (`/proc/self/statm`) and each GPU heap's usage,
  budget and allocated total are plots. The RHI's allocation registry is reported to Tracy as the "GPU" pool, only while a server is connected,
  by diffing when `RhiDevice::allocationSerial()` moves.

Rejected: plots in Tracy only (not readable without it); a sampling thread (values without the frame they belong to); one GPU busy figure (the
portable one is everywhere but blind to other processes, the driver's is exact but amdgpu only); CPU heap tracking through a global
`operator new` (every allocation would pass through Tracy while connected; deferred until a memory question the resident plot can't answer).

## Consequences

- Easier: a frame's spike in Tracy sits beside the draws, uploads, culling results and memory that explain it; agents read the same series from
  `plots` and `introspect.plot`.
- Easier: Tracy's memory view shows GPU memory by allocation, totalling the heaps' allocated bytes.
- Harder: two GPU busy figures that disagree (75% and 96% on Sponza) need their meanings read; the README states them.
- Harder: a plot table of 512 fixed slots; plotting costs about 7 µs a frame in the view.

## Verification

- Sources: [plan_profile_counters.md](../plan_profile_counters.md), `src/profile/README.md` ("Plots").
- Code:
  - `src/profile/profile.h:275` (`PROFILE_PLOT`), `:176` (`registerPlot`), `:185` (`gpuBusyPercent`), `:190` (`residentBytes`), `:194`
    (`memoryAlloc`).
  - `src/profile/profile.cpp:690` (`registerPlot`), `:710` (`plot`), `:666` (portable GPU busy in `submitGpuZones`).
  - `src/profile/systemusage.cpp` (sysfs card match, resident memory).
  - `src/rhi/rhidevice.h:89` (`allocationSerial`); `src/renderer/renderer.cpp:553` (`reportGpuAllocations`).
  - `src/view/viewframe.cpp:456` (`plotFrame`); `src/ui/performancewindow.cpp:343` (`drawGuidePlots`).
- Differs from plan: the system readings are in `systemusage.cpp`, not `gpuusage.cpp`; culling plots are sampled per readback, not per frame;
  the plan's criterion that the two GPU busy figures agree within 15 points was wrong, since they measure different things.
