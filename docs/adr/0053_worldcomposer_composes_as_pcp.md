# 53. `WorldComposer` composes packs as USD's Pcp does

Date: 2026-10-12

## Status

Accepted

## Context

Without pxr in the view ([ADR 52](0052_view_renders_layer_packs.md)), the view still had to arrive at the same prims, types and opinions a USD stage
would. Caldera fully expanded would be tens of millions of entities. And correctness had to be checkable from this repository, against OpenUSD
itself, rather than by eye.

## Decision

- `src/world/` holds the runtime scene: `World`, its contents and `Entity` with components (lights, LOD roots, point instancers, payload
  placements).
- `WorldComposer` composes layer packs following Pcp's prim indexing: node trees, strength order, namespace mapping, `defaultPrim`, and instancing
  as `Pcp_ChildNodeIsInstanceable`.
- Every composed prim is an entity, whatever its type or purpose. What is drawn is decided when the World is read: purpose `default` and `render`
  always, `guide` and `proxy` on a toggle.
- Instanced content is kept once, as a prototype shared by every instance. Per-instance state (LOD level, transform) lives outside the prototype.
- Each scene keeps its own up axis and `metersPerUnit`; nothing is converted. Engine defaults (the fallback distant light) are added by the view, not
  the pack.
- `ngen-worldcheck` (from packs) and `ngen-usdcheck` (from OpenUSD) print fingerprints that must match.

## Consequences

- Easier: composition is verifiable prim for prim against OpenUSD.
- Easier: memory scales with unique content, not placements, so Caldera fits.
- Harder: a re-implementation of Pcp to maintain.
- Harder: gaps — inherits, specializes, relocates and layer offsets are not composed.
- Harder: a divergence from USD — a prototype loads every level any of its instances wants, where USD gives instances with different loaded levels
  different prototypes.

## Verification

Checked against `d83048c` on 2026-11-04.

- Sources: [plan_scene_pack.md](../plan_scene_pack.md) (decisions 5, 10, 15 to 21), [usd_payloads_and_variants.md](../usd_payloads_and_variants.md),
  [src/world/README.md](../../src/world/README.md).
- Code: `src/world/worldcomposer.h:26` (`WorldComposer`), `src/world/world.h:18` (every prim an entity, instanced content kept once),
  `src/world/world.h:24` (`Purpose`), `src/world/world.h:277` (`upAxis` and `metersPerUnit` with USD's fallbacks), `src/world/fingerprint.h`,
  `build.cpp:387` (`ngen-worldcheck`) and `:389` (`ngen-usdcheck`), `src/view/placement.cpp:1747` (the view adds `/__DefaultEngineLight`).
- Differs from plan: variant sets were "Out" in the plan and were added later ([ADR 66](0066_variants_and_pointinstancer.md)).
- Rejected alternatives: expanding instances (memory); one engine frame the packer converts everything into (needs root-layer metadata that
  per-file jobs don't have); verifying with the Caldera fork's Python script (a dependency on another repository).
