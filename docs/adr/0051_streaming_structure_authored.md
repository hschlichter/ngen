# 51. The streaming structure is authored, not computed

Date: 2026-10-12

## Status

Accepted

## Context

A first version of the streaming plan, locked a day earlier, had the packer cut the world into a uniform XY grid of cell packs, each with a coarse
level computed at pack time. That is layout a designer can neither control nor predict. Its main motivation — one request per placement — turned out
to be one load per unique file: Caldera has 11,934 files against 311,765 placements.

Caldera's own structure — payloads, `extentsHint`, kinds, terrain tiles — is already authored with standard USD.

## Decision

- What streams is what the author made a payload. Bounds come from the authored `extentsHint`.
- The packer translates what is authored and never invents layout. No new schema is introduced.
- A scene authored as one big file is the author's choice, and streams as one unit.

## Consequences

- Easier: residency is predictable and visible to designers — what loads together is what they put together.
- Easier: the packer stays a translator, one USD file at a time ([ADR 52](0052_view_renders_layer_packs.md)).
- Harder: badly authored data streams badly, by design. Caldera's kilometre-wide districts are the example; the answer is an authoring fix, not engine
  code ([ADR 65](0065_test_world_over_caldera.md)).
- Harder: there is no automatic splitting of oversized content.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_streaming_lod.md](../plan_streaming_lod.md) (decision 3, "Replaced"), [test_streaming_world.md](../test_streaming_world.md),
  [src/world/README.md](../../src/world/README.md).
- Code: `src/world/worldcomposer.cpp:965` (a LOD root streams when one of its levels is a payload; otherwise its levels are composed in place),
  `src/world/worldcomposer.cpp:1189` (`extentsHint` is read into the payload placement).
- Differs from plan: since streaming v2 ([ADR 67](0067_placement_tree.md)) only payloads that are LOD levels stream on their own; any other payload
  loads with its parent's content, so its `extentsHint` is shown in the `payloads` record but decides nothing. The plan's "residency is load rules"
  (`UsdStageLoadRules`) was never built; residency is a set of levels per root (`setResidentLevels`).
- Rejected alternatives: a uniform XY grid of cell packs; per-cell coarse levels cut by the packer; the authored hierarchy alone; a quadtree.
