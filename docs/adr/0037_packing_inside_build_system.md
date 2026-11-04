# 37. Packing inside the build system (`pack_rule`)

Date: 2026-09-29

## Status

Superseded by [ADR 42](0042_separate_asset_server.md)

## Context

Shaders and other assets needed a packing step — turning a source file into engine-ready data — with stable ids, dependency tracking and
invalidation. The build runner (ADR 15) already had content hashing, Make-format depfiles and a build log, so reusing it looked cheapest.

An earlier plan, the cooked-asset cache with an in-process cooker (`src/assetsystem/`, `ngen-cook`), had been set aside: cooking inside the engine
would have pulled OpenUSD, shaderc and stb into it.

## Decision

- A generic `pack_rule(...)` type in the build framework (`src/build/framework/packrule.hpp`): globs and per-config parameters, declared in `build.cpp`.
- Pack jobs became runner edges (`pack:<id>`, flagged `kEdgeFlagPack`) in the IR, which went to format version 2.
- A static `pack("core")` target that ngen-view depended on, and a reverse index in `packs/.ngen-packdeps`.
- Each packer is its own program with a fixed command line (ADR 38); ids are paths (ADR 39).
- At landing, the pack container, the chunk types and in-build request "rounds" were dropped: a packer that finds another asset sends a new request
  instead of the build looping.

A separate rules file loaded by a server was rejected at the time, because it would split the project's description in two.

## Consequences

- Easier: it proved the packer contract — byte-identical SPIR-V for 51 of 51 shaders.
- Harder: every packed asset had to be listed statically in `build.cpp`, but assets are discovered at runtime, through scene references.
- Harder: packing was mixed into a build framework meant to stay project-agnostic, and the IR format carried pack concepts.
- Both were undone two days later: packing moved to `ngen-asset-server` (ADR 42), and IR format 3 removed `pack_rules`, `packs_root` and
  `kEdgeFlagPack`. The rule type survives, rewritten, in the asset system.

## Verification

- Sources: [plan_pack_rules.md](../plan_pack_rules.md) (Results, Deviations), [plan_asset_server.md](../plan_asset_server.md) (Current state, step 1),
  [plan_async_asset_system.md](../plan_async_asset_system.md) (the cooked-asset cache, never built).
- Code: `src/build/ir/schema.hpp:22` (`kFormatVersion = 3; // 2 carried pack rules, which left the build system`); no `packrule.hpp` under `src/build/`;
  the rule type is now `src/asset/pack/packrule.h`. `src/assetsystem/` never existed in any commit.
