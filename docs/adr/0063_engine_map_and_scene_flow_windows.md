# 63. Engine Map and Scene Flow windows

Date: 2026-10-15

## Status

Superseded by [ADR 62](0062_runtime_access_and_visibility.md) (its 2026-10-31 form: where a part sits in the flow is written in its library's
README) and by the `scenethread`, `gpuscene` and `assets` records.

## Context

The visibility rule ([ADR 62](0062_runtime_access_and_visibility.md)) asked that every part of the engine show where it sits in the engine's flow.
Nothing in the UI showed the engine as a whole: how a scene gets from the asset server to the screen, or which thread does what.

## Decision

- An Engine Map window: the engine's parts as nodes and edges with live numbers, backed by a `map` record.
- A Scene Flow window: how a scene reaches the screen, with the upload budget and geometry pool controls.
- `AGENTS.md` required every new part of the engine to get a node in the Engine Map. Other windows linked to it (`drawEngineMapLink`).

Removed on 2026-10-31, as step 0 of the lean pass: both windows, the `map` record, `engineMapStats`, the cross-links and the frame-loop syncs that
fed them — 1,195 lines removed, 22 added. Henrik's reason was that they "aren't what they should be"; they were removed first so later steps
would not move code that was going. Their controls became configuration values ([ADR 74](0074_configuration_values.md)).

## Consequences

- Easier, while they existed: one picture of the engine's flow with live numbers.
- Harder, while they existed: every new part needed a node, and the view's commands included a UI header to feed them.
- After removal: less coupling, and the flow is described in prose in each library's README.
- After removal: there is no graphical overview of the engine's flow.

## Verification

- Sources: [plan_lean_pass.md](../plan_lean_pass.md) (step 0), [plan_streaming_tool.md](../plan_streaming_tool.md) (the Residency node),
  [roadmap.md](../roadmap.md).
- Code: no match for "engine map" or "scene flow" in `src/`, `AGENTS.md` or `.claude/`.
- Differs from plan: `docs/roadmap.md:31` and `:210` still mention Engine Map nodes.
