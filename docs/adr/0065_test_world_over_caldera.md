# 65. A test world authored to stream, over fitting Caldera

Date: 2026-10-16

## Status

Accepted

## Context

Districts streaming ([ADR 59](0059_districts_streaming.md)) was built and measured on Caldera. Most of its machinery answered problems specific to
that dataset: districts a kilometre wide, 159,000 payload prims per level, levels hidden inside instanceable assets. Bending the engine around one
dataset made every system larger and harder to reason about, and it left no way to tell an engine problem from an authoring problem.

## Decision

- A generated USD world written to best practice, with no knowledge of ngen, is the scene streaming is built and verified on: 1 km regions,
  250 m tiles, bounded LOD levels, variant sets and `PointInstancer`.
- The engine adapts to what that world authors and never changes the world to suit the engine. Where the world is wrong, the generator is fixed.
- Mechanisms that exist only for Caldera are removed outright, not kept behind a flag.
- Whatever standard USD the world uses, the engine reads as USD does: variant sets as Pcp composes them, `PointInstancer` as a World component
  ([ADR 66](0066_variants_and_pointinstancer.md)).
- Caldera is revisited later, through authoring fixes (its converter), not engine changes.

Rejected: keeping Caldera's mechanisms behind flags.

## Consequences

- Easier: much smaller systems. The cap, its ranking, paced extracts and payload contents went away with streaming v2
  ([ADR 67](0067_placement_tree.md)).
- Easier: an engine change can be verified on a world whose authoring is known to be right.
- Harder: Caldera runs out of memory with the LOD heuristic on until it is revisited; it currently runs with the heuristic off. This was accepted
  in streaming v2 decision 7.
- Harder: the generator is one more thing to maintain; the world was regenerated on 2026-10-29 to author the streaming domain
  ([ADR 70](0070_residency_from_streaming_radii.md)).

## Verification

- Sources: [test_streaming_world.md](../test_streaming_world.md), [plan_streaming_v2.md](../plan_streaming_v2.md),
  [plan_variants_pointinstancer.md](../plan_variants_pointinstancer.md), [plan_streaming_domain.md](../plan_streaming_domain.md).
- Code: `src/world/README.md:119` ("Only LOD levels and prototypes are contents"); the Caldera cap and ranking symbols are absent from `src/`.
- Differs from plan: v2's deferred item "Caldera revisited" is still open.
