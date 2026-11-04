# 49. Streaming and LOD are one system

Date: 2026-10-11

## Status

Accepted

Amended by [ADR 70](0070_residency_from_streaming_radii.md): residency comes from the authored streaming radii, and the state's level and every
coarser level stay resident, instead of only the coarsest as a floor.

## Context

Until this decision, everything a scene named was loaded at start-up and stayed loaded. Large worlds (Caldera, later the streaming test world)
need levels of detail and need to load and release content as the camera moves. LOD picks what to draw; streaming picks what is in memory.
Two systems computing nearly the same distances can disagree: LOD wants a level that isn't loaded, so something pops or a hole shows, or streaming
keeps a level LOD never shows. The roadmap's "everything is async" asks for no holes.

## Decision

- One evaluation per LOD root gives both outputs: the level to draw, and the levels to keep resident.
- Each root's heuristic gives a wanted level. Residency is derived from that, with the coarser levels kept as fallbacks.
- Drawing shows the finest resident level that is not finer than the wanted one, so a coarser level stands in while a finer one loads.

## Consequences

- Easier: one computation, and one place to look when a level is wrong; the coarse level doubles as the placeholder.
- Easier: LOD and residency can be shown side by side in one record and one window.
- Harder: holding coarser levels costs memory. Caldera's coarsest proxy map alone is 14.6 million points.
- The prefetch of the next finer level inside a band, and trimming residency by priority under a budget, were part of the plan and never built
  ([ADR 71](0071_requests_never_cancelled_no_budgets.md)).

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_streaming_lod.md](../plan_streaming_lod.md) (decision 1, locked 2026-10-11), [plan_streaming_domain.md](../plan_streaming_domain.md),
  [src/view/README.md](../../src/view/README.md).
- Code: `src/view/viewscene.cpp:811` (`evaluateLevels`: the imaging pick, then residency, then `setResidentLevels` at `:972`),
  `src/world/worldcomposer.h:50` (`setResidentLevels`).
- Differs from plan: wanted level and residency now come from two heuristics — the imaging domain and the authored `streaming` domain — evaluated in
  the same pass ([ADR 70](0070_residency_from_streaming_radii.md)). The "coarsest level is always the floor" rule became "the state's level and
  every coarser level". No prefetch margin and no budget code exist.
- Rejected alternatives: LOD alone decides residency (nothing is prefetched, so every switch shows a hole); a separate streaming system (duplicated
  work, and the two disagree).
