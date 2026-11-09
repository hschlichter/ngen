# Profile

Where the time goes, on every thread and on the GPU: markers that bracket a piece of work, a built-in collector that keeps the last few seconds
of them in memory, and Tracy as a second sink for when that is not enough. The library (`profile`) is standard library only apart from Tracy's
client, and sits beside `trace` and `config`: any library may profile, and nothing in it knows the engine.

The built-in collector is the source of every profile number the engine shows or answers with: the Performance window, the `profile` and
`zones` records and `dump-profile`. They work the same with or without Tracy, and in gamerelease. Nothing in the engine reads data back from
Tracy.

## Markers

```cpp
#include "profile.h"

PROFILE_ZONE("BuildSnapshot");          // scoped CPU zone on the calling thread
PROFILE_ZONE_VALUE(instances.size());   // a number on the innermost open zone: a count, a byte size
PROFILE_FRAME_MARK();                   // once per frame, on the main thread
profile::registerThread("Render");      // names the calling thread's lane

#include "profilegpu.h"

PROFILE_GPU_ZONE(cmd, "ShadowPass");    // scoped GPU zone on a command buffer, through the RHI
```

Names are static strings, interned once per call site (`registerName`). Code that opens and closes a zone in different places calls
`profile::beginZone(id)` and `profile::endZone()` with an id it registered once. Zones nest up to 32 deep per thread.

## The built-in collector

Each thread writes the zones it finishes into its own lock-free ring (16,384 zones per lane); the main thread's frame marks keep 1,024 frames of
history. No locks and no allocation on the hot path, so the markers stay in every build. Readers copy out from any thread without blocking the
writers. Pausing (the Performance window's Pause) drops new zones so the history stays as it is; zone stacks keep balancing.

## The GPU lane

GPU zones are the RHI's timestamp queries (`beginGpuZone`/`endGpuZone`, `src/rhi/README.md`). After a frame slot's fence has signalled, the
render thread reads them (`Renderer::readGpuTimings`), moves them onto the CPU clock (calibrated with `VK_EXT_calibrated_timestamps`, recalibrated
every 120 frames; anchored at the submit without it) and hands them in with `profile::submitGpuZones`. The built-in history keeps up to 64 GPU
zones per frame.

## Where it sits in the frame

- CPU zones are recorded on the thread that runs them, when they end. `PROFILE_FRAME_MARK` is in the main loop (`src/apps/view.cpp`), so a frame
  is one main-thread iteration; the render thread's work for it follows, overlapping the next main frame.
- GPU zones arrive about two frames after they were recorded, on the render thread, when the slot's fence has passed.

## At runtime

- **Records**: `profile` (CPU and GPU time per frame over the history), `zones` (every zone over the last 60 frames per lane and on the GPU:
  count, total, per frame, median, longest).
- **Verb and method**: `dump-profile <path>` / `introspect.profile {path}`, the whole history as Chrome trace JSON (Perfetto, `chrome://tracing`).
- **Window**: Performance, the main, render, worker and GPU lanes on one clock, with frame times, zone details and statistics.

## Tracy

[Tracy](https://github.com/wolfpld/tracy) is a second sink inside the collector, for questions the built-in history cannot answer: a whole
session instead of a few seconds, statistics and histograms per zone, searching and comparing, sampling and context switches. Every zone, zone
value, frame mark and thread name goes to Tracy from `beginZone`, `endZone`, `zoneValue`, `frameMark` and `registerThread`, and every GPU zone
from `submitGpuZones`, so nothing that uses the markers knows about Tracy.

- **Builds**: debug and release define `TRACY_ENABLE`, `TRACY_ON_DEMAND` and `TRACY_ONLY_LOCALHOST` (`build.cpp`). The client listens on
  loopback and records nothing until a Tracy server connects; a zone then costs about 30 ns more than without Tracy. In gamerelease the client
  (`external/tracy/public/TracyClient.cpp`, the `tracy` library) compiles to nothing.
- **Version**: the client is the `external/tracy` submodule, pinned to the tag of the Tracy tools installed on the machine (`tracy-profiler
  --version`). Tracy's server refuses a client with another protocol version; when the tools are updated, move the submodule to their tag.
- **Interactively**: run `tracy-profiler`; a running ngen-view is listed under its program name. Connect, and the timeline fills from that moment.
  Pausing the Performance window does not pause Tracy.
- **Headless**: start `tracy-capture -o run.tracy -f -a 127.0.0.1` first, then the view with `TRACY_NO_EXIT=1` (the process waits on exit until
  the capture has everything). `tracy-csvexport run.tracy` gives per-zone statistics; `-u` every CPU zone, `-g` every GPU zone. For the everyday
  numbers, the records above are simpler.
- **GPU lane**: a Tracy GPU context named "GPU" whose clock is the profile clock, re-synchronised against Tracy's own every frame. Tracy gets every
  GPU zone, not only the 64 per frame the built-in history keeps. Because the zones are sent when they are collected, Tracy's "CPU to GPU" delay
  of a GPU zone is meaningless; the zone's place on the GPU lane is right.
- **Source locations**: a zone's source location in Tracy is its name only, without file or line.
- **Sampling and context switches** are Tracy's own and start with the process when the kernel lets it open `perf_event`s and read
  tracefs: a lowered `kernel.perf_event_paranoid` (`sudo sysctl kernel.perf_event_paranoid=-1`), and for context switches in practice running
  as root. Without that they are simply absent. `TRACY_NO_SAMPLING=1` and `TRACY_NO_CONTEXT_SWITCH=1` in the environment turn them off.
