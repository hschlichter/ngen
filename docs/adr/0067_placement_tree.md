# 67. Streaming v2: a placement tree of immutable contents

Date: 2026-10-16

## Status

Accepted

Supersedes [ADR 59](0059_districts_streaming.md).

## Context

Districts streaming re-extracted the whole World on every change: the extract replayed every content and rebuilt every draw, 266,000 draws in
1.3 seconds. Readiness was a recursive walk, unloading had three timer-based mechanisms, and a cap with a ranking kept memory in check on Caldera.
With a world authored to stream ([ADR 65](0065_test_world_over_caldera.md)), most of that was no longer needed, and a level switch should cost in
proportion to what switched.

## Decision

- A content — a prototype or a LOD level — is linked only when it is complete, and then never changes.
- A placement is a content at a transform, with its inherited purpose and material binding; it owns the instance slots of its draws.
- A level switch removes one child placement's slots and adds another's. Nothing else in the tree changes.
- Readiness is a count of what each placement still waits for (`PlacementWait`), not a walk.
- A content is released when no resolve reaches it.
- Only LOD levels and prototypes are contents; a payload that isn't a level loads with its parent's content.
- The cap is removed; the heuristic runs over placed roots only.

Rejected: the whole-world extract with a diff per use, GPU-driven placement (deferred until meshlets), keeping gaps or payload contents, and a plain
cap as a safety valve.

## Consequences

- Easier: a switch costs what it changes; evaluating the heuristic takes 0.18 milliseconds (median).
- Easier: `viewscene.cpp` went from 2,116 lines to 1,164 at landing.
- Harder: `placement.*` is new code (about 2,000 lines today), and snapshots needed a copy-on-write `ChunkedList`.
- Harder: levels inside prototypes never unload, and material and texture packs are never released.

## Verification

- Sources: [plan_streaming_v2.md](../plan_streaming_v2.md), [plan_async_scene.md](../plan_async_scene.md) (decision 2, replaced here).
- Code: `src/view/placement.h:40` (`PlacementWait`), `:163` (`PlacedRoot`), `:178` (`Placement`); `src/view/README.md:97` ("The placement tree");
  `src/world/README.md:119-123`.
- Differs from plan:
  - Decision 2 B said to discard a partial content each round; the composer still recomposes a content with gaps in place, internally, and links
    it only when complete (`src/world/README.md:123`).
  - `viewscene.cpp` has since grown back to 1,621 lines and `placement.cpp`/`.h` total about 2,075.
