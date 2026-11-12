# Profile

Where the time goes, on every thread and on the GPU: markers that bracket a piece of work, a built-in collector that keeps the last seconds of
them in memory, and Tracy as a second sink for whole sessions. The library (`profile`) is standard library only apart from Tracy's client and
`config`, and is a foundation library like `trace`: `rpccore`, `trace`, `packer` and the RHI backend link it, so every program does — ngen-view, the asset server,
the packers and the tools.

The built-in collector is the source of every profile number the engine shows or answers with: the Performance window, the `profile` and
`zones` records and `dump-profile`. They work the same with or without Tracy, and in gamerelease. Nothing in the engine reads data back from
Tracy.

## Markers

```cpp
#include "profile.h"

PROFILE_ZONE("BuildSnapshot", Frame);          // scoped CPU zone on the calling thread, guide level
PROFILE_ZONE_DETAIL("ComposeLayer", Scene);    // the same at detail level
PROFILE_ZONE_VALUE(instances.size());          // a number on the innermost open zone: a count, a byte size
PROFILE_FRAME_MARK();                          // once per frame, on the main thread
profile::registerThread("Render");             // names the calling thread's lane

#include "profilegpu.h"

PROFILE_GPU_ZONE(cmd, "ShadowPass", Shadow);         // GPU zone on a command buffer, through the RHI
PROFILE_GPU_ZONE_DETAIL(cmd, "HzbLevel0", Culling);  // only while profile.gpu.detail is on
```

Every marker is a **site**: its name, file, line and function (`std::source_location`), a **group** and a **level**. Two markers with the same
name are two sites, each pointing at its own line. Code that opens and closes a zone in different places calls `profile::beginZone(id)` and
`profile::endZone()` with an id from `profile::registerSite(name, group, level)`; a function that registers for its caller (`FrameGraph::addPass`,
`JobSystem::submit`, `RpcRegistry::add`) takes a `std::source_location` default argument, so the zone points at the caller's line. Names are
copied when registered. Zones nest up to 32 deep per thread.

The site is what Tracy shows as the zone's source (its source view opens the line), and what the `zones` record, the dump and the Performance
window's zone details show as `file:line`. A GPU zone's site travels through the RHI as the zone's tag (`beginGpuZone(name, tag)`).

- **Groups** colour the zones in Tracy and the Performance window: Frame (the main loop's steps), Scene, Assets, Jobs (each job name a shade),
  Ui, Tools, Render, Culling, Geometry, Shadow, Lighting, Post, Upload, Io, and Wait in grey (blocking: fences, snapshots, condition variables,
  idle).
- **Levels**: guide zones are the overview — the frame's stages, each pass — and what the Performance window shows by default; detail zones are
  everything finer. Both are always recorded.

**How deep.** Wherever time goes, a child zone says what it is: a zone that takes 0.05 ms or more per frame on the main or render thread, or 1 ms
or more per call anywhere else, has children covering at least 80% of its time, down to a function that does one thing (a loop over instances,
one root's LOD evaluation) or a library or driver call (`vkQueueSubmit`, freeing a snapshot). The `zones` record gives each zone's `selfMs` and
`coveredShare`, so a gap shows without Tracy; markers are added where it does.

Some zones come by themselves, from a name the engine already has: each frame-graph pass (a CPU record zone and a GPU zone, in the group
`addPass` gives it), each RPC method call (`RpcRegistry::invoke`, group Io), each render extension hook (`RenderTools.beginFrame`, …), each job
(the name given to `JobSystem::submit`), each pack task in the asset server (the rule's name).
The RHI backend's driver calls that can be slow have zones of their own (`RhiCreateBuffer`, `RhiCreateTexture`, `vkAllocateMemory`,
`vkMapMemory`, `RhiDestroyBuffer`, `RhiDestroyTexture`; `src/rhi/README.md`), so a zone that grows a buffer is covered down to the driver.
A thread that blocks on a lock another thread may hold has a Wait zone for the wait (`WaitSceneChangesLock`: the main thread handing the
render thread scene changes), so a frame's time reads as waiting, not working.

## The built-in collector

Each thread writes the zones it finishes into its own lane: blocks of 4,096 zones, appended without a lock. When a block is full the next one
starts, and blocks whose newest zone is older than the history window leave the lane, a block no reader holds being reused. Memory follows the
zone rate; the `profile` record says how much each lane holds. Frames and GPU frames are kept for the same window. Readers copy out from any
thread; a writer waits for a reader only while it starts a block.

- **History**: `profile.history.seconds` (default 10, 1 to 120), a configuration value handed to the collector by the main loop when it changes.
- **Pausing** (the Performance window's Pause) drops new zones so the history stays as it is; zone stacks keep balancing. Tracy keeps recording.
- **Lanes** are taken by a thread's first zone or `registerThread`. When a thread exits its lane is free, and a thread registering the same name
  takes it over, so threads started again and again (the mip chain's workers) keep one lane per name. 64 lanes at most.

### Every thread visible

Every thread an ngen program starts calls `registerThread` (Main, Render, Scene, Worker N, RPC I/O, Asset I/O, Trace I/O; in the asset server
Pack N, File Watcher, Cache Saver; Mip N in the texture packer), which also records a zero-length `ThreadStarted` zone. A thread that idles
waits in slices of `profile::idleSlice` (100 ms), each a detail `Idle` zone (`RpcPoll`, `TraceWait` for the I/O threads), so an idle thread has
zones in the window and a row in Tracy within a slice of anyone looking.

Threads the engine does not start (the driver's, SDL's, Tracy's own) appear only in Tracy's context-switch capture, which needs the kernel's
permission: a lowered `kernel.perf_event_paranoid` (`sudo sysctl kernel.perf_event_paranoid=-1`), and in practice running as root.

## The GPU lane

GPU zones are the RHI's timestamp queries (`beginGpuZone`/`endGpuZone`, `src/rhi/README.md`; up to 1,024 per command buffer). After a frame
slot's fence has signalled, the render thread reads them (`Renderer::readGpuTimings`), moves them onto the CPU clock (calibrated with
`VK_EXT_calibrated_timestamps`, recalibrated every 120 frames; anchored at the submit without it) and hands them in with
`profile::submitGpuZones`, every zone of the frame.

GPU zones stop at commands: timestamps can't split a draw or a dispatch, so a pass's draws are its leaves. The draw zones are named for what they
draw (`Opaque`, `Opaque two-sided`, `Cutout`, `Cutout two-sided`, `DrawLists::drawName`) under the pass, and in the shadow pass under a
`Cascade N` zone per cascade. Detail GPU zones (the depth pyramid's levels, the upload batches) record only while `profile.gpu.detail` is on,
off by default: a timestamp makes the GPU wait for earlier work at that point, so fine zones change the timings they measure.

## Plots

A plot is a value over time, sampled where it is produced, kept for the same history window as the zones and sent to Tracy (`TracyPlot`,
coloured by group):

```cpp
PROFILE_PLOT("Upload bytes", Upload, Bytes, bytes);   // registered once per call site; formats Number, Bytes, Percent
auto id = profile::registerPlot(name, group, format); // a name built at run time, kept and sampled with profile::plot(id, value)
```

ngen-view samples its plots once a frame, last in the frame (`ViewFrame::plotFrame`): the frame times, GPU busy from both sources, resident
memory, the GPU scene's tables and uploads, the frame's commands (draws, dispatches, barriers, pipeline binds, indirect draws, copies,
primitives), each GPU memory heap's usage, budget and allocation, culling (the camera's and each cascade's instances and triangles drawn, its
meshlets tested and culled, when a readback arrives), the scene thread's assets in flight, placements and LOD groups, and the asset client's
requests in flight and bytes received. Each plotted number is also in a record (`gpuscene`, `culling`, `scenethread`, `assets`, `profile`).

- **GPU usage, two sources.** Portable (`profile::gpuBusyPercent`, the plot "GPU busy"): the share of the time between two GPU frames' starts
  that the later frame's top-level zones cover, computed as GPU frames arrive. The driver's (`profile::gpuDriverBusyPercent`, "GPU busy
  (driver)"): amdgpu's `gpu_busy_percent` in sysfs, for the DRM card whose PCI vendor and device IDs are the Vulkan device's
  (`profile::setGpuDevice`), read at most every 100 ms; absent on other drivers, and `gpuBusyDriverSource` in the `profile` record says why.
  They measure different things: the first, how much of the time this process's GPU work fills; the second, the whole GPU over the driver's
  own window, other processes' work included (on Sponza, 75% against 96%).
- **Resident memory** (`profile::residentBytes`): `/proc/self/statm`, read at most every 100 ms.
- **GPU memory in Tracy.** The render thread reports the device's allocations as Tracy's "GPU" memory pool (`profile::memoryAlloc`,
  `memoryFree`, keyed by each allocation's creation sequence), what changed since the last report when the device's allocation serial moved,
  and only while a server is connected; a server that connects later is told every live allocation. Tracy's memory views (and its "GPU" plot of
  the pool's total) show it. The engine keeps no allocation events of its own: the `memory` record lists every allocation.

## Where it sits in the frame

- CPU zones are recorded on the thread that runs them, when they end. `PROFILE_FRAME_MARK` is in the main loop (`src/apps/view.cpp`), so a frame
  is one main-thread iteration; the render thread's work for it follows, overlapping the next main frame.
- GPU zones arrive about two frames after they were recorded, on the render thread, when the slot's fence has passed.
- The asset server and the packers have no frames: their history is by time.

## At runtime

- **Records** (`src/profile/rpc`, the `profilerpc` library, registered by ngen-view and the asset server):
  - `profile`: CPU and GPU time per frame over the history, `historySeconds`, `historyBytes`, `gpuDetail`, the zones and bytes each lane
    holds, `gpuBusyPercent`, `gpuBusyDriverPercent` (null where there is none) and `gpuBusyDriverSource`, and `residentBytes`.
  - `plots`: every plot over the history window with its group, format, sample count, latest, min, average and max.
  - `zones`: every zone site over the last 60 frames (a process without frames: the whole history) per lane and on the GPU, with its file,
    line, function, group, level, count, total, self time (`selfMs`), the share its children cover (`coveredShare`), per frame, median and
    longest time.
- **Methods and verb**: `introspect.plot {name, seconds?}`: one plot's samples (`ms` before now, `value`). `introspect.profile {path, seconds?, level?}` / `dump-profile <path> [seconds] [guide|detail]`: the history as Chrome
  trace JSON (Perfetto, `chrome://tracing`), the last `seconds` (default all), guide zones only or everything (default). An event's `cat` is its
  group; its `args` hold group, level, depth, value, file and line. Zones that overlap the window are included whole, so the first ones start before it.
  Plots are counter events (`"ph": "C"`), one series per plot. The asset server answers both methods too (`ngen-cli introspect call asset introspect.profile '{"path": …}'`).
- **Configuration**: `profile.history.seconds`, `profile.gpu.detail`, `profile.tracy.frameImages`, and `profile.budget.frameMs`: the frame time
  content is measured against (default 16.7 ms, 60 frames a second) (`config` record, `config` verb, Configuration window).
- **Window**: Performance, the main, render, worker and GPU lanes on one clock in their group colours, with frame times, zone details (with the
  site's file, line and function, and self time) and statistics. It shows the guide zones, nested among themselves; Detail shows everything.
  Under the frame times, the guide plots: GPU busy (both sources), resident memory, the device heap's usage and budget, upload bytes.
- **Overlay**: the frame budget overlay (`src/ui/budgetoverlay.*`, View › Frame Budget, Ctrl+B, `ui.overlay.budget`), for content
  developers: the GPU's passes summed by group in plain words (Shadows, Geometry, Lighting, Post-processing, …), each group's passes beneath,
  as bars against `profile.budget.frameMs`, with the whole frame and the GPU frame in the header, green, yellow from 80% and red more than
  5% over budget (a frame locked to the display's rate sits on a budget of that rate), and the frame's draws and triangles (K, M). Its
  numbers come from `profile::gpuPassTimes(60, …)`: each top-level GPU zone's (each pass's) milliseconds per frame over the last 60 frames,
  in the frame graph's recording order, the `zones` record's GPU lane over the same window.

## Tracy

[Tracy](https://github.com/wolfpld/tracy) is a second sink inside the collector, for questions the built-in history cannot answer: a whole
session instead of seconds, statistics and histograms per zone, searching and comparing, sampling and context switches. Every zone (with its
group's colour), zone value, frame mark and thread name goes to Tracy from `beginZone`, `endZone`, `zoneValue`, `frameMark` and
`registerThread`, and every GPU zone from `submitGpuZones`, so nothing that uses the markers knows about Tracy.

- **Builds**: debug and release define `TRACY_ENABLE`, `TRACY_ON_DEMAND`, `TRACY_ONLY_LOCALHOST`, `TRACY_MANUAL_LIFETIME` and
  `TRACY_DELAYED_INIT` (`build.cpp`). In gamerelease the client (`external/tracy/public/TracyClient.cpp`, the `tracy` library) compiles to
  nothing.
- **When it runs**: Tracy's client starts only when a program asks, because its start-up sleeps 200 ms to calibrate its timer and its shutdown
  takes as long again. ngen-view and the asset server call `profile::startTracy()` at launch, which starts it on a background thread; the
  packers and the tools call `profile::startTracyIfAsked()`, which starts it when `NGEN_TRACY=1` is set. Until it has started nothing goes to
  Tracy (zones begun before are not sent); it stops at exit. Running, it listens on loopback and records only while a server is connected; a
  zone then costs about 40 ns more than without Tracy.
- **Several programs**: each running client listens on its own port, the first on 8086 and the next ones after it; `tracy-profiler` lists them
  all. To pick one from the command line, give it a port: `TRACY_PORT=8095 ./ngen-cli view …` and `tracy-capture -p 8095`.
- **Version**: the client is the `external/tracy` submodule, pinned to the tag of the Tracy tools installed on the machine (`tracy-profiler
  --version`). Tracy's server refuses a client with another protocol version; when the tools are updated, move the submodule to their tag.
- **Interactively**: run `tracy-profiler`, pick the program, connect; the timeline fills from that moment. Pausing the Performance window does
  not pause Tracy.
- **Headless**: start `tracy-capture -o run.tracy -f -a 127.0.0.1 -p <port>` first, then the program with that `TRACY_PORT` and
  `TRACY_NO_EXIT=1` (it waits on exit until the capture has everything). `tracy-csvexport run.tracy` gives per-zone statistics; `-u` every CPU
  zone, `-g` every GPU zone. For the everyday numbers, the records above are simpler.
- **GPU lane**: a Tracy GPU context named "GPU" whose clock is the profile clock, re-synchronised against Tracy's own every frame. Because the
  zones are sent when they are collected, Tracy's "CPU to GPU" delay of a GPU zone is meaningless; the zone's place on the GPU lane is right.
- **Frame images**: while a server is connected, every presented frame (UI included) reaches Tracy as a 320-pixel-wide image under the
  main-thread frame that built it (`profile::frameImage`; the render tools make them, `src/rendertools/README.md`). The frame is
  `profile::frameMarks()` when its snapshot was filled; Tracy places an image by an offset from its own frame count, so `frameMark` moves
  Tracy's count and ours under one lock and `frameImage` reads it under the same lock: Tracy fails a capture that gets two images for one
  frame. `profile.tracy.frameImages` (default on) turns them off. The built-in collector keeps no images; `screenshot` is the way to see a
  frame without Tracy.
- **Thread names**: a thread named while Tracy runs gives Tracy its full name; one named before (at launch, while the client is still starting)
  shows under its system name, the first 15 characters, which `registerThread` sets too.
- **Sampling and context switches** start with the client when the kernel lets it open `perf_event`s and read tracefs (see "Every thread
  visible"). `TRACY_NO_SAMPLING=1` and `TRACY_NO_CONTEXT_SWITCH=1` in the environment turn them off.
