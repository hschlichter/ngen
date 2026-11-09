# Profile counters: renderer stats, GPU usage and memory over time

**Status. Landed.**

## Current state

The engine's numbers are records, read when asked: `render` (passes, draws, the draw log; built on request), `culling`, `counters` (pipeline
statistics; turned on by asking), `gpuscene` (the GPU scene's change lists, upload queue and geometry pool, from `SceneGpuStats`, which the render
thread publishes after every frame), `memory` (every allocation and heap, from `RhiDevice::allocations` and `memoryHeaps`), `scenethread`,
`assets`, `streaming`. None of them is kept over time: a record is a snapshot. The profile collector keeps zones and frame times only
([plan_profile_detail.md](plan_profile_detail.md) makes its history time-based and configurable). There is no GPU usage figure and no process
memory figure anywhere. Tracy ([ADR 81](adr/0081_tracy_as_second_sink.md)) has plots and a memory view; the engine sends it neither.

## Goals

1. Renderer stats, GPU usage and memory as values over time, in Tracy beside the zones that explain them.
2. **Through the profile system**: the same values kept by the built-in collector for the history window, readable by agents from a record and
   the dump, without Tracy.
3. Every plotted value is also in a record; a value that is new (GPU usage, process memory) is added to one.
4. GPU memory in Tracy's memory view, from the RHI's allocation registry.

## Decisions

Locked with Henrik on 2026-11-09.

1. **`PROFILE_PLOT(name, group, value)` goes to both sinks.** The built-in collector keeps each plot as a time series for the history window,
   beside the zones; Tracy gets `TracyPlot`. Plots are fed once per frame where their numbers are already produced, not sampled by a thread.
2. **GPU usage from two sources.** Portable: the GPU frame's busy time over the frame interval, from data the profiler already has. The driver's:
   amdgpu's `gpu_busy_percent` (sysfs), present on AMD under Linux only; absent elsewhere, and the record says so.
3. **Memory as plots, and the GPU allocations as a Tracy memory pool.** Plots: process resident memory (`/proc/self/statm`) and each GPU heap's
   usage and budget. The GPU allocation registry is reported to Tracy as a named pool ("GPU"), so Tracy's memory views show GPU memory. CPU heap
   tracking through a global `operator new` is deferred: every allocation would pass through Tracy while connected.

## Scope

**In**

- `src/profile/`: plots (macro, time-series history by the history window, records, dump, Tracy), a named memory pool sink.
- The per-frame plots listed in Steps, from `SceneGpuStats`, the frame's command stats, the profiler's frames, the scene thread's stats, the asset
  client, and the two memory sources.
- GPU usage and process memory in the `profile` record.
- The Performance window: a small set of guide plots under the frame-time plot (GPU busy, resident memory, GPU memory, uploads).

**Out**

- CPU heap tracking (Deferred).
- GPU vendor counters beyond amdgpu's busy percentage (NVML, AMD's GPU Perf API).
- Plots in the asset server and the packers: they link `profile` ([plan_profile_detail.md](plan_profile_detail.md)) and can use
  `PROFILE_PLOT`; which of their numbers to plot is left to when a question asks for them.

## Steps

1. **Plots in the collector.** `src/profile/profile.h`:

   ```cpp
   struct PlotSample {
       uint64_t timeNs = 0;
       double value = 0.0;
   };

   auto registerPlot(const char* name, Group group, PlotFormat format) -> uint32_t; // PlotFormat: Number, Bytes, Percent
   auto plot(uint32_t plotId, double value) -> void;
   auto plots(std::vector<PlotInfo>& out) -> void;
   auto plotSamplesIn(uint32_t plotId, uint64_t startNs, uint64_t endNs, std::vector<PlotSample>& out) -> void;

   #define PROFILE_PLOT(name, group, format, value) ... // registers once per call site
   ```

   A plot's samples are kept like zones (blocks by age, the same `profile.history.seconds`). Tracy: `___tracy_emit_plot_config` once with the
   format and the group's colour, `___tracy_emit_plot` per sample.

2. **The frame's command stats.** The renderer adds the frame's totals of `RhiCommandStats` (draws, dispatches, barriers, pipeline binds, indirect
   draws, copies) to `SceneGpuStats` as `frameCommands`. A sum over the frame's command buffer, no readback.

3. **Feeding the plots.** One step, `ViewFrame::plotFrame`, on the main thread after the frame's results are taken (group in brackets):
   - From `SceneGpuStats` (Render, Upload): draw slots, meshes, textures, materials; upload bytes and uploads this frame, queued bytes; geometry
     pool used bytes per buffer; change lists waiting; `frameCommands`.
   - From the profiler (Frame): CPU frame ms, GPU frame ms, GPU busy percent (step 4).
   - From the scene thread's stats (Scene): requests in flight, composes, assets held.
   - From the asset client (Assets): requests in flight, bytes received this frame.
   - Memory (step 5): resident bytes, GPU heap usage and budget per heap.

   - From the culling readback (Culling): instances drawn and culled, meshlets tested and culled by frustum, cone and depth, triangles drawn, for
     the camera and each cascade. The readback already runs every frame (`view.latestCull`, the render thread's `ParseReadback` zone), a few
     frames late; the plot's sample carries the frame the counts belong to.

4. **GPU usage.** `src/profile/gpuusage.cpp`:
   - Portable: per CPU frame, the union of that frame's GPU zone intervals (top level) over the frame interval, in percent. Computed in the
     collector when a GPU frame arrives.
   - amdgpu: `/sys/class/drm/card*/device/gpu_busy_percent` for the card whose `vendor` and `device` files match the Vulkan device's vendor and
     device IDs (core Vulkan, added to `RhiDeviceLimits`; no extension), read at most every 100 ms on the main thread. Absent, or several cards
     match: the value is not plotted, and the record says why.

5. **Memory.**
   - Process: `/proc/self/statm` resident pages × page size, read at most every 100 ms.
   - GPU heaps: `RhiDevice::memoryHeaps` on the render thread after the fence, published in `SceneGpuStats` as `heaps`.
   - GPU allocations in Tracy: the render thread compares the allocation registry with what it last reported when the registry's change
     counter moved (`RhiDevice::allocationSerial()`, new) and reports the difference: `profile::memoryAlloc("GPU", id, bytes)` and
     `profile::memoryFree("GPU", id)` → `___tracy_emit_memory_alloc_named` / `_free_named`. The built-in collector keeps no allocation events;
     the `memory` record already lists every allocation.

6. **Readers.**
   - New record `plots`: every plot with its group, format, latest value, and min/average/max over the history; `plots {name}` returns one
     plot's samples.
   - `profile` record gains `gpuBusyPercent` (portable), `gpuBusyDriverPercent` (null when absent), `residentBytes`.
   - `dump-profile` writes plots as Chrome counter events (`"ph": "C"`), within the same `seconds`.

7. **Performance window.** Under the frame-time plot: GPU busy (both sources), resident memory, GPU memory against budget, upload bytes per
   frame. The rest is in Tracy and the `plots` record.

8. **Docs.** `src/profile/README.md` (plots, GPU usage sources, memory pool), `src/renderer/README.md` (`frameCommands` and `heaps` in
   `SceneGpuStats`), `src/rhi/README.md` (`allocationSerial`, the vendor and device IDs).

## Built

Where the code differs from the steps above:

- **System readings** are in `src/profile/systemusage.cpp` (the driver's GPU busy figure and resident memory), not `gpuusage.cpp`; the portable
  GPU busy figure is computed in `submitGpuZones` and plotted there.
- **The frame's commands** come from `FrameGraph::frameStats()` (the passes that ran, summed), so their draws match the `render` record's per-pass
  draws, indirect draws included.
- **The GPU's allocations reach Tracy only while a server is connected**: `Renderer::reportGpuAllocations` reads `allocations()` when
  `allocationSerial()` moved and a server is connected, and forgets what it reported when none is, so a server that connects later is told every
  live allocation. Reading every allocation each frame without a server would cost for nothing.
- **Culling plots are sampled when a readback arrives**, not every frame; the others once a frame.
- **`introspect.plot {name, seconds?}`** is the method for one plot's samples (the asset server answers it too); the `plots` record lists them.
- **The heaps** are in `SceneGpuStats` and the `gpuscene` record (`heaps`), with the frame's commands (`frameCommands`).

## Visibility

- **Records**: `plots` (new), `profile` (GPU usage, resident memory), `gpuscene` (`frameCommands`, `heaps`). The culling plots' numbers are the
  `culling` record's.
- **RPC methods and verbs**: the `plots` record's method; `dump-profile` includes plots.
- **Windows**: the Performance window's guide plots.
- **Tracy**: every plot, coloured by group; the "GPU" memory pool.

## Verification

Results (2026-11-09, release, this machine) in **bold**.

- Headless, release, Sponza, `record plots` and `record profile` at frame 600:
  - Every plot listed in step 3 is present, with a sample per frame over the history window (count within 1% of the frames in it). **47
    plots; 2,497 samples over 2,497 frames each; the culling plots 2,391, sampled per readback.**
  - `frameCommands.draws` equals the sum of the `render` record's per-pass draws on the same frame (recorded with `render` at that frame).
    **619 and 619.**
  - The camera's instances-drawn plot at frame 600 equals the `culling` record's `camera.drawn` for the same readback frame; flying the camera
    through Sponza (a `camera` verb every 60 frames) makes it change between samples. **173 and 173; the plot's min and max over the flight
    of the streaming test world differ (not run as a Sponza camera script).**
  - Live view: `introspect get view profile` and `/proc/<pid>/status` `VmRSS` read right after it agree within 5%. **1.8% apart.**
  - On this machine (amdgpu) `gpuBusyDriverPercent` is a number; the portable `gpuBusyPercent` is within 15 points of it over 600 frames.
    **Not met, and the criterion was wrong: 96% (driver, `/sys/class/drm/card1/device/gpu_busy_percent`) against 75% (portable). The GPU
    frame's whole span, not only its zones, is 3.0 ms of a 4.0 ms frame — the same 75%. The driver's figure is the whole GPU over its own
    window, other processes' work included; the portable one is this process's. The README says what each measures.**
- A Tracy capture has every plot (`tracy-csvexport -u -p` lists them) and the GPU pool's total equals the `memory` record's allocated total at
  the end of the run (±0). **All 47 plots. The pool's last total (Tracy's "GPU" plot) is 2,745,250,008 bytes, the heaps' allocated totals
  added up 2,745,250,008.**
- The GPU heap usage plot matches the `memory` record's heaps on the same frame. **The heap plots come from the same `memoryHeaps` call the
  `gpuscene` record's `heaps` do; heap 1 (device): 2.77 GB used of a 10.8 GB budget, 2.69 GB allocated by the engine.**
- Trace: no warnings or errors. The Performance window's plots: Henrik's visual check. **Checked by Henrik. Debug three_cubes run: no warnings or errors,
  validation clean, 47 plots. All 17 RHI examples exit 0. Cost: `PlotFrame` 7 µs a frame (median; 0.4 ms when the 100 ms readings are
  taken), `MemoryHeaps` 8 µs.**

## Deferred / follow-ups

- **CPU heap tracking** (global `operator new`/`delete` in ngen-view, `TracyAlloc`/`TracyFree`, and allocation totals in the collector). Trigger:
  a memory growth question the resident plot can't answer.
- **Vendor GPU counters** (occupancy, bandwidth). Trigger: a GPU-bound pass the zones and pipeline statistics can't explain.
