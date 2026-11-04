# 60. A time-based unload delay

Date: 2026-10-15

## Status

Superseded by [ADR 70](0070_residency_from_streaming_radii.md)

## Context

With districts streaming ([ADR 59](0059_districts_streaming.md)), a camera hovering at a threshold, or turning around, made levels load and unload
over and over. [plan_streaming_lod.md](../plan_streaming_lod.md) had already proposed that a loaded payload stays at least some seconds. A minimum
time between level switches had been rejected in favour of hysteresis, but unloading still needed something to stop thrash.

## Decision

A level that is no longer wanted stays loaded for a delay (10 seconds, set with the `lod-unload-delay` verb), then is released. The coarsest
level is never released. Streaming v2 step 5 (2026-10-23) moved the delay onto the level (`keepLodLevels`).

Rejected at the time: never unloading.

## Consequences

- Easier: no thrash at a threshold or when turning around.
- Harder: what was resident depended on the clock, so two runs of the same flight held different packs.
- Harder: an authored middle level was resident only by chance, when the camera had passed it within the delay.
- Harder: the delay was the one place where time decided residency. ADR 70 replaced it with authored unload radii and hysteresis by position;
  keeping what the camera may come back to was left to budgets ([ADR 71](0071_requests_never_cancelled_no_budgets.md)).

## Verification

- Sources: [plan_streaming_districts.md](../plan_streaming_districts.md) (decision 6), [plan_streaming_v2.md](../plan_streaming_v2.md) (step 5),
  [plan_streaming_domain.md](../plan_streaming_domain.md) (decision 1).
- Code: `unloadDelay`, `keepLodLevels` and `levelsKept` are absent from `src/`; `src/view/README.md:197` states residency is decided "never by
  time".
