# 82. Detailed profiling: sites, history by time, profile as a foundation library

Date: 2026-11-09

## Status

Accepted

Extends [ADR 23](0023_profiling_markers.md) and [ADR 81](0081_tracy_as_second_sink.md), which stay Accepted; changes when ADR 81's Tracy client
starts.

## Context

With Tracy in place ([ADR 81](0081_tracy_as_second_sink.md)), a capture showed too little: about 100 zones sized for a guide, named without
colour or source, a fixed ring of 16,384 zones per thread, the RPC, asset client and trace threads invisible, and only ngen-view profiled. Henrik
wanted to see, a few levels down, where the time goes on the CPU and the GPU, find the code from a zone, and see every thread; the built-in
Performance window stays a guide, but everything goes through the profile system and stays readable by agents without Tracy.

## Decision

Locked with Henrik on 2026-11-09:

- **A zone is a site**: its name, file, line and function (`std::source_location`), a group and a level. Groups (Frame, Scene, Assets, Jobs,
  Ui, Tools, Render, Culling, Geometry, Shadow, Lighting, Post, Upload, Io, Wait) are the colour in Tracy and the Performance window; levels
  (guide, detail) are what the window shows by default. Functions that register for their caller (`FrameGraph::addPass`, `JobSystem::submit`,
  `RpcRegistry::add`) take the caller's location. A GPU zone's site travels through the RHI as a tag the RHI keeps and does not read.
- **History by time**: each thread's zones in blocks dropped by age, `profile.history.seconds` (default 10). Frames and GPU frames too, with no
  per-frame cap. The collector keeps everything, detail included; the `zones` record gives each site's self time and the share its children
  cover, and `dump-profile` takes seconds and a level.
- **How deep**: a zone of 0.05 ms or more per frame on the main and render threads, or 1 ms or more per call elsewhere, has children covering
  80% of it, down to a function that does one thing or a library or driver call. No zones per item in large loops: a loop over items is a
  leaf, and a recursion is zoned by structure (the placement tree per stage and LOD level).
- **On the GPU, the pass and its draws are the leaves**: timestamps cannot split a draw. Draws are named for what they draw (`Opaque`,
  `Opaque two-sided`, `Cutout`, `Cutout two-sided`), cascades are zones. Finer GPU zones are behind `profile.gpu.detail`, off by default,
  because a timestamp makes the GPU wait.
- **Every thread visible**: every thread a program starts names itself, and idle waits are 100 ms slices, each a zone.
- **`profile` is a foundation library**: `rpccore`, `trace` and `packer` link it, so every program does. The asset server answers the profile
  records and dump; the packers are instrumented.
- **Tracy's client starts by hand** (`TRACY_MANUAL_LIFETIME`, `TRACY_DELAYED_INIT`): its start-up sleeps 200 ms and its shutdown takes as long,
  so ngen-view and the asset server start it on a background thread and short-lived programs only when `NGEN_TRACY=1` is set.

Rejected: zones routed to Tracy only (the data would not be reachable without Tracy); a fixed history in zone counts (the seconds covered would
change with the detail); a thread-start hook in `rpccore` and `trace` instead of linking `profile` (names, but nothing inside the loops, and the
tools unprofiled); a zone count as the measure of detail (it measured the wrong thing; coverage does); zones per item (tens of millions per
flight, an 11 GB dump); Tracy started in every process (a packer run went from 1.2 ms to 350 ms).

## Consequences

- Easier: a zone leads to its line in Tracy and in the records; where the time goes is accounted for down to loops and library calls, on every
  thread, in the view, the asset server and the packers.
- Easier: gaps show without Tracy (`selfMs`, `coveredShare` in the `zones` record); the findings so far (two per-frame loops over every instance,
  a whole-World walk for a trace message, a contended mutex in the asset server) came from it.
- Harder: every program carries the profiler and Tracy's client; Tracy's on-demand mode drops a short-lived program's zones begun before a
  server connects.
- Harder: zones need the discipline of the rule — a zone per item floods the history and Tracy.

## Verification

- Sources: [plan_profile_detail.md](../plan_profile_detail.md) (both rounds), `src/profile/README.md`.
- Code:
  - `src/profile/profile.h:36` (`Group`), `:56` (`Level`), `:33` (`idleSlice`), `:192` (`ScopedZone` with `active`).
  - `src/profile/profile.cpp:398` (`registerSite`), `:306` (`startBlock`, blocks by age), `:261` (`laneForThread`, lanes reused by name),
    `:346` (`startTracy`), `:466` (`registerThread`).
  - `src/rhi/rhicommandbuffer.h:47` (`beginGpuZone(name, tag)`); `build.cpp:70`, `:84` (the Tracy defines).
- Differs from plan: round 1 measured detail by zone count (5× today's), which round 2 replaced with the coverage rule; per-item zones were
  removed after the 11 GB dump; Tracy's manual start was decided while building.
