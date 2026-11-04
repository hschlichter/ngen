# 61. Switch only when the finer level is complete; no frustum in the heuristic

Date: 2026-10-15

## Status

Accepted

Amended by [ADR 69](0069_lod_heuristics_and_dither_fade.md): the 20% band applies only where no `blendThresholds` are authored.
Amended by [ADR 70](0070_residency_from_streaming_radii.md): the drawn level falls back to the nearest ready coarser level, else a counted hole.

## Context

[plan_streaming_lod.md](../plan_streaming_lod.md) decision 6 first said a root switches as soon as its finer level's payload is in. On Caldera a
district's proxy vanished and thousands of models filled in over several seconds. Separately, USD's reference LOD query treats anything outside
the view as coarsest; screen size is zero behind the camera, so turning around unloaded the districts right next to it and loaded them again on
turning back.

## Decision

- A root keeps drawing its coarser level until everything the finer level needs is held — meshes, materials, textures and the coarsest levels of
  nested roots — and then switches in one frame.
- The drawn level is never finer than the one the heuristic wants.
- The heuristic measures distance and projected size only, with no frustum test.
- Levels set by hand win over the heuristic.

Rejected: switching as soon as the payload is in (pops of half-loaded content), USD's off-screen-is-coarsest rule (reloads on every turn), and
hysteresis by minimum time (adds latency).

## Consequences

- Easier: no pops of half-loaded content; a level appears whole.
- Easier: residency is stable while the camera turns.
- Harder: a level that never completes never shows; the streaming record counts what it waits for.
- Harder: this departs from USD's reference query, so parity checks and the `streaming` record report frustum membership (`inFrustum`) separately.

## Verification

- Sources: [plan_streaming_districts.md](../plan_streaming_districts.md) (decisions 4, 5, 7, 8),
  [plan_streaming_lod.md](../plan_streaming_lod.md) (decision 6, revised).
- Code: `src/view/placement.cpp:785-790` (`drawnLevelOf`, the switch rule); `src/view/viewscene.cpp:629` (`lodMeasure`, no frustum);
  `src/view/lodcommands.cpp:174-181` (`inFrustum` counted for the record).
