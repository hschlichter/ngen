# 81. Tracy as a second sink inside the profile collector

Date: 2026-11-09

## Status

Accepted

Completes [ADR 23](0023_profiling_markers.md), which stays Accepted.

## Context

The built-in profiler ([ADR 23](0023_profiling_markers.md)) keeps a few seconds of zones per thread and the GPU lane, enough for the Performance
window and the `profile` and `zones` records, but not for a whole session, statistics and histograms per zone, search, sampling or context
switches. ADR 23 shaped the markers for "Tracy as a second sink" and recorded that the shape was untested. Every zone already reaches the collector
through `beginZone`/`endZone` (the macros, about ten direct call sites, the job system), and GPU zones arrive on the CPU clock through
`submitGpuZones`.

## Decision

Locked by Henrik on 2026-11-09:

- Tracy is fed from inside the collector's functions (`beginZone`, `endZone`, `zoneValue`, `frameMark`, `registerThread`, `submitGpuZones`) through
  Tracy's C API, with one static source location per interned name. Tracy stays inside `profile.cpp`; no call site changes.
- GPU zones go to a Tracy GPU context whose clock is the profile clock, from the zones the renderer already collects and calibrates, with a time
  sync every frame. One set of timestamp queries; no Tracy in the RHI.
- Debug and release define `TRACY_ENABLE`, `TRACY_ON_DEMAND` and `TRACY_ONLY_LOCALHOST`; gamerelease does not, and Tracy's client compiles to
  nothing there.
- The client is the `external/tracy` submodule, pinned to the tag of the Tracy tools installed from the distribution (0.14.1).
- The built-in collector stays the source of every record and window. Nothing reads data back from Tracy, and the engine shows no Tracy state:
  Tracy's own window lists its clients and their connection.

Rejected: Tracy macros inside `PROFILE_ZONE` (file and line, but a second path for the direct call sites and Tracy's header in every file that
profiles); `TracyVkContext` in the Vulkan backend (a second set of queries owned by Tracy, Tracy inside the RHI); a separate `profile`
configuration (a third variant to build); building Tracy's tools from the submodule (a CMake build outside `ngen-build`); trace events as Tracy
messages (the trace library is linked by the asset server too, which would have become a Tracy client, and the trace is sparse by design).

## Consequences

- Easier: whole sessions, statistics, search and comparison in Tracy, with every existing zone and the GPU lane, and nothing new to instrument.
- Easier: agents keep reading profile numbers from records; `tracy-capture` and `tracy-csvexport` reach Tracy's detail headless when needed.
- Harder: a zone costs about 30 ns more in debug and release with no server connected (37 ns to 67 ns; about 2 µs per Sponza frame).
- Harder: zones in Tracy carry their name only, no file or line; Tracy's "CPU to GPU" delay for GPU zones is meaningless, because they are sent
  when collected, about two frames after they were recorded.
- Harder: the submodule must follow the distribution's Tracy version, or the tools refuse the connection.

## Verification

- Sources: [plan_tracy.md](../plan_tracy.md), `src/profile/README.md` ("Tracy").
- Code:
  - `src/profile/profile.cpp:71` (`sendGpuZonesToTracy`), `:168` (`registerName`, the source locations), `:208` (`beginZone`), `:237`
    (`endZone`), `:302` (`submitGpuZones`).
  - `build.cpp:66`, `:78` (the defines in debug and release), `:102` (the `tracy` library).
- Differs from plan: the GPU time sync runs every frame instead of every 120 frames; trace events as Tracy messages were dropped; the cost
  criterion was measured per zone, because Sponza is GPU-bound and its frame time could not show it.
