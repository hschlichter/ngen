# 71. Requests are never cancelled; budgets set aside

Date: 2026-10-30

## Status

Accepted

## Context

[plan_streaming_lod.md](../plan_streaming_lod.md) (2026-10-12) planned per-request priorities, memory budgets with eviction by priority, and texture
mip streaming. The asset server packs and caches what it is asked for ([ADR 42](0042_separate_asset_server.md)); cancelling a request would throw away
finished or nearly finished work that the next visit wants again. When residency moved to authored radii
([ADR 70](0070_residency_from_streaming_radii.md)), a level could stop being wanted while its packs were still on the way.
[plan_streaming_budgets.md](../plan_streaming_budgets.md) designed GPU and CPU budgets, but no measured run ran short of memory.

## Decision

- Every wanted asset is requested, and a request is never cancelled.
- An answer that arrives for a level no longer resident is not placed; it is counted (`arrivedUnwanted` in the streaming record).
- Memory is not bounded until a need shows. Budgets, priorities and eviction are set aside (Henrik, 2026-10-31: no observed need, no memory
  target).

Deferred: cancelling in the asset client and server, decided by the `arrivedUnwanted` count; budgets, decided by a run that runs short of memory.

## Consequences

- Easier: simple request handling, and the server's cache stays warm for the camera coming back.
- Easier: the count shows whether cancelling would be worth building.
- Harder: nothing bounds memory; a world authored with large radii can run out.
- Harder: layer packs can queue behind meshes, since requests have no priority.

## Verification

- Sources: [plan_streaming_lod.md](../plan_streaming_lod.md) (decisions 4, 7), [plan_streaming_domain.md](../plan_streaming_domain.md)
  (decision 3), [plan_streaming_budgets.md](../plan_streaming_budgets.md) (Draft, set aside), [roadmap.md](../roadmap.md).
- Code: `src/view/viewscene.cpp:497` (`arrivedUnwanted`); no priority or cancel in `src/asset`.
- Differs from plan: plan_streaming_lod's decisions 4 (budgets, eviction with time hysteresis) and 7 (priorities), and texture mip streaming,
  were never built; that plan still reads "In progress".
