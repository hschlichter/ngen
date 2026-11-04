# 70. Residency from the authored streaming radii, with no time

Date: 2026-10-30

## Status

Accepted

Supersedes [ADR 60](0060_time_based_unload_delay.md). Amends [ADR 49](0049_streaming_and_lod_one_system.md): every coarser level is resident, not
only the coarsest.

## Context

The regenerated test world authors a custom `streaming`-domain `LODDistanceHeuristic` on each streaming root: its thresholds are load radii, and a
custom `streaming:unloadThresholds` attribute holds the unload radii. The engine ignored that domain. It derived residency from the imaging pick plus
a 10-second unload clock, which produced orders like full, then proxy, then medium, and made two runs of one flight hold different packs.

## Decision

Decisions 1, 2 and 5 locked by Henrik on 2026-10-30:

- What is resident is a function of the camera position, the authored radii and each root's previous state (hysteresis). Nothing happens because
  time passed.
- The state's level and every coarser level are resident; no level is special by its position in the list.
- Drawing: the picked level if it is ready, else the nearest ready coarser level, else a hole that is counted.
- A pick finer than the streaming state is not loaded early; it draws late, and that is reported.
- The authoring contract — imaging threshold at most the load radius, load radius at most the unload radius, a child inside its parent — is
  checked and reported, never corrected.
- Time is only measured: lead, late draws and reloads.
- Levels set by hand are the one exception.

Of the options, a streaming heuristic of either kind (A) was chosen over distance only (B).

## Consequences

- Easier: residency is predictable, repeatable and authorable; a designer can say what is loaded where.
- Easier: drawing falls back in both directions without holes where anything coarser is ready.
- Harder: about 1.5 times the resident set, since every coarser level stays.
- Harder: latency is fixed only by authoring larger radii, or by budgets, which don't exist ([ADR 71](0071_requests_never_cancelled_no_budgets.md)).

## Verification

- Sources: [plan_streaming_domain.md](../plan_streaming_domain.md), [plan_streaming_tool.md](../plan_streaming_tool.md) (the Streaming window,
  map and overlays).
- Code: `src/world/world.cpp:261` (`streamingState`), `:286` (`checkLodAuthoring`); `src/world/worldcomposer.cpp:1210-1212` (unload thresholds
  read); `src/view/README.md:197` ("never by time"); `src/tools/streamingtool.*`, `src/ui/streamingwindow.*`.
- Differs from plan: `docs/roadmap.md` still says "the coarsest level always resident as the placeholder".
