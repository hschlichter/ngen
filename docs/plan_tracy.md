# Tracy profiler integration

**Status. Landed.**

## Current state

Profiling is `src/profile/` ([ADR 23](adr/0023_profiling_markers.md), [plan_performance_window.md](plan_performance_window.md)): the macros
`PROFILE_ZONE`, `PROFILE_ZONE_VALUE`, `PROFILE_FRAME_MARK`, `PROFILE_GPU_ZONE`; a built-in collector with a lock-free ring per thread (16,384 zones
per lane, 1,024 frames); the Performance window; the `profile` and `zones` records and the `dump-profile` verb (`src/view/profilecommands.cpp`).
GPU zones are the RHI's own timestamp queries (`beginGpuZone`/`collectGpuZones`), moved onto the CPU clock by `Renderer::readGpuTimings` and
handed in through `profile::submitGpuZones`.

Every zone reaches the collector through `profile::beginZone`/`endZone`: the macro's `ScopedZone`, about ten sites that call them directly with a
pre-registered id (`renderer.cpp`, `viewframe.cpp`, `uihooks.cpp`, `view.cpp`), and job zones (`ScopedZone(job.profileNameId)` in `jobsystem.cpp`).
Threads name their lanes with `profile::registerThread` (Main, Render, Scene, Worker N).

What the built-in profiler does not give: history past a few seconds, statistics over a whole session, sampling and context switches, and a
timeline that can be searched and compared. That is what Tracy is for. ADR 23 shaped the API for "Tracy as a second sink" and records that the
shape is untested. The profile library has no README yet.

Tracy 0.14.1 is installed on the development machine from the distribution (`tracy-profiler`, `tracy-capture`, `tracy-csvexport`).

## Goals

1. Everything the engine already profiles shows up in Tracy — CPU zones with their values, thread names, frames, the GPU lane — without changing a
   single call site.
2. **The built-in profiler stays the source of every record and window.** Nothing in the engine reads data back from Tracy; every number
   available today (the `profile` and `zones` records, `dump-profile`, the Performance window) stays available without Tracy, and in gamerelease.
   Tracy adds detail on top; it replaces nothing.
3. Free when nobody is looking: no cost beyond a branch per zone until a Tracy server connects.
4. A small change: one new library and the sink inside `profile.cpp`. No renderer, RHI, trace or call-site changes.

## Decisions

Locked; the alternatives are kept as the record of what was weighed.

1. **The Tracy calls sit inside the collector's functions.** `beginZone`, `endZone`, `zoneValue`, `frameMark`, `registerThread` and
   `submitGpuZones` in `profile.cpp` also call Tracy's C API (`TracyC.h`); `registerName` builds one static source location per interned name.
   Tracy stays in one `.cpp` and every existing zone reaches it. Cost: a zone's source location in Tracy is its name only (no file or line).
   Weighed: Tracy macros inside `PROFILE_ZONE` (file and line, but a second path for the direct `beginZone` sites and job zones, and Tracy's
   header in every file that profiles); call-site locations on top of this (Deferred).
2. **GPU zones are fed from `submitGpuZones`.** The zones the renderer already collects and calibrates go to a Tracy GPU context through the C
   API's serial GPU events. One set of timestamp queries, no Tracy in `src/rhi`. Cost: Tracy records the CPU side of a GPU zone when the zones
   are collected (about two frames late), so its "CPU to GPU delay" for GPU zones is meaningless; their place on the GPU lane is right.
   Weighed: `TracyVkContextCalibrated` in the Vulkan backend (a second set of queries owned by Tracy, a per-frame `TracyVkCollect`, Tracy
   headers in the RHI).
3. **Compiled into debug and release, on demand.** `TRACY_ENABLE`, `TRACY_ON_DEMAND` and `TRACY_ONLY_LOCALHOST` defined by the debug and release
   configurations. The client listens on loopback (like the RPC) and records nothing until a server connects. In gamerelease
   `TracyClient.cpp` compiles to nothing and the sink is `#ifdef`'d out. Weighed: a separate `profile` configuration (a third variant to build);
   starting Tracy at runtime from a config value (Deferred).
4. **Client from a submodule pinned to the installed tools' tag; tools from the distribution.** `external/tracy` at `v0.14.1`; Tracy's client
   and server must speak the same protocol, so the submodule follows the distribution's version; `tracy-capture` and `tracy-profiler` report a
   protocol mismatch when they connect. Weighed: building the tools from the submodule (a CMake build outside `ngen-build`); linking the
   distribution's client library (not vendored, moves under the build).
5. **Zones only; no trace events as Tracy messages.** Planned first (the event's text sent to Tracy where it enters the trace's ring), dropped
   while building: the trace library is linked by `ngen-asset-server` too, so a Tracy call in it would have made the asset server a Tracy
   client as well, and the trace is sparse by design (a few dozen events per run, mostly start-up), so the messages would have added little.
   Plots, locks, memory and the asset server are deferred.

## Scope

**In**

- `external/tracy` submodule at `v0.14.1`; a `tracy` static library in `build.cpp`; Tracy defines in the debug and release configurations;
  `tracy` linked into every program that links `profile` (static libraries are not passed on transitively).
- The Tracy sink inside `src/profile/profile.cpp`: CPU zones, zone values, frame marks, thread names, the GPU context and its zones.
- `src/profile/README.md` (new). Root `README.md` links it. Agents keep reading profile data from the `profile` and `zones` records and
  `dump-profile`; Tracy's headless tools get one mention in the profile README, no tooling of their own.
- On landing: an ADR "Tracy as a second sink inside the collector" (completes ADR 23, which stays Accepted).

**Out**

- Changing any call site, the renderer, the RHI, the built-in collector, the Performance window, or the `profile` and `zones` records.
- Showing Tracy's state in the engine's UI or records: Tracy has its own window, which lists the clients and their connection.
- Reading anything back from Tracy into the engine.
- Tracy in gamerelease.
- Starting or stopping a Tracy capture over the engine's RPC: Tracy's own tools are the controller.
- See Deferred.

## Steps

1. **Vendor and build.** `git submodule add https://github.com/wolfpld/tracy external/tracy`, checked out at `v0.14.1`. In `build.cpp`:

   ```cpp
   // Tracy's client: empty unless the configuration defines TRACY_ENABLE (src/profile/README.md).
   auto tracy =
       cxx::static_library("tracy")
           .sources({"external/tracy/public/TracyClient.cpp"})
           .public_include({"external/tracy/public"});
   ```

   `debug` and `release` add `.define("TRACY_ENABLE")`, `.define("TRACY_ON_DEMAND")` and `.define("TRACY_ONLY_LOCALHOST")` beside
   `NGEN_INTROSPECTION`: defined at the configuration, so every file that includes a Tracy header agrees on them. `profile` links `tracy` (its
   include path); each program that links `profile` also links `tracy`. Vendored warnings are turned off per source, as with imgui.

2. **CPU sink in `profile.cpp`.** All under `#ifdef TRACY_ENABLE`:

   ```cpp
   constexpr uint32_t maxNames = 4096;
   // One per interned name, written before registerName returns its id, so beginZone reads it without a lock.
   std::array<___tracy_source_location_data, maxNames> g_tracySourceLocations;

   struct ThreadState {
       // ... as today
       std::array<TracyCZoneCtx, maxDepth> tracyZones;
   };
   ```

   - `registerName`: on a new name, fills `g_tracySourceLocations[id]` with the name as `.name` and `.function`, an empty file, line 0. The name
     pointer is the caller's static string, the contract `registerName` already has. Past `maxNames` it asserts.
   - `beginZone`: below `maxDepth`, `t_state.tracyZones[depth] = ___tracy_emit_zone_begin(&g_tracySourceLocations[nameId], 1)`.
   - `zoneValue`: `___tracy_emit_zone_value`. `endZone`: `___tracy_emit_zone_end`, before the pause check — pausing the Performance window does
     not pause Tracy.
   - `frameMark`: `___tracy_emit_frame_mark(nullptr)`, before the pause check.
   - `registerThread`: `___tracy_set_thread_name(name)`.

3. **GPU sink in `submitGpuZones`.** Before the pause check and before the 64-zones-per-frame cut, so Tracy gets every zone the RHI recorded. On
   the first call, a context named "GPU" whose clock is the profile clock: `gpuTime = profile::now()`, period 1 ns, type `Custom` (Tracy keeps it
   and re-sends it to a server that connects later). While a server is connected, each frame first sends a `___tracy_emit_gpu_time_sync_serial`
   with `profile::now()`, re-anchoring the profile clock (`CLOCK_MONOTONIC`) against Tracy's (the TSC) so drift never builds up, then the zones in
   start order, opened and closed by depth: per zone an allocated source location (`___tracy_alloc_srcloc_name`), a begin with query id `q`, an
   end with `q + 1`, and the two times (`___tracy_emit_gpu_*_serial`); query ids wrap at 16 bits. The renderer is unchanged. (Built: a sync every
   frame instead of every 120, which is simpler and costs one event.)

4. **Docs.** `src/profile/README.md` (the markers, the collector, the GPU lane, the Tracy sink, where each sits in the frame, using Tracy
   interactively and with `tracy-capture`, and `kernel.perf_event_paranoid` for Tracy's sampling), the root README link, and the
   `docs/README.md` and `docs/roadmap.md` entries.

## Visibility

- **Records, RPC methods, verbs and windows**: none added or changed. Tracy has its own window (`tracy-profiler`, which also lists the running
  clients and whether one is connected) and its own headless tools (`tracy-capture`, `tracy-csvexport`). The engine's profile data stays where it
  is, from the built-in collector, with or without Tracy: the `profile` and `zones` records, `dump-profile` and the Performance window.
- **Where it sits in the flow** (profile README): CPU zones and frame marks reach Tracy at the same moment they reach the ring, on the thread that
  ran them; GPU zones on the render thread after the slot's fence, when `Renderer::readGpuTimings` hands them to `submitGpuZones`.

## Verification

- No Tracy in gamerelease: the binary is stripped, so `strings _out/linux-vulkan/gamerelease/ngen-view | rg -ci tracy` is 0; `nm` on the debug
  and release binaries finds `___tracy` symbols. **Verified** (0 in gamerelease, 87 symbols in debug).
- Cost without a server. The plan's first criterion, Sponza's `cpuMsAverage` within 2%, cannot show it: Sponza is GPU-bound (CPU and GPU
  frame both 3.99 ms) and its CPU work is about 0.3 ms per frame with run-to-run noise around 10%. Measured instead: a loop of `PROFILE_ZONE`
  built from `profile.cpp` with and without the Tracy defines. **Verified**: 37 ns per zone without Tracy, 67 ns with Tracy and no server
  (three runs, one outlier at 90 ns); Sponza runs 74 CPU zones per frame, about 2 µs per frame, 0.06% of its frame.
- Headless capture: `tracy-capture -o <scratch>/run.tracy -f` started first, ngen-view run with `TRACY_NO_EXIT=1` and a script that records
  `profile` and `zones` at frame 300 and quits. The trace has no warnings or errors.
  `tracy-csvexport run.tracy` lists every CPU zone name the `zones` record lists, `tracy-csvexport -g run.tracy` every GPU zone name.
  **Verified** on three_cubes in release: no warnings or errors, all 74 CPU and 40 GPU zone names of the `zones` record in the capture (Tracy
  has more: the whole run, not the last 60 frames).
- GPU placement: in the capture, each frame's first GPU zone starts after the render thread's `Submit` zone of that frame (from the `-u` and
  `-g` exports). **Verified**: no GPU zone before the first `Submit`; from each `Submit`'s start to the next GPU zone 0 to 745 µs, median 90 µs.
- Connect late: a `tracy-capture` started 10 seconds after the view still has the GPU lane and the thread names. **Verified** for the GPU lane
  (75,000 GPU zones in 3 s, placement as above); thread names are in Henrik's visual check, `tracy-csvexport` does not export them.
- Henrik, visual: `tracy-profiler` lists the running view; the capture shows Main, Render, Scene and the workers by name, the GPU lane, frames,
  zone values.
- Example sweep unchanged (`examples` with `--check --validation`). **Verified**: all 17 exit 0.

## Open questions

- Answered: on-demand mode starts Tracy's sampling and context switches with the process (`SpawnWorkerThreads`), not on connect, when the
  kernel allows them. On this machine (`perf_event_paranoid` 2, not root) they don't start; the cost above is without them. `TRACY_NO_SAMPLING=1`
  turns sampling off if it is ever in the way.

## Deferred / follow-ups

- **Call-site source locations** — `PROFILE_ZONE` passes `__FILE__`, `__LINE__` and the function to a per-site registration. Trigger: wanting
  Tracy's source view on a zone.
- **Plots** — a `PROFILE_PLOT(name, value)` that also feeds a built-in plot, so the data stays reachable without Tracy. Trigger: a counter worth
  watching over time, such as upload bytes or resident memory.
- **Locks** — `TracyLockable` on the asset client, the render thread's hand-off and the job queue. Trigger: a stall that zones show but do not
  explain.
- **Memory** — allocation tracking through Tracy's hooks. Trigger: a memory growth question.
- **Asset server** — `profile` and Tracy in `ngen-asset-server` (a second client on the next port). Trigger: a packing or serving time question.
- **Runtime start** — `TRACY_MANUAL_LIFETIME` behind a config value. Trigger: the listener's cost or its port shows up as a problem.
