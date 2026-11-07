# 18. No project defaults: platform and config always required

Date: 2026-05-18

## Status

Superseded by [ADR 80](0080_build_without_targets_builds_everything.md)

## Context

The build framework is meant to be lifted into other projects ([ADR 11](0011_build_described_in_cpp.md)). Choosing a default platform or
configuration inside `src/build/` would leak this project's knowledge into it. Variants also live side by side on disk, so there is no single
"current" one for the build system to assume.

## Decision

- `ngen-build` requires both `-p` / `--platform` and `-c` / `--config` on every call.
- If either is missing, it prints the help and the project's listing (platforms, configurations, top-level targets) and exits with an error.
- The only project-level default is `default_target()`, declared in `build.cpp`.
- Convenience belongs above the build system: `ngen-cli` remembers a chosen variant ([ADR 33](0033_ngen_cli_front_door.md)).

## Consequences

- `src/build/` stays free of project knowledge.
- Every direct call spells the variant out, and so did every build-and-run pair; that friction is what `ngen-cli` was made to remove.

## Verification

- Sources: [plan_build_system_v2.md](../plan_build_system_v2.md) §7.5, [plan_ngen_cli.md](../plan_ngen_cli.md) "Current state",
  [src/build/README.md](../../src/build/README.md).
- Code:
  - `src/build/bootstrap.cpp:29-32`: both flags required, no defaults, the listing printed when either is missing.
  - `src/build/bootstrap.cpp:80-91`: flag parsing.
  - `build.cpp:586`: `p.default_target(view)`.
