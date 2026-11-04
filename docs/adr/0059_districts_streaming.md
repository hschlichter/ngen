# 59. Districts streaming: kept contents and a whole-world extract

Date: 2026-10-15

## Status

Superseded by [ADR 67](0067_placement_tree.md)

The time-based unload that came with it has its own record, [ADR 60](0060_time_based_unload_delay.md). The cache of composed contents survives
into ADR 67.

## Context

Before this, the whole scene loaded at start-up and stayed loaded, and every change recomposed and re-extracted the whole World: 48 seconds for one
Caldera district. Nothing was ever released. For streaming ([ADR 49](0049_streaming_and_lod_one_system.md)) the camera had to drive which level
each LOD root shows, and a switch had to cost far less than a full recompose.

## Decision

- The screen-size heuristic runs on the scene thread from the latest camera the main thread posted.
- Composed contents (prototypes, LOD levels, and later every payload) are cached across composes instead of recomposed.
- Draws are kept per content and replayed per use, over a whole-world extract.
- A 20% hysteresis band; levels set by hand win over the heuristic; no frustum test in the heuristic
  ([ADR 61](0061_switch_when_complete.md)).
- A level no longer wanted stays loaded for 10 seconds ([ADR 60](0060_time_based_unload_delay.md)).
- After an out-of-memory crash (10.8 GB, 35 roots at a finer level at once), a cap of 2 roots finer than their coarsest, ranked with the roots
  containing the camera first.

Rejected: general incremental composition in the style of Pcp (too much work and risk), keeping full recomposition (48 seconds), and keeping the
full walk but making it faster (it cannot get to milliseconds).

## Consequences

- Easier: a Caldera flight was repeatable and had no holes.
- Harder: a switch from cached content still took 1.85 seconds, against a target of under one second.
- Harder: much of the machinery existed only for Caldera — gaps, paced extracts, `TransformIndex`, the cap and its ranking. That is what
  [ADR 65](0065_test_world_over_caldera.md) and ADR 67 removed.

## Verification

- Sources: [plan_streaming_districts.md](../plan_streaming_districts.md), [plan_streaming_v2.md](../plan_streaming_v2.md) ("Current state").
- Code: the removed symbols (`heuristicCap`, `aroundCamera`, `TransformIndex`, `requestDrawnAssets`, `contentReady`, `unloadDelay`,
  `keepLodLevels`) are absent from `src/world`, `src/view`, `src/tools` and `src/renderer`. The kept contents survive:
  `src/world/README.md:123` ("Composed content is kept across composes").
